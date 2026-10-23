// VRM / glTF loader. Geometry, skins and textures come from cgltf; the VRM
// extensions (VRMC_vrm, VRMC_materials_mtoon, and the VRM 0.x "VRM" block) are
// read from the GLB JSON chunk with nlohmann::json.
#include "engine/texture_mips.h"
#include "engine/vfs.h"
#include "engine/vfs_cgltf.h"
#include "engine/model.h"
#include "engine/pmx_glb.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "meshoptimizer.h"

#include "cgltf.h"
#include "engine/rig.h"
#include "stb_image.h"

namespace ukiyo {
namespace {

using json = nlohmann::json;

const json kNull = nullptr;

// Safe member access on a const json: returns null when the key is missing.
const json& jget(const json& j, const char* key) {
    if (!j.is_object()) return kNull;
    auto it = j.find(key);
    return it == j.end() ? kNull : *it;
}

json jsonFromBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 20 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        uint32_t jsonLen = 0;
        std::memcpy(&jsonLen, bytes.data() + 12, 4);
        if (20u + jsonLen > bytes.size()) throw std::runtime_error("truncated GLB JSON chunk");
        return json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLen);
    }
    return json::parse(bytes.begin(), bytes.end());
}

json readJson(const std::string& path) {
    std::vector<uint8_t> bytes;
    vfs::readBytes(path, bytes);
    return jsonFromBytes(bytes);
}

float numOr(const json& j, const char* key, float def) {
    const json& v = jget(j, key);
    return v.is_number() ? v.get<float>() : def;
}

glm::vec3 vec3Or(const json& j, const char* key, glm::vec3 def) {
    const json& v = jget(j, key);
    if (!v.is_array() || v.size() < 3) return def;
    return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
}

glm::vec3 vec3Obj(const json& v, glm::vec3 def) {
    if (v.is_array() && v.size() >= 3) return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    if (v.is_object() && v.contains("x") && v.contains("y") && v.contains("z"))
        return {v["x"].get<float>(), v["y"].get<float>(), v["z"].get<float>()};
    return def;
}

std::vector<int> intArray(const json& v) {
    std::vector<int> out;
    if (v.is_array())
        for (const auto& e : v) if (e.is_number_integer()) out.push_back(e.get<int>());
    return out;
}

int intOr(const json& j, const char* key, int def) {
    const json& v = jget(j, key);
    return v.is_number_integer() ? v.get<int>() : def;
}

// Averages the normals of vertices that share a position (quantised to 10 um). Used for smooth object normals.
void smoothVertexNormals(const std::vector<float>& P, std::vector<float>& N) {
    const size_t count = P.size() / 3;
    std::map<std::array<long long, 3>, glm::vec3> sum;
    auto key = [&](size_t v) {
        return std::array<long long, 3>{std::llround(P[v * 3] * 1e5), std::llround(P[v * 3 + 1] * 1e5),
                                        std::llround(P[v * 3 + 2] * 1e5)};
    };
    for (size_t v = 0; v < count; ++v)
        sum[key(v)] += glm::vec3(N[v * 3], N[v * 3 + 1], N[v * 3 + 2]);
    for (size_t v = 0; v < count; ++v) {
        glm::vec3 n = glm::normalize(sum[key(v)]);
        if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) continue;
        N[v * 3] = n.x;
        N[v * 3 + 1] = n.y;
        N[v * 3 + 2] = n.z;
    }
}

// Outline direction per vertex (the NPR repo's useAreaWeight): the sum of the triangle normals that share the
// vertex's position, each scaled by its area (the cross product carries the area). Vertices whose sum is degenerate
// keep the plain average already in S.
void areaWeightedOutlineNormals(const std::vector<float>& P, const cgltf_primitive& prim, std::vector<float>& S) {
    const size_t count = P.size() / 3;
    const size_t triCount = (prim.indices ? prim.indices->count : count) / 3;
    auto index = [&](size_t k) -> size_t {
        return prim.indices ? static_cast<size_t>(cgltf_accessor_read_index(prim.indices, k)) : k;
    };
    auto key = [&](size_t v) {
        return std::array<long long, 3>{std::llround(P[v * 3] * 1e5), std::llround(P[v * 3 + 1] * 1e5),
                                        std::llround(P[v * 3 + 2] * 1e5)};
    };
    std::map<std::array<long long, 3>, glm::vec3> sum;
    for (size_t t = 0; t < triCount; ++t) {
        const size_t i[3] = {index(t * 3), index(t * 3 + 1), index(t * 3 + 2)};
        if (i[0] >= count || i[1] >= count || i[2] >= count) continue;
        const glm::vec3 a(P[i[0] * 3], P[i[0] * 3 + 1], P[i[0] * 3 + 2]);
        const glm::vec3 b(P[i[1] * 3], P[i[1] * 3 + 1], P[i[1] * 3 + 2]);
        const glm::vec3 c(P[i[2] * 3], P[i[2] * 3 + 1], P[i[2] * 3 + 2]);
        const glm::vec3 fn = glm::cross(b - a, c - a);   // length = 2 x area
        for (int k = 0; k < 3; ++k) sum[key(i[k])] += fn;
    }
    for (size_t v = 0; v < count; ++v) {
        auto it = sum.find(key(v));
        if (it == sum.end() || glm::length(it->second) < 1e-30f) continue;
        const glm::vec3 n = glm::normalize(it->second);
        if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) continue;
        S[v * 3] = n.x;
        S[v * 3 + 1] = n.y;
        S[v * 3 + 2] = n.z;
    }
}

class Loader {
public:
    Loader(rhi::Device& dev, Model& m, const cgltf_data* d, const json& root, std::string baseDir)
        : dev_(dev), m_(m), data_(d), root_(root), baseDir_(std::move(baseDir)) {}
    bool smoothNormals = false;   // set before run()
    std::string faceSdfPath;      // set before run()
    std::string faceMaterial;     // set before run()
    bool preserveAlphaTestCoverage = true;   // set before run()
    float alphaTestThreshold = 0.5f;         // set before run()

    void run() {
        detectFormat();
        makeWhiteTexture();
        loadMaterials();
        loadPrimitives();
        loadNodes();
        loadSkins();
        buildDraws();
        loadHumanoid();
        loadSprings();
        loadReactions();
        loadExpressions();
        loadNamedMorphs();
        loadCollision();
    }

