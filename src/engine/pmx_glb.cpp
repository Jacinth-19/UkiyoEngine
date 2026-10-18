// PMX 2.0 / 2.1 reader and GLB writer. Reads the sections the engine uses (header, vertices,
// faces, textures, materials, bones) and stops after the bones; morphs and later sections
// are not needed.
//
// Coordinates: PMX is left-handed with models facing -Z. The output is right-handed with
// models facing +Z (the engine's yaw 0 side), so Z is negated and triangles are reversed.
//
// Skinning: each draw is split into chunks that use at most kMaxJointsPerSkin joints. The
// inverse bind matrices are translations of the rest bone positions (PMX bones have no rest
// rotation), so the skinned rest pose equals the authored mesh.

#include "engine/pmx_glb.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ukiyo::pmx {
namespace {

struct Reader {
    const uint8_t* d;
    size_t n;
    size_t p = 0;

    void need(size_t k) const {
        if (k > n - p) throw std::runtime_error("PMX file ends early");
    }
    uint8_t u8() { need(1); return d[p++]; }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, d + p, 2); p += 2; return v; }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, d + p, 4); p += 4; return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() {
        need(4);
        float v;
        std::memcpy(&v, d + p, 4);
        p += 4;
        return v;
    }
    void skip(size_t k) { need(k); p += k; }
    void floats(float* out, int count) {
        for (int i = 0; i < count; ++i) out[i] = f32();
    }
    // Index of `size` bytes, unsigned (vertex indices).
    uint32_t uIndex(int size) {
        switch (size) {
            case 1: return u8();
            case 2: return u16();
            case 4: return u32();
        }
        throw std::runtime_error("PMX has an invalid unsigned index size");
    }
    // Index of `size` bytes, signed (bone, texture, material indices; -1 means none).
    int32_t sIndex(int size) {
        switch (size) {
            case 1: return i8();
            case 2: return i16();
            case 4: return i32();
        }
        throw std::runtime_error("PMX has an invalid signed index size");
    }
};

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Reads one PMX text: a 4-byte byte count, then UTF-16LE (encoding 0) or UTF-8 (encoding 1).
std::string readText(Reader& r, int encoding) {
    const int32_t len = r.i32();
    if (len < 0) throw std::runtime_error("PMX text has a negative length");
    r.need(static_cast<size_t>(len));
    const uint8_t* b = r.d + r.p;
    std::string out;
    if (encoding == 1) {
        out.assign(reinterpret_cast<const char*>(b), static_cast<size_t>(len));
    } else {
        for (size_t i = 0; i + 1 < static_cast<size_t>(len); i += 2) {
            uint32_t c = b[i] | (b[i + 1] << 8);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < static_cast<size_t>(len)) {
                const uint32_t c2 = b[i + 2] | (b[i + 3] << 8);
                if (c2 >= 0xDC00 && c2 < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                    i += 2;
                }
            }
            appendUtf8(out, c);
        }
    }
    r.skip(static_cast<size_t>(len));
    return out;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string jsonNum(double v) {
    if (!std::isfinite(v)) v = 0.0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

std::string jsonArray(const float* v, int n) {
    std::string out = "[";
    for (int i = 0; i < n; ++i) {
        if (i) out += ",";
        out += jsonNum(v[i]);
    }
    return out + "]";
}

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string mimeFor(const std::string& path) {
    const std::string ext = lowerExt(path);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "bmp") return "image/bmp";
    if (ext == "tga") return "image/x-tga";
    return "";
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct Vertex {
    float pos[3], nrm[3], uv[2];
    int bones[4];
    float weights[4];
};

struct Material {
    std::string name;
    float diffuse[4];
    uint8_t flags;
    int32_t texture;
    uint32_t faceCount;
};

struct Bone {
    std::string name;
    float pos[3];
    int32_t parent;
};

// One skinned primitive: a material's triangles that use at most kMaxJointsPerSkin bones.
struct Chunk {
    int32_t material = -1;
    std::vector<float> pos, nrm, uv, weights;
    std::vector<uint8_t> joints;
    std::vector<uint32_t> indices;
    std::vector<int> jointBones;   // global bone index per local joint slot
};

// Growing binary blob plus the JSON pieces that describe it.
struct GlbBuilder {
    std::vector<uint8_t> bin;
    std::vector<std::string> bufferViews, accessors;

    int view(const void* data, size_t bytes) {
        while (bin.size() % 4) bin.push_back(0);
        const size_t off = bin.size();
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bin.insert(bin.end(), p, p + bytes);
        bufferViews.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
                              ",\"byteLength\":" + std::to_string(bytes) + "}");
        return static_cast<int>(bufferViews.size()) - 1;
    }
    // componentType: 5121 ubyte, 5125 uint, 5126 float. type: SCALAR, VEC2, VEC3, VEC4, MAT4.
    int accessor(int view, int componentType, int count, const char* type,
                 const std::string& extra = "") {
        accessors.push_back("{\"bufferView\":" + std::to_string(view) +
                            ",\"componentType\":" + std::to_string(componentType) +
                            ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" +
                            extra + "}");
        return static_cast<int>(accessors.size()) - 1;
    }
};