    // Torso collider and arm IK setup. The torso profile is measured from the skin: for each
    // height band we take percentiles of the body vertices, skipping any vertex whose dominant
    // joint is an arm, hand or finger bone. Symmetric extents are used so one-sided props (a
    // robotic arm, a bag) do not widen the body.
    void loadCollision() {
        auto humanoidNode = [&](const char* name) -> int {
            auto it = m_.humanoid.find(name);
            return it == m_.humanoid.end() ? -1 : it->second;
        };
        // Rest world position and rotation of every node (model space, root excluded).
        std::vector<glm::vec3> P(m_.nodes.size(), glm::vec3(0.0f));
        std::vector<glm::quat> R(m_.nodes.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        for (int i : m_.updateOrder) {
            const Node& n = m_.nodes[i];
            glm::vec3 t = n.translation;
            glm::quat r = n.baseRotation;
            if (n.hasMatrix) {
                glm::vec3 s, tt, skew;
                glm::vec4 persp;
                glm::quat rr;
                glm::decompose(n.matrix, s, rr, tt, skew, persp);
                t = tt;
                r = rr;
            }
            const int p = n.parent;
            const glm::vec3 pp = p >= 0 ? P[p] : glm::vec3(0.0f);
            const glm::quat pr = p >= 0 ? R[p] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            P[i] = pp + pr * t;
            R[i] = glm::normalize(pr * r);
        }

        const int hips = humanoidNode("hips"), spine = humanoidNode("spine");
        int chest = humanoidNode("chest");
        if (chest < 0) chest = humanoidNode("upperChest");
        if (chest < 0) chest = humanoidNode("neck");
        const int lUp = humanoidNode("leftUpperArm"), rUp = humanoidNode("rightUpperArm");
        const int lLo = humanoidNode("leftLowerArm"), rLo = humanoidNode("rightLowerArm");
        const int lHand = humanoidNode("leftHand"), rHand = humanoidNode("rightHand");
        if (hips < 0 || spine < 0 || chest < 0 || lUp < 0 || rUp < 0 || lLo < 0 || rLo < 0 || lHand < 0 || rHand < 0) {
            std::printf("collision: missing humanoid bones, torso collider disabled\n");
            return;
        }

        // Torso vertices are those whose dominant joint is a humanoid torso or leg bone. Arm,
        // hand and finger bones are excluded, and so are hair, tails and cloth-physics joints,
        // which hang off the body and would widen the measurement.
        std::vector<bool> isBody(m_.nodes.size(), false);
        static const char* kArmWords[] = {"UpperArm", "LowerArm", "Hand", "Thumb", "Index",
                                          "Middle", "Ring", "Little", "Toes"};
        for (const auto& [bone, node] : m_.humanoid) {
            if (node < 0 || node >= static_cast<int>(isBody.size())) continue;
            bool arm = false;
            for (const char* w : kArmWords)
                if (bone.find(w) != std::string::npos) arm = true;
            if (!arm) isBody[node] = true;
        }

        // Skinned body vertices (rest pose), tagged with whether their dominant joint is an arm.
        std::vector<glm::vec3> body;
        for (cgltf_size ni = 0; ni < data_->nodes_count; ++ni) {
            const cgltf_node& cn = data_->nodes[ni];
            if (!cn.mesh || !cn.skin) continue;
            const cgltf_skin& sk = *cn.skin;
            for (cgltf_size pi = 0; pi < cn.mesh->primitives_count; ++pi) {
                const cgltf_primitive& prim = cn.mesh->primitives[pi];
                const cgltf_accessor *pos = nullptr, *jnt = nullptr, *wgt = nullptr;
                for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                    const cgltf_attribute& at = prim.attributes[a];
                    if (at.type == cgltf_attribute_type_position) pos = at.data;
                    if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
                    if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
                }
                if (!pos || !jnt || !wgt) continue;
                for (cgltf_size v = 0; v < pos->count; ++v) {
                    float p3[3];
                    cgltf_uint j4[4];
                    float w4[4];
                    cgltf_accessor_read_float(pos, v, p3, 3);
                    cgltf_accessor_read_uint(jnt, v, j4, 4);
                    cgltf_accessor_read_float(wgt, v, w4, 4);
                    int best = 0;
                    for (int k = 1; k < 4; ++k) if (w4[k] > w4[best]) best = k;
                    if (w4[best] <= 0.0f || j4[best] >= sk.joints_count) continue;
                    const int node = static_cast<int>(sk.joints[j4[best]] - data_->nodes);
                    if (node < 0 || node >= static_cast<int>(isBody.size()) || !isBody[node]) continue;
                    body.push_back(glm::vec3(p3[0], p3[1], p3[2]));
                }
            }
        }

        const float Hy = P[hips].y, Sy = P[spine].y, Cy = P[chest].y;
        TorsoCollider& t = m_.torso;
        const float yLo = Sy - 0.4f * (Sy - Hy);
        const float yHi = Cy + 0.5f * (Cy - Sy);
        const float cxRef = 0.5f * (P[spine].x + P[chest].x);
        auto pct = [](std::vector<float>& v, float q) -> float {
            if (v.empty()) return 0.0f;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<float>(v.size())))];
        };
        for (int k = 0; k < TorsoCollider::kSamples; ++k) {
            const float y = yLo + (yHi - yLo) * float(k) / float(TorsoCollider::kSamples - 1);
            std::vector<float> xs, zs;
            float band = 0.025f;
            for (int attempt = 0; attempt < 3 && xs.size() < 40; ++attempt, band *= 2.0f) {
                xs.clear();
                zs.clear();
                for (const glm::vec3& p : body)
                    if (std::fabs(p.y - y) < band) { xs.push_back(p.x - cxRef); zs.push_back(p.z); }
            }
            t.ys[k] = y;
            t.cx[k] = cxRef;
            if (xs.size() < 10) {
                t.ax[k] = k > 0 ? t.ax[k - 1] : 0.15f;
                t.az[k] = k > 0 ? t.az[k - 1] : 0.10f;
                t.cz[k] = k > 0 ? t.cz[k - 1] : P[spine].z;
                continue;
            }
            std::vector<float> negx(xs.size());
            for (size_t i = 0; i < xs.size(); ++i) negx[i] = -xs[i];
            std::vector<float> zc = zs;
            const float cz = pct(zc, 0.5f);
            std::vector<float> dz(zs.size());
            for (size_t i = 0; i < zs.size(); ++i) dz[i] = std::fabs(zs[i] - cz);
            std::vector<float> xsCopy = xs;
            const float ax = std::min(pct(xsCopy, 0.97f), pct(negx, 0.97f));
            const float az = pct(dz, 0.97f);
            t.cz[k] = cz;
            t.ax[k] = std::max(ax, 0.05f);
            t.az[k] = std::max(az, 0.04f);
        }
        t.goalY = Sy + 0.3f * (Cy - Sy);
        t.valid = true;

        // Lateral and facing directions in model space (pre-root).
        glm::vec3 lat = P[lUp] - P[rUp];
        lat.y = 0.0f;
        if (glm::length(lat) < 1e-4f) lat = glm::vec3(1.0f, 0.0f, 0.0f);
        m_.lateral = glm::normalize(lat);
        m_.facing = glm::normalize(glm::cross(m_.lateral, glm::vec3(0.0f, 1.0f, 0.0f)));

        // arms[0] is right, arms[1] is left.
        m_.arms[0].valid = true; m_.arms[0].upper = rUp; m_.arms[0].lower = rLo; m_.arms[0].hand = rHand;
        m_.arms[1].valid = true; m_.arms[1].upper = lUp; m_.arms[1].lower = lLo; m_.arms[1].hand = lHand;

        { float topY = -1e9f, botY = 1e9f; for (const glm::vec3& v : body) { topY = std::max(topY, v.y); botY = std::min(botY, v.y); }
        std::printf("collision: body y[%.3f..%.3f] ", botY, topY); }
        std::printf("collision: hips %.3f spine %.3f chest %.3f torso y[%.3f..%.3f] ax[", Hy, Sy, Cy, yLo, yHi);
        for (int k = 0; k < TorsoCollider::kSamples; ++k) std::printf("%s%.3f", k ? " " : "", t.ax[k]);
        std::printf("] az[");
        for (int k = 0; k < TorsoCollider::kSamples; ++k) std::printf("%s%.3f", k ? " " : "", t.az[k]);
        std::printf("] goalY %.3f, %zu body verts\n", t.goalY, body.size());
    }

    // Named morph targets (mesh extras.targetNames), such as Seed-san's eye_brow_up and
    // eye_smile. They bind to face-track channels that the presets do not cover.
    void loadNamedMorphs() {
        // Weights: eye_smile closes the eyes almost fully at 1.0, so it is scaled down.
        static const struct { const char* channel; const char* target; float weight; } kNamed[] = {
            {"browup", "eye_brow_up", 1.0f},     {"browup", "eye_brow_up_L", 1.0f},   {"browup", "eye_brow_up_R", 1.0f},
            {"browdown", "eye_brow_down", 1.0f}, {"browdown", "eye_brow_down_L", 1.0f}, {"browdown", "eye_brow_down_R", 1.0f},
            {"eyesmile", "eye_smile", 0.35f},    {"cheek", "cheek", 1.0f},
        };
        const json& meshes = jget(root_, "meshes");
        if (!meshes.is_array()) return;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            const json& names = jget(jget(meshes[mi], "extras"), "targetNames");
            if (!names.is_array()) continue;
            for (const auto& k : kNamed) {
                for (size_t t = 0; t < names.size(); ++t) {
                    if (names[t].is_string() && names[t].get<std::string>() == k.target)
                        m_.expressionBinds[k.channel].push_back(
                            MorphBind{static_cast<int>(mi), static_cast<int>(t), k.weight});
                }
            }
        }
    }

    // Canonical expression name: VRM 0.x preset names are mapped onto VRM 1.0 names.
    static std::string canonicalExpression(std::string n) {
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n == "a") return "aa";
        if (n == "i") return "ih";
        if (n == "u") return "ou";
        if (n == "e") return "ee";
        if (n == "o") return "oh";
        if (n == "joy") return "happy";
        if (n == "sorrow") return "sad";
        if (n == "fun") return "relaxed";
        return n;
    }

    void loadExpressions() {
        const json& ext = jget(root_, "extensions");
        auto addBind = [&](const std::string& name, int mesh, int target, float weight) {
            if (name.empty() || mesh < 0 || target < 0 || weight == 0.0f) return;
            m_.expressionBinds[name].push_back(MorphBind{mesh, target, weight});
        };
        if (m_.isVRM) {
            const json& preset = jget(jget(jget(ext, "VRMC_vrm"), "expressions"), "preset");
            if (preset.is_object()) {
                for (auto it = preset.begin(); it != preset.end(); ++it) {
                    const json& binds = jget(it.value(), "morphTargetBinds");
                    if (!binds.is_array()) continue;
                    for (const json& b : binds) {
                        int node = b.value("node", -1);
                        if (node < 0 || node >= static_cast<int>(data_->nodes_count)) continue;
                        const cgltf_node& cn = data_->nodes[node];
                        if (!cn.mesh) continue;
                        addBind(canonicalExpression(it.key()),
                                static_cast<int>(cn.mesh - data_->meshes),
                                b.value("index", -1), b.value("weight", 1.0f));
                    }
                }
            }
        } else {
            const json& groups = jget(jget(jget(ext, "VRM"), "blendShapeMaster"), "blendShapeGroups");
            if (groups.is_array()) {
                for (const json& g : groups) {
                    std::string name = canonicalExpression(g.value("presetName", std::string()));
                    if (name == "unknown") continue;
                    const json& binds = jget(g, "binds");
                    if (!binds.is_array()) continue;
                    for (const json& b : binds)
                        addBind(name, b.value("mesh", -1), b.value("index", -1),
                                b.value("weight", 100.0f) / 100.0f);
                }
            }
        }
    }

    void loadReactions() {
        struct Spec { const char* bone; float weight, stiffness, damping; };
        const Spec specs[] = {
            {"hips", 0.0f, 0.0f, 0.0f},            // placeholder for future root-motion lean
            {"spine", 0.5f, 70.0f, 9.0f},
            {"chest", 1.0f, 55.0f, 8.0f},
            {"upperChest", 1.0f, 55.0f, 8.0f},
            {"neck", 0.6f, 45.0f, 7.0f},
            {"head", 0.9f, 35.0f, 6.0f},
        };
        for (const Spec& sp : specs) {
            if (sp.weight <= 0.0f) continue;
            auto it = m_.humanoid.find(sp.bone);
            if (it == m_.humanoid.end()) continue;
            ReactionBone r;
            r.node = it->second;
            r.weight = sp.weight;
            r.stiffness = sp.stiffness;
            r.damping = sp.damping;
            m_.reactions.push_back(r);
        }
    }

private:
    int firstChild(int node) const {
        for (size_t i = 0; i < m_.nodes.size(); ++i)
            if (m_.nodes[i].parent == node) return static_cast<int>(i);
        return -1;
    }

    glm::vec3 restTranslation(int node) const {
        const Node& n = m_.nodes[node];
        return n.hasMatrix ? glm::vec3(n.matrix[3]) : n.translation;
    }

    // Creates a simulated joint for `node`. Its rest axis points to the first child, or to
    // the node's own offset when it is a tip.
    void addSpringJoint(int node, float stiffness, float gravityPower, glm::vec3 gravityDir, float drag,
                        float hitRadius, const std::vector<int>& colliders, int childHint) {
        if (node < 0 || node >= static_cast<int>(m_.nodes.size())) return;
        if (m_.nodes[node].springJoint >= 0) return;   // one simulation per bone
        SpringJoint j;
        j.node = node;
        j.stiffness = stiffness;
        j.gravityPower = gravityPower;
        j.gravityDir = gravityDir;
        j.drag = drag;
        j.hitRadius = hitRadius;
        j.colliders = colliders;
        int child = childHint >= 0 ? childHint : firstChild(node);
        glm::vec3 axis = (child >= 0) ? restTranslation(child) : restTranslation(node);
        float len = glm::length(axis);
        if (len < 1e-5f) { axis = glm::vec3(0.0f, -1.0f, 0.0f); len = 0.05f; }
        j.restDir = axis / std::max(len, 1e-5f);
        j.boneLength = std::max(len, 0.01f);
        m_.nodes[node].springJoint = static_cast<int>(m_.springJoints.size());
        m_.springJoints.push_back(j);
    }

    void loadSprings() {
        const json& ext = jget(root_, "extensions");
        if (m_.isVRM) {
            const json& sb = jget(ext, "VRMC_springBone");
            if (!sb.is_object()) return;
            for (const auto& c : jget(sb, "colliders")) {
                SpringCollider sc;
                sc.node = intOr(c, "node", -1);
                const json& shape = jget(c, "shape");
                const json& sph = jget(shape, "sphere");
                const json& cap = jget(shape, "capsule");
                if (cap.is_object()) {
                    sc.capsule = true;
                    sc.offset = vec3Or(cap, "offset", glm::vec3(0.0f));
                    sc.tail = vec3Or(cap, "tail", glm::vec3(0.0f));
                    sc.radius = numOr(cap, "radius", 0.0f);
                } else if (sph.is_object()) {
                    sc.offset = vec3Or(sph, "offset", glm::vec3(0.0f));
                    sc.radius = numOr(sph, "radius", 0.0f);
                }
                m_.colliders.push_back(sc);
            }
            std::vector<std::vector<int>> groups;
            for (const auto& g : jget(sb, "colliderGroups")) groups.push_back(intArray(jget(g, "colliders")));
            for (const auto& sp : jget(sb, "springs")) {
                std::vector<int> cols;
                for (int gi : intArray(jget(sp, "colliderGroups")))
                    if (gi >= 0 && gi < static_cast<int>(groups.size()))
                        cols.insert(cols.end(), groups[gi].begin(), groups[gi].end());
                const json& joints = jget(sp, "joints");
                if (!joints.is_array()) continue;
                for (size_t k = 0; k < joints.size(); ++k) {
                    const json& jn = joints[k];
                    int child = (k + 1 < joints.size()) ? intOr(joints[k + 1], "node", -1) : -1;
                    addSpringJoint(intOr(jn, "node", -1), numOr(jn, "stiffness", 1.0f),
                                   numOr(jn, "gravityPower", 0.0f), vec3Or(jn, "gravityDir", glm::vec3(0, -1, 0)),
                                   numOr(jn, "dragForce", 0.5f), numOr(jn, "hitRadius", 0.0f), cols, child);
                }
            }
        } else if (m_.isVRM0) {
            const json& sa = jget(jget(ext, "VRM"), "secondaryAnimation");
            if (!sa.is_object()) return;
            // Collider groups: each group is one node with several sphere colliders.
            std::vector<std::vector<int>> groups;
            for (const auto& g : jget(sa, "colliderGroups")) {
                std::vector<int> idx;
                int node = intOr(g, "node", -1);
                for (const auto& c : jget(g, "colliders")) {
                    SpringCollider sc;
                    sc.node = node;
                    // VRM 0.x stores offsets in Unity space (left-handed): mirror X.
                    glm::vec3 o = vec3Obj(jget(c, "offset"), glm::vec3(0.0f));
                    sc.offset = glm::vec3(-o.x, o.y, o.z);
                    sc.radius = numOr(c, "radius", 0.0f);
                    idx.push_back(static_cast<int>(m_.colliders.size()));
                    m_.colliders.push_back(sc);
                }
                groups.push_back(idx);
            }
            for (const auto& bg : jget(sa, "boneGroups")) {
                std::vector<int> cols;
                for (int gi : intArray(jget(bg, "colliderGroups")))
                    if (gi >= 0 && gi < static_cast<int>(groups.size()))
                        cols.insert(cols.end(), groups[gi].begin(), groups[gi].end());
                float stiffness = numOr(bg, "stiffiness", 1.0f);   // (sic) VRM 0.x spelling
                float gpow = numOr(bg, "gravityPower", 0.0f);
                glm::vec3 gdir = vec3Obj(jget(bg, "gravityDir"), glm::vec3(0, -1, 0));
                float drag = numOr(bg, "dragForce", 0.4f);
                float hit = numOr(bg, "hitRadius", 0.02f);
                // Every bone in the group simulates, and so does every descendant of each root bone.
                std::function<void(int)> visit = [&](int node) {
                    if (node < 0 || node >= static_cast<int>(m_.nodes.size())) return;
                    addSpringJoint(node, stiffness, gpow, gdir, drag, hit, cols, -1);
                    for (size_t c = 0; c < m_.nodes.size(); ++c)
                        if (m_.nodes[c].parent == node) visit(static_cast<int>(c));
                };
                for (int root : intArray(jget(bg, "bones"))) visit(root);
            }
        }
    }

    int indexOf(const cgltf_texture* t) const {
        return t ? static_cast<int>(t - data_->textures) : -1;
    }

    void detectFormat() {
        const json& ext = jget(root_, "extensions");
        m_.isVRM = jget(ext, "VRMC_vrm").is_object();
        m_.isVRM0 = !m_.isVRM && jget(ext, "VRM").is_object();
        if (m_.isVRM0) m_.root = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0));
        const json& meta = m_.isVRM ? jget(jget(ext, "VRMC_vrm"), "meta")
                         : m_.isVRM0 ? jget(jget(ext, "VRM"), "meta") : kNull;
        const char* nameKey = m_.isVRM ? "name" : "title";
        const json& nm = jget(meta, nameKey);
        if (nm.is_string()) m_.name = nm.get<std::string>();
    }

    void makeWhiteTexture() {
        const uint8_t white[4] = {255, 255, 255, 255};
        rhi::TextureDesc d;
        d.width = 1;
        d.height = 1;
        d.format = rhi::Format::RGBA8_SRGB;
        d.wrap = rhi::Wrap::Repeat;
        auto tex = dev_.createTexture(d, white);
        white_ = tex.get();
        m_.textures.push_back(std::move(tex));   // model owns it; white_ stays valid
        // Flat tangent-space normal (0.5, 0.5, 1) in linear space: the normal map when a material has none.
        const uint8_t flatN[4] = {128, 128, 255, 255};
        rhi::TextureDesc n = d;
        n.format = rhi::Format::RGBA8;
        auto ntex = dev_.createTexture(n, flatN);
        flat_ = ntex.get();
        m_.textures.push_back(std::move(ntex));
    }

    // `masked` asks for an alpha-coverage-preserving chain at the material's cutoff: the same image can be shared between
    // an opaque material and a cutout, and the two need different chains, so the cutoff is part of the cache key.
    rhi::Texture* texture(int idx, bool srgb, bool masked = false, float cutoff = 0.5f) {
        if (idx < 0 || idx >= static_cast<int>(data_->textures_count)) return white_;
        const int cutoffKey = int(std::clamp(cutoff, 0.0f, 1.0f) * 255.0f + 0.5f);
        auto key = std::make_tuple(idx, srgb, masked, cutoffKey);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;

        const cgltf_image* img = data_->textures[idx].image;
        if (!img) return white_;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        std::vector<uint8_t> fileBytes;
        if (img->buffer_view) {
            bytes = cgltf_buffer_view_data(img->buffer_view);
            size = img->buffer_view->size;
        } else if (img->uri) {
            vfs::readBytes(baseDir_ + "/" + img->uri, fileBytes);
            bytes = fileBytes.data();
            size = fileBytes.size();
        }
        if (!bytes || size == 0) return white_;

        int w = 0, h = 0, c = 0;
        stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &c, 4);
        if (!px) return white_;
        rhi::TextureDesc d;
        d.width = static_cast<uint32_t>(w);
        d.height = static_cast<uint32_t>(h);
        d.format = srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8;
        d.mipmaps = true;
        d.filter = rhi::Filter::Linear;
        d.wrap = rhi::Wrap::Repeat;
        // Alpha-coverage-preserving mips for cutouts (GH-104289). The chain is built here, on the CPU, because the
        // coverage search cannot run inside a blit; every other texture keeps the backend's own filtering.
        rhi::MipChain chain;
        if (masked && preserveAlphaTestCoverage) {
            const float threshold = cutoff > 0.0f ? cutoff : alphaTestThreshold;
            const std::vector<MipLevel> levels = buildMipChain(px, uint32_t(w), uint32_t(h), true, threshold);
            chain.reserve(levels.size());
            for (const MipLevel& level : levels) chain.push_back(level.rgba);
        }
        auto tex = dev_.createTexture(d, px, chain.empty() ? nullptr : &chain);
        stbi_image_free(px);
        rhi::Texture* raw = tex.get();
        m_.textures.push_back(std::move(tex));
        cache_[key] = raw;
        return raw;
    }

    // Fills MToon parameters. VRM 1.0 uses VRMC_materials_mtoon per material;
    // VRM 0.x uses extensions.VRM.materialProperties (same index as glTF materials).
    // Texture indices are glTF texture indices, or -1 when absent.
    // True when the glTF material is MToon: VRM 1.0 VRMC_materials_mtoon, or VRM 0.x materialProperties shader VRM/MToon.
    bool isMToonMaterial(size_t matIndex) {
        const json& mats = jget(root_, "materials");
        if (mats.is_array() && matIndex < mats.size() &&
            jget(jget(mats[matIndex], "extensions"), "VRMC_materials_mtoon").is_object()) return true;
        const json& props = jget(jget(jget(root_, "extensions"), "VRM"), "materialProperties");
        if (props.is_array() && matIndex < props.size()) {
            const json& sh = jget(props[matIndex], "shader");
            if (sh.is_string() && sh.get<std::string>().rfind("VRM/MToon", 0) == 0) return true;
        }
        return false;
    }

    void applyMToon(int matIndex, MaterialUBO& p, int& shadeTex, int& emissiveTex) {
        const json& ext = jget(root_, "extensions");
        if (m_.isVRM) {
            const json& mats = jget(root_, "materials");
            if (!mats.is_array() || matIndex < 0 || matIndex >= static_cast<int>(mats.size())) return;
            const json& mt = jget(jget(mats[matIndex], "extensions"), "VRMC_materials_mtoon");
            if (!mt.is_object()) return;
            p.shadeColor = glm::vec4(vec3Or(mt, "shadeColorFactor", glm::vec3(0.97f, 0.81f, 0.86f)), 1.0f);
            const json& sh = jget(mt, "shadeMultiplyTexture");
            shadeTex = intOr(sh, "index", -1);
            p.toon.x = numOr(mt, "shadingShiftFactor", 0.0f);
            p.toon.y = numOr(mt, "shadingToonyFactor", 0.9f);
            p.toon.z = numOr(mt, "giEqualizationFactor", 0.9f);
            p.toon.w = numOr(mt, "rimLightingMixFactor", 1.0f);
            p.rim = glm::vec4(vec3Or(mt, "parametricRimColorFactor", glm::vec3(0.0f)),
                              numOr(mt, "parametricRimFresnelPowerFactor", 5.0f));
            p.rimLift.x = numOr(mt, "parametricRimLiftFactor", 0.0f);
            const json& wm = jget(mt, "outlineWidthMode");
            std::string mode = wm.is_string() ? wm.get<std::string>() : "none";
            p.flags2.x = mode == "worldCoordinates" ? 1.0f : mode == "screenCoordinates" ? 2.0f : 0.0f;
            p.outline = glm::vec4(vec3Or(mt, "outlineColorFactor", glm::vec3(0.0f)),
                                  numOr(mt, "outlineWidthFactor", 0.0f));
            return;
        }
        if (m_.isVRM0) {
            const json& mps = jget(jget(ext, "VRM"), "materialProperties");
            if (!mps.is_array() || matIndex < 0 || matIndex >= static_cast<int>(mps.size())) return;
            const json& mp = mps[matIndex];
            const json& shader = jget(mp, "shader");
            if (!shader.is_string() || shader.get<std::string>() != "VRM/MToon") return;
            const json& fp = jget(mp, "floatProperties");
            const json& vp = jget(mp, "vectorProperties");
            const json& tp = jget(mp, "textureProperties");
            auto color3 = [&](const char* name, glm::vec3 def) { return vec3Or(vp, name, def); };
            p.shadeColor = glm::vec4(color3("_ShadeColor", glm::vec3(0.97f, 0.81f, 0.86f)), 1.0f);
            shadeTex = intOr(tp, "_ShadeTexture", -1);
            emissiveTex = intOr(tp, "_EmissionMap", -1);
            p.toon.x = numOr(fp, "_ShadeShift", 0.0f);
            p.toon.y = numOr(fp, "_ShadeToony", 0.9f);
            p.toon.w = numOr(fp, "_RimLightingMix", 1.0f);
            p.rim = glm::vec4(color3("_RimColor", glm::vec3(0.0f)), numOr(fp, "_RimFresnelPower", 5.0f));
            p.rimLift.x = numOr(fp, "_RimLift", 0.0f);
            p.flags2.x = numOr(fp, "_OutlineWidthMode", 0.0f);
            // VRM 0.x outline width is in percent-ish units; UniVRM scales it by 0.01.
            p.outline = glm::vec4(color3("_OutlineColor", glm::vec3(0.0f)), numOr(fp, "_OutlineWidth", 0.0f) * 0.01f);
            if (jget(vp, "_EmissionColor").is_array())
                p.emissive = glm::vec4(color3("_EmissionColor", glm::vec3(0.0f)), 0.0f);
        }
    }

    void loadMaterials() {
        for (cgltf_size i = 0; i < data_->materials_count; ++i) {
            const cgltf_material& cm = data_->materials[i];
            auto mat = std::make_unique<Material>();
            mat->name = cm.name ? cm.name : "";
            MaterialUBO& p = mat->params;
            int baseIdx = -1, shadeIdx = -1, emisIdx = -1;

            if (cm.has_pbr_metallic_roughness) {
                const auto& pbr = cm.pbr_metallic_roughness;
                p.baseColor = glm::make_vec4(pbr.base_color_factor);
                baseIdx = indexOf(pbr.base_color_texture.texture);
            }
            p.emissive = glm::vec4(glm::make_vec3(cm.emissive_factor), 0.0f);
            emisIdx = indexOf(cm.emissive_texture.texture);

            switch (cm.alpha_mode) {
            case cgltf_alpha_mode_mask:
                p.flags.x = 1.0f;
                p.flags.y = cm.alpha_cutoff;
                mat->mask = true;
                break;
            case cgltf_alpha_mode_blend:
                p.flags.x = 2.0f;
                mat->blend = true;
                break;
            default:
                p.flags.x = 0.0f;
                break;
            }
            mat->doubleSided = cm.double_sided != 0;
            p.flags.z = mat->doubleSided ? 1.0f : 0.0f;
            // KHR_materials_unlit is ignored on MToon materials so the toon lighting runs (project choice).
            p.flags.w = (cm.unlit && !isMToonMaterial(i)) ? 1.0f : 0.0f;

            applyMToon(static_cast<int>(i), p, shadeIdx, emisIdx);

            // RealToon: normal map and gloss mask flags (set before the UBO is uploaded).
            int normIdx = cm.normal_texture.texture ? indexOf(cm.normal_texture.texture) : -1;
            int glossIdx = cm.has_pbr_metallic_roughness && cm.pbr_metallic_roughness.metallic_roughness_texture.texture
                ? indexOf(cm.pbr_metallic_roughness.metallic_roughness_texture.texture) : -1;
            const bool isFace = faceTexture() != nullptr && cm.name && containsNoCase(cm.name, faceMaterial);
            // Eye materials: the name has "eye" or "iris" but not "brow" (eyebrows). Matcap and highlight textures
            // are not assigned from the file yet, so their flags stay off.
            const bool isEye = cm.name && (containsNoCase(cm.name, "eye") || containsNoCase(cm.name, "iris"))
                               && !containsNoCase(cm.name, "brow");
            p.toon2 = glm::vec4(glossIdx >= 0 ? 1.0f : 0.0f, normIdx >= 0 ? 1.0f : 0.0f, isFace ? 1.0f : 0.0f, isEye ? 1.0f : 0.0f);
            // Leaf transmission: glTF material extras "leafTransmission" (strength, 0 = off) and "leafPower".
            {
                const json& mats = jget(root_, "materials");
                if (mats.is_array() && i < mats.size()) {
                    const json& ex = jget(mats[i], "extras");
                    if (ex.is_object()) {
                        const float strength = std::clamp(ex.value("leafTransmission", 0.0f), 0.0f, 1.0f);
                        p.leaf = glm::vec4(strength, std::max(ex.value("leafPower", 2.0f), 1.0f), 0.0f, 0.0f);
                    }
                }
            }
            mat->outlineBase = p.outline;
            mat->ubo = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(MaterialUBO));
            mat->ubo->upload(&p, sizeof(p));
            mat->set = dev_.createDescriptorSet(rhi::SetType::Material);
            mat->set->setTexture(4, texture(baseIdx, true, mat->mask, p.flags.y));
            mat->set->setTexture(5, texture(shadeIdx, true));
            mat->set->setTexture(6, texture(emisIdx, true));
            // RealToon: a normal map (linear data) and a gloss mask from the metallic-roughness texture.
            // Both fall back to fixed values, and the toon2 flags say whether the texture is really there.
            mat->set->setTexture(3, normIdx >= 0 ? texture(normIdx, false) : flat_);
            mat->set->setTexture(7, glossIdx >= 0 ? texture(glossIdx, false) : white_);
            mat->set->setTexture(2, isFace ? faceTexture() : white_);
            mat->set->setTexture(9, white_);
            mat->set->setTexture(10, white_);
            mat->set->setBuffer(8, mat->ubo.get());
            materialPtrs_.push_back(mat.get());
            m_.materials.push_back(std::move(mat));
        }
        // Fallback for primitives without a material.
        auto def = std::make_unique<Material>();
        def->name = "default";
        def->ubo = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(MaterialUBO));
        def->ubo->upload(&def->params, sizeof(MaterialUBO));
        def->set = dev_.createDescriptorSet(rhi::SetType::Material);
        def->set->setTexture(4, white_);
        def->set->setTexture(5, white_);
        def->set->setTexture(6, white_);
        def->set->setTexture(3, flat_);
        def->set->setTexture(7, white_);
        def->set->setTexture(2, white_);
        def->set->setTexture(9, white_);
        def->set->setTexture(10, white_);
        def->set->setBuffer(8, def->ubo.get());
        defaultMaterial_ = def.get();
        m_.materials.push_back(std::move(def));
    }

    static void readFloats(const cgltf_accessor* acc, size_t comps, std::vector<float>& out) {
        out.assign(acc->count * comps, 0.0f);
        for (cgltf_size i = 0; i < acc->count; ++i)
            cgltf_accessor_read_float(acc, i, &out[i * comps], comps);
    }

    void loadPrimitives() {
        meshFirst_.assign(data_->meshes_count, 0);
        meshCount_.assign(data_->meshes_count, 0);
        for (cgltf_size mi = 0; mi < data_->meshes_count; ++mi) {
            const cgltf_mesh& mesh = data_->meshes[mi];
            meshFirst_[mi] = static_cast<int>(m_.primitives.size());
            for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
                const cgltf_primitive& prim = mesh.primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) continue;

                const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
                const cgltf_accessor *col = nullptr, *uv1 = nullptr;
                for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                    const cgltf_attribute& at = prim.attributes[a];
                    if (at.type == cgltf_attribute_type_position) pos = at.data;
                    else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                    else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                    else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
                    else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
                    else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
                    else if (at.type == cgltf_attribute_type_texcoord && at.index == 1) uv1 = at.data;
                }
                if (!pos) continue;

                std::vector<float> P, N, T, W, C, T1, S;
                readFloats(pos, 3, P);
                if (nrm) readFloats(nrm, 3, N);
                if (nrm) { S = N; smoothVertexNormals(P, S); areaWeightedOutlineNormals(P, prim, S); }   // outline smooth normals, area-weighted; N is kept as authored
                if (nrm && smoothNormals) smoothVertexNormals(P, N);
                if (uv) readFloats(uv, 2, T);
                if (col) {
                    readFloats(col, 4, C);   // a vec3 colour leaves the fourth component at 0: set it below
                    if (cgltf_num_components(col->type) == 3)
                        for (size_t v = 0; v < col->count; ++v) C[v * 4 + 3] = 1.0f;
                }
                if (uv1) readFloats(uv1, 2, T1);
                if (wgt) readFloats(wgt, 4, W);

                const size_t count = pos->count;
                std::vector<rhi::Vertex> verts(count);
                glm::vec3 bmin(std::numeric_limits<float>::max());
                glm::vec3 bmax(std::numeric_limits<float>::lowest());
                for (size_t v = 0; v < count; ++v) {
                    rhi::Vertex& out = verts[v];
                    out.pos = {P[v * 3], P[v * 3 + 1], P[v * 3 + 2]};
                    bmin = glm::min(bmin, out.pos);
                    bmax = glm::max(bmax, out.pos);
                    out.normal = nrm ? glm::vec3(N[v * 3], N[v * 3 + 1], N[v * 3 + 2]) : glm::vec3(0, 1, 0);
                    out.uv = uv ? glm::vec2(T[v * 2], T[v * 2 + 1]) : glm::vec2(0.0f);
                    out.color = col ? glm::vec4(C[v * 4], C[v * 4 + 1], C[v * 4 + 2], C[v * 4 + 3]) : glm::vec4(1.0f);
                    out.uv1 = uv1 ? glm::vec2(T1[v * 2], T1[v * 2 + 1]) : out.uv;
                    out.smoothNormal = nrm ? glm::vec4(S[v * 3], S[v * 3 + 1], S[v * 3 + 2], 0.0f) : glm::vec4(0.0f);
                    out.weights = wgt ? glm::vec4(W[v * 4], W[v * 4 + 1], W[v * 4 + 2], W[v * 4 + 3])
                                      : glm::vec4(1, 0, 0, 0);
                    unsigned int j[4] = {0, 0, 0, 0};
                    if (jnt) cgltf_accessor_read_uint(jnt, v, j, 4);
                    for (int k = 0; k < 4; ++k) out.joints[k] = static_cast<uint16_t>(j[k]);
                    float wsum = out.weights.x + out.weights.y + out.weights.z + out.weights.w;
                    if (wsum > 1e-6f) out.weights /= wsum;
                    else out.weights = glm::vec4(1, 0, 0, 0);
                }
                if (m_.primitives.empty()) { m_.boundsMin = bmin; m_.boundsMax = bmax; }
                else { m_.boundsMin = glm::min(m_.boundsMin, bmin); m_.boundsMax = glm::max(m_.boundsMax, bmax); }

                // Morph targets: per-target position and normal deltas.
                std::vector<std::vector<glm::vec3>> tPos, tNrm;
                for (cgltf_size t = 0; t < prim.targets_count; ++t) {
                    const cgltf_morph_target& mt = prim.targets[t];
                    std::vector<glm::vec3> dp(count, glm::vec3(0.0f)), dn(count, glm::vec3(0.0f));
                    for (cgltf_size a = 0; a < mt.attributes_count; ++a) {
                        const cgltf_attribute& at = mt.attributes[a];
                        if (!at.data || at.data->count != count) continue;
                        if (at.type != cgltf_attribute_type_position && at.type != cgltf_attribute_type_normal)
                            continue;
                        std::vector<float> f;
                        readFloats(at.data, 3, f);
                        auto& dst = at.type == cgltf_attribute_type_position ? dp : dn;
                        for (size_t v = 0; v < count; ++v) dst[v] = {f[v * 3], f[v * 3 + 1], f[v * 3 + 2]};
                    }
                    tPos.push_back(std::move(dp));
                    tNrm.push_back(std::move(dn));
                }

                std::vector<uint32_t> idx;
                if (prim.indices) {
                    idx.resize(prim.indices->count);
                    for (cgltf_size k = 0; k < prim.indices->count; ++k)
                        idx[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k));
                } else {
                    idx.resize(count);
                    for (size_t k = 0; k < count; ++k) idx[k] = static_cast<uint32_t>(k);
                }

                Primitive out;
                out.mesh = static_cast<int>(mi);
                if (!tPos.empty()) {
                    out.morphBase = verts;
                    out.morphPos = std::move(tPos);
                    out.morphNrm = std::move(tNrm);
                }
                out.cpuVertices = verts;
                out.cpuIndices = idx;
                out.vertices = dev_.createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
                out.vertices->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
                out.indices = dev_.createBuffer(rhi::BufferUsage::Index, idx.size() * sizeof(uint32_t));
                out.indices->upload(idx.data(), idx.size() * sizeof(uint32_t));
                out.indexCount = static_cast<uint32_t>(idx.size());
                int mIdx = prim.material ? static_cast<int>(prim.material - data_->materials) : -1;
                out.material = (mIdx >= 0 && mIdx < static_cast<int>(materialPtrs_.size()))
                                   ? materialPtrs_[mIdx] : defaultMaterial_;
                m_.primitives.push_back(std::move(out));
                meshCount_[mi]++;
            }
        }
    }

    void loadNodes() {
        m_.nodes.resize(data_->nodes_count);
        for (cgltf_size i = 0; i < data_->nodes_count; ++i) {
            const cgltf_node& cn = data_->nodes[i];
            Node& n = m_.nodes[i];
            n.name = cn.name ? cn.name : "";
            n.parent = cn.parent ? static_cast<int>(cn.parent - data_->nodes) : -1;
            n.mesh = cn.mesh ? static_cast<int>(cn.mesh - data_->meshes) : -1;
            if (cn.has_matrix) {
                n.hasMatrix = true;
                n.matrix = glm::make_mat4(cn.matrix);
            } else {
                n.translation = cn.has_translation ? glm::make_vec3(cn.translation) : glm::vec3(0.0f);
                // glTF quaternions are stored as (x, y, z, w).
                if (cn.has_rotation)
                    n.baseRotation = glm::quat(cn.rotation[3], cn.rotation[0], cn.rotation[1], cn.rotation[2]);
                n.scale = cn.has_scale ? glm::make_vec3(cn.scale) : glm::vec3(1.0f);
            }
        }
        // Parents-before-children order for the world-matrix pass.
        std::vector<bool> done(m_.nodes.size(), false);
        std::function<void(int)> visit = [&](int i) {
            if (done[i]) return;
            if (m_.nodes[i].parent >= 0) visit(m_.nodes[i].parent);
            done[i] = true;
            m_.updateOrder.push_back(i);
        };
        for (size_t i = 0; i < m_.nodes.size(); ++i) visit(static_cast<int>(i));
    }

    void loadSkins() {
        for (cgltf_size s = 0; s < data_->skins_count; ++s) {
            const cgltf_skin& cs = data_->skins[s];
            if (cs.joints_count > 128) throw std::runtime_error("skin has more than 128 joints");
            Skin sk;
            for (cgltf_size j = 0; j < cs.joints_count; ++j)
                sk.joints.push_back(static_cast<int>(cs.joints[j] - data_->nodes));
            sk.inverseBind.assign(sk.joints.size(), glm::mat4(1.0f));
            if (cs.inverse_bind_matrices) {
                const cgltf_accessor* acc = cs.inverse_bind_matrices;
                for (cgltf_size j = 0; j < acc->count && j < sk.joints.size(); ++j) {
                    float f[16];
                    cgltf_accessor_read_float(acc, j, f, 16);
                    sk.inverseBind[j] = glm::make_mat4(f);
                }
            }
            JointUBO init{};
            for (auto& jm : init.joints) jm = glm::mat4(1.0f);
            sk.ubo = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(JointUBO));
            sk.ubo->upload(&init, sizeof(init));
            sk.set = dev_.createDescriptorSet(rhi::SetType::Skin);
            sk.set->setBuffer(9, sk.ubo.get());
            m_.skins.push_back(std::move(sk));
        }
        // Identity palette for non-skinned draws, so the Skin set is always bound.
        JointUBO init{};
        for (auto& jm : init.joints) jm = glm::mat4(1.0f);
        m_.staticSkinUbo = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(JointUBO));
        m_.staticSkinUbo->upload(&init, sizeof(init));
        m_.staticSkinSet = dev_.createDescriptorSet(rhi::SetType::Skin);
        m_.staticSkinSet->setBuffer(9, m_.staticSkinUbo.get());
    }

    void buildDraws() {
        for (cgltf_size i = 0; i < data_->nodes_count; ++i) {
            const cgltf_node& cn = data_->nodes[i];
            if (!cn.mesh) continue;
            int mi = static_cast<int>(cn.mesh - data_->meshes);
            int skin = cn.skin ? static_cast<int>(cn.skin - data_->skins) : -1;
            for (int k = 0; k < meshCount_[mi]; ++k) {
                DrawItem d;
                d.node = static_cast<int>(i);
                d.primitive = meshFirst_[mi] + k;
                d.skin = skin;
                m_.draws.push_back(d);
            }
        }
    }

    void loadHumanoid() {
        const json& ext = jget(root_, "extensions");
        if (m_.isVRM) {
            const json& hb = jget(jget(jget(ext, "VRMC_vrm"), "humanoid"), "humanBones");
            if (hb.is_object())
                for (auto it = hb.begin(); it != hb.end(); ++it) {
                    int node = intOr(it.value(), "node", -1);
                    if (node >= 0) m_.humanoid[it.key()] = node;
                }
        } else if (m_.isVRM0) {
            const json& hb = jget(jget(jget(ext, "VRM"), "humanoid"), "humanBones");
            if (hb.is_array())
                for (const auto& e : hb) {
                    const json& bone = jget(e, "bone");
                    int node = intOr(e, "node", -1);
                    if (bone.is_string() && node >= 0) m_.humanoid[bone.get<std::string>()] = node;
                }
        }
    }

    rhi::Device& dev_;
    Model& m_;
    const cgltf_data* data_;
    const json& root_;
    std::string baseDir_;
    rhi::Texture* white_ = nullptr;
    rhi::Texture* flat_ = nullptr;     // flat normal map fallback
    rhi::Texture* faceSdf_ = nullptr;  // face shadow atlas, loaded on first use
    bool faceSdfTried_ = false;

    static bool containsNoCase(const char* name, const std::string& needle) {
        std::string h(name);
        std::string n = needle;
        for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return !n.empty() && h.find(n) != std::string::npos;
    }

    // The face SDF atlas, read as linear data (R holds the distance). Null when there is none or it cannot be read.
    rhi::Texture* faceTexture() {
        if (faceSdfTried_) return faceSdf_;
        faceSdfTried_ = true;
        if (faceSdfPath.empty()) return nullptr;
        int w = 0, h = 0, c = 0;
        std::vector<uint8_t> sdfBytes;
        stbi_uc* px = vfs::readBytes(faceSdfPath, sdfBytes) && !sdfBytes.empty()
            ? stbi_load_from_memory(sdfBytes.data(), static_cast<int>(sdfBytes.size()), &w, &h, &c, 4)
            : nullptr;
        if (!px) return nullptr;
        rhi::TextureDesc d;
        d.width = static_cast<uint32_t>(w);
        d.height = static_cast<uint32_t>(h);
        d.format = rhi::Format::RGBA8;
        d.mipmaps = false;
        d.filter = rhi::Filter::Linear;
        d.wrap = rhi::Wrap::Clamp;
        auto tex = dev_.createTexture(d, px);
        stbi_image_free(px);
        faceSdf_ = tex.get();
        m_.textures.push_back(std::move(tex));
        return faceSdf_;
    }
    std::map<std::tuple<int, bool, bool, int>, rhi::Texture*> cache_;
    std::vector<Material*> materialPtrs_;
    Material* defaultMaterial_ = nullptr;
    std::vector<int> meshFirst_;
    std::vector<int> meshCount_;
};

} // namespace