std::string minMaxJson(const std::vector<float>& xyz) {
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < xyz.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], xyz[i + k]);
            mx[k] = std::max(mx[k], xyz[i + k]);
        }
    if (xyz.empty()) mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0.0f;
    return ",\"min\":" + jsonArray(mn, 3) + ",\"max\":" + jsonArray(mx, 3);
}

} // namespace

bool convertToGlb(const std::vector<uint8_t>& pmx, const TextureReader& readTexture,
                  std::vector<uint8_t>& glb, std::string& error, ImportReport* report,
                  const ImportOptions& options) {
    ImportReport rep;
    try {
        if (pmx.size() < 9 || std::memcmp(pmx.data(), "PMX ", 4) != 0) {
            error = "not a PMX file";
            return false;
        }
        Reader r{pmx.data(), pmx.size()};
        r.p = 4;
        const float version = r.f32();
        if (version < 1.99f || version >= 2.2f) {
            error = "unsupported PMX version";
            return false;
        }
        const uint8_t globalCount = r.u8();
        if (globalCount < 8) throw std::runtime_error("PMX globals are too short");
        std::vector<uint8_t> g(globalCount);
        for (auto& v : g) v = r.u8();
        const int encoding = g[0];         // 0 UTF-16LE, 1 UTF-8
        const int extraUv = g[1];
        const int vertexSize = g[2];
        const int textureSize = g[3];
        const int boneSize = g[5];
        if (encoding > 1) throw std::runtime_error("PMX text encoding is unknown");

        // Model names and comments (not used).
        for (int k = 0; k < 4; ++k) readText(r, encoding);

        // Vertices.
        const int32_t vertexCount = r.i32();
        if (vertexCount < 0) throw std::runtime_error("PMX vertex count is negative");
        std::vector<Vertex> verts(static_cast<size_t>(vertexCount));
        for (Vertex& v : verts) {
            r.floats(v.pos, 3);
            r.floats(v.nrm, 3);
            r.floats(v.uv, 2);
            r.skip(static_cast<size_t>(extraUv) * 16);
            const uint8_t type = r.u8();
            for (int k = 0; k < 4; ++k) { v.bones[k] = -1; v.weights[k] = 0.0f; }
            switch (type) {
                case 0:  // BDEF1
                    v.bones[0] = r.sIndex(boneSize);
                    v.weights[0] = 1.0f;
                    break;
                case 1:  // BDEF2
                case 3:  // SDEF (its correction vectors are not used; plain blend)
                    v.bones[0] = r.sIndex(boneSize);
                    v.bones[1] = r.sIndex(boneSize);
                    v.weights[0] = r.f32();
                    v.weights[1] = 1.0f - v.weights[0];
                    if (type == 3) r.skip(36);   // C, R0, R1 vectors
                    break;
                case 2:  // BDEF4
                case 4:  // QDEF (rest pose only, so the blend is the same)
                    for (int k = 0; k < 4; ++k) v.bones[k] = r.sIndex(boneSize);
                    for (int k = 0; k < 4; ++k) v.weights[k] = r.f32();
                    break;
                default:
                    throw std::runtime_error("PMX vertex has an unknown weight type");
            }
            r.f32();   // edge scale
        }

        // Faces: indices into the vertex list, three per triangle.
        const int32_t indexCount = r.i32();
        if (indexCount < 0 || indexCount % 3 != 0) throw std::runtime_error("PMX face count is not a multiple of 3");
        std::vector<uint32_t> indices(static_cast<size_t>(indexCount));
        for (auto& i : indices) {
            i = r.uIndex(vertexSize);
            if (i >= verts.size()) throw std::runtime_error("PMX face index is out of range");
        }

        // Texture paths.
        const int32_t textureCount = r.i32();
        std::vector<std::string> texturePaths(static_cast<size_t>(std::max(textureCount, 0)));
        for (auto& t : texturePaths) {
            t = readText(r, encoding);
            std::replace(t.begin(), t.end(), '\\', '/');
        }

        // Materials. Only the diffuse parts and the face count are kept.
        const int32_t materialCount = r.i32();
        std::vector<Material> materials(static_cast<size_t>(std::max(materialCount, 0)));
        uint64_t faceSum = 0;
        for (Material& m : materials) {
            const std::string jp = readText(r, encoding);
            const std::string en = readText(r, encoding);
            m.name = jp.empty() ? en : jp;
            r.floats(m.diffuse, 4);
            r.skip(12 + 4 + 12);     // specular colour, specularity, ambient
            m.flags = r.u8();
            r.skip(16 + 4);          // edge colour and size
            m.texture = r.sIndex(textureSize);
            r.sIndex(textureSize);   // sphere texture
            r.skip(1);               // sphere mode
            const uint8_t toonFlag = r.u8();
            if (toonFlag == 0) r.sIndex(textureSize);   // toon texture index
            else r.skip(1);                             // internal toon 0..9
            readText(r, encoding);   // memo
            const int32_t faces = r.i32();
            if (faces < 0) throw std::runtime_error("PMX material face count is negative");
            m.faceCount = static_cast<uint32_t>(faces);
            faceSum += m.faceCount;
        }
        if (faceSum != indices.size())
            throw std::runtime_error("PMX material face counts do not match the face count (layout misread)");

        // Bones. Each bone's optional parts are read in the order the format defines.
        const int32_t boneCount = r.i32();
        if (boneCount < 0) throw std::runtime_error("PMX bone count is negative");
        std::vector<Bone> bones(static_cast<size_t>(boneCount));
        for (int32_t b = 0; b < boneCount; ++b) {
            Bone& bone = bones[static_cast<size_t>(b)];
            bone.name = readText(r, encoding);
            readText(r, encoding);   // English name
            r.floats(bone.pos, 3);
            bone.parent = r.sIndex(boneSize);
            r.i32();                 // layer
            const uint16_t flags = r.u16();
            if (flags & 0x0001) r.sIndex(boneSize);   // tail is a bone
            else r.skip(12);                          // tail is an offset
            if (flags & 0x0300) {                     // append rotation or translation
                r.sIndex(boneSize);
                r.f32();
            }
            if (flags & 0x0400) r.skip(12);           // fixed axis
            if (flags & 0x0800) r.skip(24);           // local axes
            if (flags & 0x2000) r.i32();              // external parent key
            if (flags & 0x0020) {                     // IK
                r.sIndex(boneSize);
                r.i32();
                r.f32();
                const int32_t links = r.i32();
                for (int32_t k = 0; k < links; ++k) {
                    r.sIndex(boneSize);
                    if (r.u8()) r.skip(24);
                }
            }
        }
        for (Bone& bone : bones)
            if (bone.parent < 0 || bone.parent >= boneCount) bone.parent = -1;

        // ----- Conversion to right-handed, +Z-facing coordinates -----
        const float scale = options.unitScale;
        auto flipPos = [](const float* in, float* out) {
            out[0] = in[0];
            out[1] = in[1];
            out[2] = -in[2];
        };
        std::vector<float> bonePos(static_cast<size_t>(boneCount) * 3);
        for (int32_t b = 0; b < boneCount; ++b) flipPos(bones[static_cast<size_t>(b)].pos, &bonePos[static_cast<size_t>(b) * 3]);

        // Influences per vertex: valid bones with weight > 0, normalised; bone 0 if none.
        struct Influence { int bone; float w; };
        std::vector<std::vector<Influence>> influences(verts.size());
        for (size_t vi = 0; vi < verts.size(); ++vi) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const int b = verts[vi].bones[k];
                const float w = verts[vi].weights[k];
                if (b >= 0 && b < boneCount && w > 0.0f) {
                    influences[vi].push_back({b, w});
                    sum += w;
                }
            }
            if (influences[vi].empty() || sum <= 1e-6f) {
                influences[vi].clear();
                influences[vi].push_back({boneCount > 0 ? 0 : -1, 1.0f});
            } else {
                for (auto& inf : influences[vi]) inf.w /= sum;
            }
        }

        // Chunks: per material, triangles grouped while the joint count stays within the limit.
        std::vector<Chunk> chunks;
        Chunk cur;
        std::unordered_map<uint32_t, uint32_t> vmap;   // global vertex -> local vertex
        std::unordered_map<int, uint8_t> jmap;         // global bone -> local joint slot
        auto flush = [&]() {
            if (!cur.indices.empty()) chunks.push_back(std::move(cur));
            cur = Chunk();
            vmap.clear();
            jmap.clear();
        };
        uint32_t faceStart = 0;
        for (int32_t mi = 0; mi < materialCount; ++mi) {
            const Material& mat = materials[static_cast<size_t>(mi)];
            flush();
            cur.material = mi;
            for (uint32_t t = faceStart; t < faceStart + mat.faceCount; t += 3) {
                const uint32_t tri[3] = {indices[t], indices[t + 1], indices[t + 2]};
                // Bones this triangle needs that the chunk does not have yet.
                std::vector<int> need;
                for (uint32_t v : tri)
                    for (const Influence& inf : influences[v])
                        if (inf.bone >= 0 && !jmap.count(inf.bone) &&
                            std::find(need.begin(), need.end(), inf.bone) == need.end())
                            need.push_back(inf.bone);
                if (jmap.size() + need.size() > static_cast<size_t>(kMaxJointsPerSkin)) {
                    flush();
                    cur.material = mi;
                }
                uint32_t local[3];
                for (int k = 0; k < 3; ++k) {
                    const uint32_t v = tri[k];
                    auto it = vmap.find(v);
                    if (it != vmap.end()) {
                        local[k] = it->second;
                        continue;
                    }
                    const Vertex& src = verts[v];
                    local[k] = static_cast<uint32_t>(cur.pos.size() / 3);
                    vmap[v] = local[k];
                    float p[3], n[3];
                    flipPos(src.pos, p);
                    flipPos(src.nrm, n);
                    cur.pos.insert(cur.pos.end(), {p[0], p[1], p[2]});   // PMX units; the root carries the scale
                    cur.nrm.insert(cur.nrm.end(), {n[0], n[1], n[2]});
                    cur.uv.insert(cur.uv.end(), {src.uv[0], src.uv[1]});
                    uint8_t joint[4] = {0, 0, 0, 0};
                    float weight[4] = {0, 0, 0, 0};
                    for (size_t q = 0; q < influences[v].size() && q < 4; ++q) {
                        const int b = influences[v][q].bone;
                        if (b < 0) continue;
                        auto jt = jmap.find(b);
                        if (jt == jmap.end()) {
                            jt = jmap.emplace(b, static_cast<uint8_t>(cur.jointBones.size())).first;
                            cur.jointBones.push_back(b);
                        }
                        joint[q] = jt->second;
                        weight[q] = influences[v][q].w;
                    }
                    cur.joints.insert(cur.joints.end(), joint, joint + 4);
                    cur.weights.insert(cur.weights.end(), weight, weight + 4);
                }
                // Reversed winding, because of the Z flip.
                cur.indices.insert(cur.indices.end(), {local[0], local[2], local[1]});
            }
            faceStart += mat.faceCount;
            flush();
        }

        // ----- GLB assembly -----
        GlbBuilder bld;
        std::string materialJson, textureJson, imageJson;
        std::vector<int> textureOfPmx(texturePaths.size(), -1);
        int textureCountOut = 0;
        // Only diffuse textures are embedded: toon and sphere maps are not used.
        std::vector<bool> usedTexture(texturePaths.size(), false);
        for (const Material& m : materials)
            if (m.texture >= 0 && static_cast<size_t>(m.texture) < usedTexture.size()) usedTexture[static_cast<size_t>(m.texture)] = true;
        for (size_t ti = 0; ti < texturePaths.size(); ++ti) {
            if (!usedTexture[ti]) continue;
            std::vector<uint8_t> bytes;
            const std::string mime = mimeFor(texturePaths[ti]);
            if (mime.empty() || !readTexture(texturePaths[ti], bytes) || bytes.empty()) {
                rep.warnings.push_back("texture not loaded: " + texturePaths[ti]);
                continue;
            }
            const int view = bld.view(bytes.data(), bytes.size());
            const int image = textureCountOut;
            imageJson += (image ? "," : "") + std::string("{\"bufferView\":") + std::to_string(view) +
                         ",\"mimeType\":\"" + mime + "\",\"name\":" + jsonString(baseName(texturePaths[ti])) + "}";
            textureJson += (image ? "," : "") + std::string("{\"source\":") + std::to_string(image) + "}";
            textureOfPmx[ti] = image;
            ++textureCountOut;
        }
        rep.textures = textureCountOut;

        for (size_t mi = 0; mi < materials.size(); ++mi) {
            const Material& m = materials[mi];
            float base[4] = {std::clamp(m.diffuse[0], 0.0f, 1.0f), std::clamp(m.diffuse[1], 0.0f, 1.0f),
                             std::clamp(m.diffuse[2], 0.0f, 1.0f), std::clamp(m.diffuse[3], 0.0f, 1.0f)};
            std::string json = "{\"name\":" + jsonString(m.name);
            if (m.flags & 0x01) json += ",\"doubleSided\":true";
            if (base[3] < 0.999f) json += ",\"alphaMode\":\"BLEND\"";
            json += ",\"pbrMetallicRoughness\":{\"baseColorFactor\":" + jsonArray(base, 4) +
                    ",\"metallicFactor\":0,\"roughnessFactor\":0.8";
            if (m.texture >= 0 && static_cast<size_t>(m.texture) < textureOfPmx.size() && textureOfPmx[static_cast<size_t>(m.texture)] >= 0)
                json += ",\"baseColorTexture\":{\"index\":" + std::to_string(textureOfPmx[static_cast<size_t>(m.texture)]) + "}";
            json += "}}";
            materialJson += (mi ? "," : "") + json;
        }
        rep.materials = static_cast<int>(materials.size());
        rep.bones = boneCount;
        rep.primitives = static_cast<int>(chunks.size());

        std::vector<std::string> nodeJson;
        // Node 0: root (carries the unit scale). Nodes 1..boneCount: bones. Then one node per chunk.
        const int chunkNodeBase = 1 + boneCount;
        std::vector<std::vector<int>> boneChildren(static_cast<size_t>(boneCount));
        std::vector<int> rootChildren;
        for (int32_t b = 0; b < boneCount; ++b) {
            const int parent = bones[static_cast<size_t>(b)].parent;
            if (parent >= 0) boneChildren[static_cast<size_t>(parent)].push_back(1 + b);
            else rootChildren.push_back(1 + b);
        }
        for (size_t c = 0; c < chunks.size(); ++c) rootChildren.push_back(chunkNodeBase + static_cast<int>(c));

        auto childrenJson = [](const std::vector<int>& kids) {
            std::string s = "[";
            for (size_t i = 0; i < kids.size(); ++i) s += (i ? "," : "") + std::to_string(kids[i]);
            return s + "]";
        };
        const float rootScale[3] = {scale, scale, scale};
        nodeJson.push_back("{\"name\":\"PMX root\",\"children\":" + childrenJson(rootChildren) +
                           ",\"scale\":" + jsonArray(rootScale, 3) + "}");
        for (int32_t b = 0; b < boneCount; ++b) {
            const Bone& bone = bones[static_cast<size_t>(b)];
            float local[3];
            const float* p = &bonePos[static_cast<size_t>(b) * 3];
            if (bone.parent >= 0) {
                const float* pp = &bonePos[static_cast<size_t>(bone.parent) * 3];
                for (int k = 0; k < 3; ++k) local[k] = p[k] - pp[k];
            } else {
                for (int k = 0; k < 3; ++k) local[k] = p[k];
            }
            nodeJson.push_back("{\"name\":" + jsonString(bone.name) + ",\"translation\":" + jsonArray(local, 3) +
                               (boneChildren[static_cast<size_t>(b)].empty() ? std::string()
                                                                             : ",\"children\":" + childrenJson(boneChildren[static_cast<size_t>(b)])) +
                               "}");
        }

        std::string meshJson, skinJson;
        for (size_t c = 0; c < chunks.size(); ++c) {
            const Chunk& ch = chunks[c];
            const int count = static_cast<int>(ch.pos.size() / 3);
            const int vPos = bld.view(ch.pos.data(), ch.pos.size() * sizeof(float));
            const int aPos = bld.accessor(vPos, 5126, count, "VEC3", minMaxJson(ch.pos));
            const int vNrm = bld.view(ch.nrm.data(), ch.nrm.size() * sizeof(float));
            const int aNrm = bld.accessor(vNrm, 5126, count, "VEC3");
            const int vUv = bld.view(ch.uv.data(), ch.uv.size() * sizeof(float));
            const int aUv = bld.accessor(vUv, 5126, count, "VEC2");
            const int vJ = bld.view(ch.joints.data(), ch.joints.size());
            const int aJ = bld.accessor(vJ, 5121, count, "VEC4");
            const int vW = bld.view(ch.weights.data(), ch.weights.size() * sizeof(float));
            const int aW = bld.accessor(vW, 5126, count, "VEC4");
            const int vI = bld.view(ch.indices.data(), ch.indices.size() * sizeof(uint32_t));
            const int aI = bld.accessor(vI, 5125, static_cast<int>(ch.indices.size()), "SCALAR");

            // Inverse bind matrices: translations to minus the rest position of each joint.
            std::vector<float> ibm;
            ibm.reserve(ch.jointBones.size() * 16);
            std::vector<int> jointNodes;
            for (int b : ch.jointBones) {
                const float* p = &bonePos[static_cast<size_t>(b) * 3];
                const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p[0], -p[1], -p[2], 1};
                ibm.insert(ibm.end(), m, m + 16);
                jointNodes.push_back(1 + b);
            }
            const int vIbm = bld.view(ibm.data(), ibm.size() * sizeof(float));
            const int aIbm = bld.accessor(vIbm, 5126, static_cast<int>(ch.jointBones.size()), "MAT4");

            std::string material = ch.material >= 0 ? ",\"material\":" + std::to_string(ch.material) : "";
            meshJson += (c ? "," : "") + std::string("{\"primitives\":[{\"attributes\":{\"POSITION\":") + std::to_string(aPos) +
                        ",\"NORMAL\":" + std::to_string(aNrm) + ",\"TEXCOORD_0\":" + std::to_string(aUv) +
                        ",\"JOINTS_0\":" + std::to_string(aJ) + ",\"WEIGHTS_0\":" + std::to_string(aW) +
                        "},\"indices\":" + std::to_string(aI) + material + "}]}";
            skinJson += (c ? "," : "") + std::string("{\"joints\":") + childrenJson(jointNodes) +
                        ",\"inverseBindMatrices\":" + std::to_string(aIbm) + "}";
            nodeJson.push_back("{\"name\":\"chunk " + std::to_string(c) + "\",\"mesh\":" + std::to_string(c) +
                               ",\"skin\":" + std::to_string(c) + "}");
            rep.triangles += static_cast<int>(ch.indices.size() / 3);
        }

        // Pad the binary chunk to a multiple of 4 bytes.
        while (bld.bin.size() % 4) bld.bin.push_back(0);

        auto join = [](const std::vector<std::string>& items) {
            std::string s;
            for (size_t i = 0; i < items.size(); ++i) s += (i ? "," : "") + items[i];
            return s;
        };
        std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"ukiyo PMX import\"},\"scene\":0,"
                           "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + join(nodeJson) + "]";
        if (!chunks.empty()) json += ",\"meshes\":[" + meshJson + "],\"skins\":[" + skinJson + "]";
        if (!materialJson.empty()) json += ",\"materials\":[" + materialJson + "]";
        if (!textureJson.empty()) json += ",\"textures\":[" + textureJson + "],\"images\":[" + imageJson + "]";
        json += ",\"accessors\":[" + join(bld.accessors) + "],\"bufferViews\":[" + join(bld.bufferViews) + "]";
        json += ",\"buffers\":[{\"byteLength\":" + std::to_string(bld.bin.size()) + "}]}";
        while (json.size() % 4) json += ' ';

        const uint32_t jsonLen = static_cast<uint32_t>(json.size());
        const uint32_t binLen = static_cast<uint32_t>(bld.bin.size());
        const uint32_t total = 12 + 8 + jsonLen + 8 + binLen;
        glb.clear();
        glb.reserve(total);
        auto put32 = [&glb](uint32_t v) {
            for (int k = 0; k < 4; ++k) glb.push_back(static_cast<uint8_t>(v >> (8 * k)));
        };
        put32(0x46546C67);   // "glTF"
        put32(2);
        put32(total);
        put32(jsonLen);
        put32(0x4E4F534A);   // "JSON"
        glb.insert(glb.end(), json.begin(), json.end());
        put32(binLen);
        put32(0x004E4942);   // "BIN"
        glb.insert(glb.end(), bld.bin.begin(), bld.bin.end());

        if (report) *report = rep;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace ukiyo::pmx