namespace {

bool isPmxPath(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".pmx";
}

// PMX textures are named relative to the model's folder. Some packages keep them one or
// two folders up, so those are tried too.
bool readPmxTexture(const std::string& modelDir, const std::string& rel, std::vector<uint8_t>& out) {
    std::filesystem::path dir(modelDir);
    for (int up = 0; up < 3 && !dir.empty(); ++up) {
        if (vfs::readBytes((dir / rel).string(), out)) return true;
        dir = dir.parent_path();
    }
    return false;
}

} // namespace

std::unique_ptr<Model> loadModel(rhi::Device& device, const std::string& path, const ModelLoadOptions& lopts) {
    cgltf_options opts{};
    vfs::useForCgltf(opts);
    const std::string baseDir = std::filesystem::path(path).parent_path().string();
    // A PMX model is converted to GLB in memory and then loaded like any glTF. The cgltf data
    // points into `converted`, so `converted` is declared before `guard` (destroyed after it).
    std::vector<uint8_t> converted;
    std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(nullptr, cgltf_free);
    cgltf_data* data = nullptr;
    json root;
    if (isPmxPath(path)) {
        std::vector<uint8_t> pmx;
        if (!vfs::readBytes(path, pmx)) throw std::runtime_error("could not read " + path);
        pmx::ImportReport report;
        std::string error;
        if (!pmx::convertToGlb(pmx,
                               [&](const std::string& rel, std::vector<uint8_t>& out) { return readPmxTexture(baseDir, rel, out); },
                               converted, error, &report))
            throw std::runtime_error("PMX import failed for " + path + ": " + error);
        for (const std::string& w : report.warnings) std::fprintf(stderr, "PMX %s: %s\n", path.c_str(), w.c_str());
        if (cgltf_parse(&opts, converted.data(), converted.size(), &data) != cgltf_result_success)
            throw std::runtime_error("cgltf could not parse the converted PMX " + path);
        guard.reset(data);
        if (cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success)
            throw std::runtime_error("cgltf could not load buffers of the converted PMX " + path);
        root = jsonFromBytes(converted);
    } else {
        if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success)
            throw std::runtime_error("cgltf could not parse " + path);
        guard.reset(data);
        if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success)
            throw std::runtime_error("cgltf could not load buffers of " + path);
        root = readJson(path);
    }

    auto model = std::make_unique<Model>();
    model->sourcePath = path;
    model->name = std::filesystem::path(path).stem().string();

    Loader loader(device, *model, data, root, baseDir);
    loader.smoothNormals = lopts.smoothNormals;
    loader.faceSdfPath = lopts.faceSdfPath;
    loader.faceMaterial = lopts.faceMaterial;
    loader.preserveAlphaTestCoverage = lopts.preserveAlphaTestCoverage;
    loader.alphaTestThreshold = lopts.alphaTestThreshold;
    loader.run();
    return model;
}

namespace {

// Closest point on segment ab to p.
glm::vec3 closestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 ab = b - a;
    float denom = glm::dot(ab, ab);
    if (denom < 1e-10f) return a;
    float t = std::clamp(glm::dot(p - a, ab) / denom, 0.0f, 1.0f);
    return a + ab * t;
}

// One verlet step for a single bone. Returns the joint's new local rotation delta.
glm::quat stepSpringJoint(Model& m, int ji, const glm::mat4& parentWorld, const Node& n, float dt) {
    SpringJoint& j = m.springJoints[ji];
    glm::quat parentRot = glm::normalize(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(parentWorld[0])),
                                                                  glm::normalize(glm::vec3(parentWorld[1])),
                                                                  glm::normalize(glm::vec3(parentWorld[2])))));
    glm::vec3 head = glm::vec3(parentWorld * glm::vec4(n.translation, 1.0f));
    glm::quat restRot = parentRot * n.baseRotation;
    glm::vec3 restDir = glm::normalize(restRot * j.restDir);

    if (!j.init) {
        j.currTail = j.prevTail = head + restDir * j.boneLength;
        j.init = true;
    }
    // Verlet: inertia from last step, pull toward rest axis, gravity, length constraint.
    glm::vec3 inertia = (j.currTail - j.prevTail) * (1.0f - j.drag);
    glm::vec3 stiff = restDir * (j.stiffness * dt);
    glm::vec3 external = j.gravityDir * (j.gravityPower * dt);
    glm::vec3 next = j.currTail + inertia + stiff + external;
    float len = glm::length(next - head);
    if (len > 1e-6f) next = head + (next - head) / len * j.boneLength;

    // Colliders push the tail out of their volumes.
    for (int ci : j.colliders) {
        const SpringCollider& c = m.colliders[ci];
        if (c.node < 0) continue;
        const glm::mat4& cw = m.nodes[c.node].world;
        glm::vec3 a = glm::vec3(cw * glm::vec4(c.offset, 1.0f));
        glm::vec3 b = c.capsule ? glm::vec3(cw * glm::vec4(c.tail, 1.0f)) : a;
        float R = c.radius + j.hitRadius;
        glm::vec3 q = closestOnSegment(next, a, b);
        glm::vec3 d = next - q;
        float dist = glm::length(d);
        if (dist < R) {
            glm::vec3 dir = dist > 1e-6f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);
            next = q + dir * R;
            float l2 = glm::length(next - head);
            if (l2 > 1e-6f) next = head + (next - head) / l2 * j.boneLength;
        }
    }

    j.prevTail = j.currTail;
    j.currTail = next;
    glm::vec3 dirNow = glm::normalize(next - head);
    glm::quat delta = glm::rotation(restDir, dirNow);
    glm::quat desired = delta * restRot;
    glm::quat local = glm::inverse(parentRot) * desired;
    return local * glm::inverse(n.baseRotation);   // so that extra * base == local
}

} // namespace

namespace {

// Integrates the reaction springs one step and clamps the lean angle.
void stepReactions(Model& m, float dt) {
    const float kMaxLean = 0.7f;
    const glm::vec3 swayLocal = m.swayForce;
    for (auto& r : m.reactions) {
        glm::vec3 acc = -r.stiffness * r.angle - r.damping * r.vel + swayLocal * r.weight;
        r.vel += acc * dt;
        r.angle += r.vel * dt;
        float a = glm::length(r.angle);
        if (a > kMaxLean) {
            r.angle *= kMaxLean / a;
            r.vel *= 0.5f;
        }
    }
    m.swayForce = glm::vec3(0.0f);
}

} // namespace

void applyHitImpulse(Model& m, const glm::vec3& worldDir, float strength) {
    glm::vec3 d = glm::vec3(glm::inverse(m.root) * glm::vec4(worldDir, 0.0f));
    d.y = 0.0f;
    if (glm::length(d) < 1e-5f) return;
    d = glm::normalize(d);
    // Top of the body moves along the push: rotation axis is up x push.
    glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), d);
    for (auto& r : m.reactions) r.vel += axis * (strength * r.weight);
}

void applySway(Model& m, const glm::vec3& worldAccel) {
    glm::vec3 a = glm::vec3(glm::inverse(m.root) * glm::vec4(worldAccel, 0.0f));
    a.y = 0.0f;
    // Lean against the acceleration: the force is the cross product of up and -accel.
    m.swayForce += glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), -a) * 0.5f;
}

void applyExpressions(Model& m) {
    for (auto& p : m.primitives) {
        if (p.morphPos.empty()) continue;
        std::vector<float> w(p.morphPos.size(), 0.0f);
        for (const auto& [name, binds] : m.expressionBinds) {
            auto it = m.expressionValues.find(name);
            if (it == m.expressionValues.end() || it->second <= 0.0f) continue;
            for (const MorphBind& b : binds)
                if (b.mesh == p.mesh && b.target >= 0 && b.target < static_cast<int>(w.size()))
                    w[b.target] += it->second * b.weight;
        }
        for (float& v : w) v = glm::clamp(v, 0.0f, 1.0f);
        if (w == p.morphW) continue;
        p.morphW = w;

        std::vector<rhi::Vertex> verts = p.morphBase;
        for (size_t t = 0; t < w.size(); ++t) {
            if (w[t] <= 1e-5f) continue;
            const auto& dp = p.morphPos[t];
            const auto& dn = p.morphNrm[t];
            for (size_t v = 0; v < verts.size(); ++v) {
                verts[v].pos += w[t] * dp[v];
                verts[v].normal += w[t] * dn[v];
            }
        }
        for (auto& v : verts) {
            float len = glm::length(v.normal);
            if (len > 1e-6f) v.normal /= len;
        }
        p.vertices->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
    }
}

namespace {


// Rotation part of a model-space matrix (scale removed).
glm::quat quatOf(const glm::mat4& M) {
    glm::mat3 r(M);
    r[0] = glm::normalize(r[0]);
    r[1] = glm::normalize(r[1]);
    r[2] = glm::normalize(r[2]);
    return glm::normalize(glm::quat_cast(r));
}

// Torso profile at height y.
void torsoAt(const TorsoCollider& t, float y, float& cx, float& cz, float& ax, float& az) {
    const int last = TorsoCollider::kSamples - 1;
    if (y <= t.ys[0]) { cx = t.cx[0]; cz = t.cz[0]; ax = t.ax[0]; az = t.az[0]; return; }
    if (y >= t.ys[last]) { cx = t.cx[last]; cz = t.cz[last]; ax = t.ax[last]; az = t.az[last]; return; }
    int k = 0;
    while (k < last - 1 && y > t.ys[k + 1]) ++k;
    const float f = (y - t.ys[k]) / (t.ys[k + 1] - t.ys[k]);
    cx = glm::mix(t.cx[k], t.cx[k + 1], f);
    cz = glm::mix(t.cz[k], t.cz[k + 1], f);
    ax = glm::mix(t.ax[k], t.ax[k + 1], f);
    az = glm::mix(t.az[k], t.az[k + 1], f);
}

// Signed distance from p to the torso surface (positive outside) and the outward normal.
float torsoDistance(const TorsoCollider& t, const glm::vec3& p, glm::vec3& normal) {
    const float yLo = t.ys[0], yHi = t.ys[TorsoCollider::kSamples - 1];
    const float yc = std::clamp(p.y, yLo, yHi);
    float cx, cz, ax, az;
    torsoAt(t, yc, cx, cz, ax, az);
    const float u = (p.x - cx) / ax, v = (p.z - cz) / az;
    const float rho = std::sqrt(u * u + v * v);
    const float sdH = (rho - 1.0f) * std::min(ax, az);
    // Height beyond the ends: the capsule caps are open, so use the vertical gap.
    const float dyOut = p.y > yHi ? p.y - yHi : (p.y < yLo ? yLo - p.y : -1.0f);
    const float a = std::max(sdH, 0.0f), b = std::max(dyOut, 0.0f);
    const float d = std::sqrt(a * a + b * b) + std::min(std::max(sdH, dyOut), 0.0f);
    if (dyOut > sdH) {
        normal = glm::vec3(0.0f, p.y > yHi ? 1.0f : -1.0f, 0.0f);
    } else if (rho > 1e-6f) {
        normal = glm::normalize(glm::vec3(u / ax, 0.0f, v / az));
    } else {
        normal = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    return d;
}

// Hand-on-waist goal for one arm (model space): the point on the waist surface beside the hip.
glm::vec3 hipGoal(const Model& m, int side) {
    const TorsoCollider& t = m.torso;
    float cx, cz, ax, az;
    torsoAt(t, t.goalY, cx, cz, ax, az);
    const glm::vec3 sideDir = side == 1 ? m.lateral : -m.lateral;
    const glm::vec3 back = -m.facing;
    const glm::vec3 c(cx, t.goalY, cz);
    return c + sideDir * (ax + 0.08f) + back * 0.02f;
}

constexpr float kHandRadius = 0.045f;     // wrist and hand samples
constexpr float kForearmRadius = 0.05f;
constexpr float kElbowRadius = 0.055f;

struct ArmSample { glm::vec3 p; float r; float weight; };

// Clearance of each sample from the torso (positive is clear) and the largest correction.
void armClearance(const TorsoCollider& t, const std::vector<ArmSample>& s, float& minClearance,
                  glm::vec3& wristPush, glm::vec3& elbowPush, float& maxPen) {
    minClearance = 1e9f;
    maxPen = 0.0f;
    wristPush = glm::vec3(0.0f);
    elbowPush = glm::vec3(0.0f);
    float wristMag = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        glm::vec3 n;
        const float d = torsoDistance(t, s[i].p, n);
        const float clear = d - (s[i].r + 0.005f);   // 5 mm skin margin
        minClearance = std::min(minClearance, clear);
        if (clear < 0.0f) {
            const float pen = -clear;
            maxPen = std::max(maxPen, pen);
            const glm::vec3 push = n * pen;
            // The largest correction from any sample drives the wrist target. The elbow
            // samples also feed the pole bias, so the elbow flares out as well.
            if (glm::length(push) > wristMag) { wristMag = glm::length(push); wristPush = push; }
            if (s[i].weight < 1.0f) elbowPush += push * s[i].weight;
        }
    }
}

// Solves one arm: the animation pose gives the start, the torso collider and hand-on-waist
// goal give the target. The offset state is a damped spring, so the arm yields and then
// relaxes back. The elbow bias moves the pole, so the elbow flares away from the torso.
void solveArm(Model& m, int side, float dt, const glm::mat4& invRoot) {
    ArmIK& A = m.arms[side];
    if (!A.valid || !m.torso.valid) return;
    const TorsoCollider& t = m.torso;
    auto M = [&](int node) { return invRoot * m.nodes[node].world; };
    const glm::mat4 Mu = M(A.upper), Ml = M(A.lower), Mh = M(A.hand);
    const int parent = m.nodes[A.upper].parent;
    const glm::mat4 Mp = parent >= 0 ? invRoot * m.nodes[parent].world : glm::mat4(1.0f);
    const glm::vec3 S = glm::vec3(Mu[3]), E0 = glm::vec3(Ml[3]), W0 = glm::vec3(Mh[3]);
    const glm::quat Rp = quatOf(Mp), Ru = quatOf(Mu), Rl = quatOf(Ml), Rh = quatOf(Mh);
    const float L1 = glm::length(E0 - S), L2 = glm::length(W0 - E0);
    if (L1 < 1e-4f || L2 < 1e-4f) return;
    const glm::vec3 dirF0 = glm::normalize(W0 - E0);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    // Paused or first frame: there is no spring history, so the goal is applied at once.
    // Anim-pose pushes are added first so frame 0 is already clear.
    if (dt <= 0.0f) {
        glm::vec3 wp0, ep0;
        float mn0, pen0;
        std::vector<ArmSample> s0 = {
            {E0, kElbowRadius, 0.5f},
            {(S + E0) * 0.5f, kForearmRadius, 0.3f},
            {(E0 + W0) * 0.5f, kForearmRadius, 1.0f},
            {W0, kHandRadius, 1.0f},
            {W0 + dirF0 * 0.08f, kHandRadius, 1.0f},
        };
        armClearance(t, s0, mn0, wp0, ep0, pen0);
        A.offsetGoal += wp0;
        A.offset = A.offsetGoal;
        A.offsetVel = glm::vec3(0.0f);
    }

    // Spring first, using the goal from last frame.
    if (dt > 0.0f) {
        const float k = 180.0f, c = 2.0f * std::sqrt(k) * 0.8f;
        glm::vec3 acc = k * (A.offsetGoal - A.offset) - c * A.offsetVel;
        A.offsetVel += acc * dt;
        A.offset += A.offsetVel * dt;
        const float decay = std::exp(-dt / 0.5f);
        A.offsetGoal *= decay;
        A.elbowBias *= std::exp(-dt / 0.3f);
    }

    // Does the animation pose need solving at all?
    std::vector<ArmSample> animSamples = {
        {E0, kElbowRadius, 0.5f},
        {(S + E0) * 0.5f, kForearmRadius, 0.3f},
        {(E0 + W0) * 0.5f, kForearmRadius, 1.0f},
        {W0, kHandRadius, 1.0f},
        {W0 + dirF0 * 0.08f, kHandRadius, 1.0f},
    };
    float animMin, animPen;
    glm::vec3 wp, ep;
    armClearance(t, animSamples, animMin, wp, ep, animPen);
    const bool needed = A.hipWeight > 1e-3f || glm::length(A.offset) > 1e-4f ||
                        glm::length(A.offsetGoal) > 1e-4f || glm::length(A.elbowBias) > 1e-4f ||
                        animMin < 0.02f;
    if (!needed) {
        A.clearance = animMin;
        A.penetration = animPen;
        return;
    }

    // Target: animated wrist, pushed out by the offset and drawn to the hip goal.
    const glm::vec3 G = hipGoal(m, side);
    const glm::vec3 T = W0 + A.offset + (G - W0) * A.hipWeight;
    const glm::vec3 sideDir = side == 1 ? m.lateral : -m.lateral;
    const glm::vec3 back = -m.facing;
    const glm::vec3 P = E0 + (sideDir * 0.35f + back * 0.12f) * (L1 * A.hipWeight) + A.elbowBias * 3.0f;

    // Shared two-bone solver (rig.cpp); the rotations are model space, so the overlays are
    // made relative to the parents.
    // Solve, then check the torso clearance. If a sample is still inside the torso, push the
    // elbow pole out by the measured penetration and re-solve in the same frame (up to three
    // passes). Without this the elbow would only react one frame later, which left a transient
    // at the start of the hip ramp.
    glm::vec3 poleExtra(0.0f);
    float minClear = 1e9f, pen = 0.0f;
    glm::vec3 wristPush(0.0f), elbowPush(0.0f);
    for (int pass = 0; pass < 3; ++pass) {
        const TwoBoneSolution sol = solveTwoBone(S, E0, W0, Ru, Rl, Rh, T, P + poleExtra * 3.0f);
        const glm::vec3 E = sol.E, W = sol.W;
        const glm::quat RuNew = sol.Ru, RlNew = sol.Rl, RhNew = sol.Rh;

        m.ikRot[A.upper] = glm::normalize(glm::inverse(Rp) * RuNew);
        m.ikRot[A.lower] = glm::normalize(glm::inverse(RuNew) * RlNew);
        m.ikRot[A.hand] = glm::normalize(glm::inverse(RlNew) * RhNew);
        m.ikActive[A.upper] = m.ikActive[A.lower] = m.ikActive[A.hand] = 1;

        const glm::vec3 dirF = glm::normalize(W - E);
        const std::vector<ArmSample> s = {
            {E, kElbowRadius, 0.5f},
            {(S + E) * 0.5f, kForearmRadius, 0.3f},
            {(E + W) * 0.5f, kForearmRadius, 1.0f},
            {W, kHandRadius, 1.0f},
            {W + dirF * 0.08f, kHandRadius, 1.0f},
        };
        armClearance(t, s, minClear, wristPush, elbowPush, pen);
        if (minClear >= 0.0f) break;
        poleExtra += elbowPush;
    }
    A.clearance = minClear;
    A.penetration = pen;
    A.offsetGoal += wristPush;
    const float maxOff = 0.25f;
    if (glm::length(A.offsetGoal) > maxOff) A.offsetGoal *= maxOff / glm::length(A.offsetGoal);
    A.elbowBias = elbowPush;
}

// Forward kinematics of the whole skeleton into node.world. Pass A uses the animation
// pose only (no IK) so the arm solver starts from the animated arm. Pass B adds the IK
// overlays and runs the spring bones.
void computeWorld(Model& m, std::vector<glm::quat>& extra, float dt, bool springs, bool useIK) {
    for (int i : m.updateOrder) {
        Node& n = m.nodes[i];
        const glm::mat4 parentW = n.parent >= 0 ? m.nodes[n.parent].world : m.root;
        if (springs && m.physics && dt > 0.0f && n.springJoint >= 0 && !n.hasMatrix)
            extra[i] = stepSpringJoint(m, n.springJoint, parentW, n, dt);
        if (!n.hasMatrix) {
            const bool posed = i < static_cast<int>(m.poseActive.size()) && m.poseActive[i];
            glm::quat base = posed ? m.poseRot[i] : n.baseRotation;
            glm::vec3 trans = posed ? m.poseTrans[i] : n.translation;
            if (useIK && i < static_cast<int>(m.ikActive.size()) && m.ikActive[i]) base = m.ikRot[i];
            glm::quat r = extra[i] * base;
            n.local = glm::translate(glm::mat4(1.0f), trans) * glm::mat4_cast(r)
                    * glm::scale(glm::mat4(1.0f), n.scale);
        } else {
            n.local = n.matrix;
        }
        n.world = parentW * n.local;
    }
}

} // namespace

void updateModel(Model& m, float time, const glm::vec3& lookAtWorld) {
    applyExpressions(m);

    // Fixed-rate verlet: dt is clamped, and the spring state resets after a time jump.
    float dt = 0.0f;
    if (m.lastSimTime >= 0.0f) {
        float raw = time - m.lastSimTime;
        if (raw > 0.25f) {
            for (auto& j : m.springJoints) j.init = false;
        }
        dt = std::clamp(raw, 0.0f, 1.0f / 30.0f);
    }
    m.lastSimTime = time;

    auto boneNode = [&](const char* name) -> int {
        auto it = m.humanoid.find(name);
        return it == m.humanoid.end() ? -1 : it->second;
    };
    const size_t nn = m.nodes.size();
    if (m.poseRot.size() != nn) {
        m.poseRot.resize(nn);
        m.poseTrans.resize(nn);
        m.poseActive.assign(nn, 0);
        for (size_t i = 0; i < nn; ++i) {
            m.poseRot[i] = m.nodes[i].baseRotation;
            m.poseTrans[i] = m.nodes[i].translation;
        }
    }
    if (m.ikRot.size() != nn) {
        m.ikRot.assign(nn, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        m.ikActive.assign(nn, 0);
    }
    std::fill(m.ikActive.begin(), m.ikActive.end(), 0);
    std::vector<glm::quat> extra(nn, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

    // Head tracking: aim the head at the look target, clamped, in glTF model space.
    // Smoothed over frames so the motion reads as a gentle follow, not a snap.
    const glm::mat4 invRoot = glm::inverse(m.root);
    int head = boneNode("head");
    if (head >= 0) {
        glm::vec3 headLocal = glm::vec3(invRoot * m.nodes[head].world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        glm::vec3 targetLocal = glm::vec3(invRoot * glm::vec4(lookAtWorld, 1.0f));
        glm::vec3 d = targetLocal - headLocal;
        float yaw = std::atan2(d.x, d.z);
        float pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
        yaw = std::clamp(yaw, -0.9f, 0.9f);
        pitch = std::clamp(pitch, -0.35f, 0.35f);
        m.headYaw += (yaw - m.headYaw) * 0.12f;
        m.headPitch += (pitch - m.headPitch) * 0.12f;
        extra[head] = glm::angleAxis(m.headYaw, glm::vec3(0, 1, 0)) * glm::angleAxis(-m.headPitch, glm::vec3(1, 0, 0)) *
                      glm::angleAxis(m.faceNod, glm::vec3(1, 0, 0)) * glm::angleAxis(m.faceTilt, glm::vec3(0, 0, 1));
    }
    int chest = boneNode("chest");
    if (chest < 0) chest = boneNode("spine");
    if (chest >= 0) {
        float breath = 0.018f * std::sin(time * 1.9f);
        extra[chest] = glm::angleAxis(breath, glm::vec3(1, 0, 0));
    }

    // Reaction layer: hits and sway, applied on top of breathing and head tracking.
    if (m.reactionsEnabled && dt > 0.0f) stepReactions(m, dt);
    else m.swayForce = glm::vec3(0.0f);
    for (const auto& r : m.reactions) {
        float a = glm::length(r.angle);
        if (a > 1e-6f && r.node >= 0)
            extra[r.node] = glm::angleAxis(a, r.angle / a) * extra[r.node];
    }

    // Pass A: animation pose, no IK, no spring bones. The arm solver reads it.
    computeWorld(m, extra, dt, false, false);
    if (m.armCollision)
        for (int side = 0; side < 2; ++side) solveArm(m, side, dt, invRoot);
    // Rigging layers (Unity-style constraints) see the animated pose plus the arm solve.
    for (Rig& r : m.rigs) r.evaluate(m, dt, [&] { computeWorld(m, extra, dt, false, true); });
    // Pass B: final pose with the IK overlays and spring bones.
    computeWorld(m, extra, dt, true, true);

    // Joint palette: jointWorld * inverseBind (glTF 2.0; the mesh node's transform is ignored).
    for (auto& sk : m.skins) {
        JointUBO ubo{};
        for (auto& jm : ubo.joints) jm = glm::mat4(1.0f);
        for (size_t j = 0; j < sk.joints.size(); ++j)
            ubo.joints[j] = m.nodes[sk.joints[j]].world * sk.inverseBind[j];
        sk.ubo->upload(&ubo, sizeof(ubo));
    }
}

namespace {
void uploadMaterial(Material& mt) { mt.ubo->upload(&mt.params, sizeof(MaterialUBO)); }
}

void setCharacterFx(Model& m, const CharacterFx& fx) {
    for (auto& mt : m.materials) {
        MaterialUBO& p = mt->params;
        p.fx0 = glm::vec4(fx.dissolve, fx.dissolveEdge, fx.darken, fx.hit);
        p.fx1 = glm::vec4(fx.edgeColor, fx.upgradePulse);
        p.fx3 = glm::vec4(fx.hitColor, p.fx3.w);          // keep the occlusion exclusion flag
        p.fx4 = glm::vec4(fx.upgradeColor, 0.0f);
        p.outline = fx.selected ? glm::vec4(fx.selectColor, mt->outlineBase.w * 1.5f) : mt->outlineBase;
        uploadMaterial(*mt);
    }
}

int setHairFace(Model& m, const glm::vec3& anchorWorld, float radius) {
    int count = 0;
    for (auto& mt : m.materials) {
        std::string lower = mt->name;
        for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        if (lower.find("hair") == std::string::npos) continue;
        ++count;
        mt->params.fx2 = glm::vec4(anchorWorld, radius > 0.0f ? radius : 0.0f);
        uploadMaterial(*mt);
    }
    return count;
}

void setOccludable(Model& m, bool occludable) {
    for (auto& mt : m.materials) {
        mt->params.fx3.w = occludable ? 0.0f : 1.0f;
        uploadMaterial(*mt);
    }
}

void applyPlacement(Model& m) {
    const glm::mat4 turn = m.isVRM0 ? glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0))
                                    : glm::mat4(1.0f);
    m.root = m.placement * turn;
}

} // namespace ukiyo

namespace ukiyo {

int buildModelLods(rhi::Device& device, Model& model, const std::vector<float>& switches) {
    model.lodSwitch = switches;
    int built = 0;
    for (Primitive& p : model.primitives) {
        p.lods.clear();
        if (switches.empty() || p.cpuVertices.empty() || p.cpuIndices.size() < 3) continue;
        const size_t n = p.cpuIndices.size();
        size_t prevCount = n;
        for (size_t k = 1; k <= switches.size(); ++k) {
            size_t target = static_cast<size_t>(double(n) * std::pow(0.5, double(k)));
            target -= target % 3;
            if (target < 3) break;
            std::vector<uint32_t> out(n);
            float error = 0.0f;
            // target_error is a fraction of the mesh extent. 5% keeps the silhouette close at the switch distance.
            const size_t count = meshopt_simplify(out.data(), p.cpuIndices.data(), n, &p.cpuVertices[0].pos.x,
                                                  p.cpuVertices.size(), sizeof(rhi::Vertex), target, 0.05f,
                                                  meshopt_SimplifyLockBorder, &error);
            // The simplifier can stop early at the error limit. A level that does not shrink the previous one is
            // dropped, and the renderer uses the last built level for the distances beyond it.
            if (count < 3 || count >= prevCount) break;
            prevCount = count;
            out.resize(count);
            LodLevel lvl;
            lvl.indexCount = static_cast<uint32_t>(count);
            lvl.indices = device.createBuffer(rhi::BufferUsage::Index, count * sizeof(uint32_t));
            lvl.indices->upload(out.data(), count * sizeof(uint32_t));
            p.lods.push_back(std::move(lvl));
            ++built;
        }
    }
    return built;
}

int buildModelVGeom(rhi::Device& device, Model& model, const VGeomSettings& settings, const std::string& cachePath) {
    constexpr size_t kMinSourceTriangles = 256;
    // A skinned draw moves the vertices every frame, so the tree's rest-pose error is not the error on screen. This is
    // a static mesh feature; leave the model on its ordinary index buffer rather than cull it wrongly.
    for (const DrawItem& d : model.draws)
        if (d.skin >= 0) return 0;

    std::vector<uint32_t> candidates;
    std::vector<uint64_t> hashes;
    for (uint32_t i = 0; i < model.primitives.size(); ++i) {
        Primitive& p = model.primitives[i];
        p.vgeom = VGeomMesh{};
        p.vgeomIndices.reset();
        p.vgeomClusters.reset();
        p.vgeomChildren.reset();
        if (p.cpuVertices.size() < 3 || p.cpuIndices.size() < kMinSourceTriangles * 3 || !p.morphPos.empty()) continue;
        candidates.push_back(i);
        hashes.push_back(vgeomHash(&p.cpuVertices[0].pos.x, p.cpuVertices.size(), sizeof(rhi::Vertex),
                                   p.cpuIndices.data(), p.cpuIndices.size()));
    }
    if (candidates.empty()) return 0;

    std::vector<VGeomMesh> meshes(candidates.size());
    if (!cachePath.empty()) loadVGeomSet(meshes, cachePath, hashes, settings);

    bool builtAny = false;
    std::vector<VGeomMesh> finalMeshes(candidates.size());
    for (size_t k = 0; k < candidates.size(); ++k) {
        Primitive& p = model.primitives[candidates[k]];
        VGeomMesh mesh = std::move(meshes[k]);
        if (!mesh.valid() || mesh.sourceHash != hashes[k]) {
            mesh = buildVGeom(&p.cpuVertices[0].pos.x, p.cpuVertices.size(), sizeof(rhi::Vertex), p.cpuIndices.data(),
                              p.cpuIndices.size(), settings);
            builtAny = true;
        }
        if (!mesh.valid()) continue;
        finalMeshes[k] = mesh;   // kept for the cache write below; the primitive owns the moved copy
        p.vgeomIndices = device.createBuffer(rhi::BufferUsage::Index, mesh.indices.size() * sizeof(uint32_t));
        p.vgeomIndices->upload(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
        // The compute cut reads the tree from storage buffers (docs/VGEOM.md §7). The children stream is always
        // created — a single-cluster mesh has none, and a descriptor needs a real buffer either way.
        {
            std::vector<VGeomGpuCluster> gpu(mesh.clusters.size());
            for (size_t i = 0; i < mesh.clusters.size(); ++i) {
                const VGeomCluster& c = mesh.clusters[i];
                VGeomGpuCluster& g = gpu[i];
                g.centreRadius = glm::vec4(c.centre, c.radius);
                g.error = c.error;
                g.childFirst = c.childFirst;
                g.childCount = c.childCount;
                g.firstIndex = c.firstIndex;
                g.indexCount = c.indexCount;
                g.level = c.level;
                g.parent = c.parent;
            }
            p.vgeomClusters = device.createBuffer(rhi::BufferUsage::Storage, gpu.size() * sizeof(VGeomGpuCluster));
            p.vgeomClusters->upload(gpu.data(), gpu.size() * sizeof(VGeomGpuCluster));
            const size_t childBytes = std::max<size_t>(1, mesh.children.size()) * sizeof(uint32_t);
            p.vgeomChildren = device.createBuffer(rhi::BufferUsage::Storage, childBytes);
            if (!mesh.children.empty())
                p.vgeomChildren->upload(mesh.children.data(), mesh.children.size() * sizeof(uint32_t));
            else {
                const uint32_t zero = 0;
                p.vgeomChildren->upload(&zero, sizeof(zero));
            }
        }
        p.vgeom = std::move(mesh);
    }
    int built = 0;
    for (const VGeomMesh& m : finalMeshes)
        if (m.valid()) ++built;
    if (builtAny && !cachePath.empty()) saveVGeomSet(finalMeshes, cachePath, settings);
    return built;
}

void worldTriangles(const Model& model, std::vector<glm::vec3>* verts, std::vector<uint32_t>* tris) {
    for (const Node& n : model.nodes) {
        if (n.mesh < 0) continue;
        for (const Primitive& p : model.primitives) {
            if (p.mesh != n.mesh || p.cpuVertices.empty()) continue;
            const uint32_t base = static_cast<uint32_t>(verts->size());
            for (const rhi::Vertex& v : p.cpuVertices)
                verts->push_back(glm::vec3(n.world * glm::vec4(v.pos, 1.0f)));
            for (uint32_t i : p.cpuIndices) tris->push_back(base + i);
        }
    }
}

} // namespace ukiyo
