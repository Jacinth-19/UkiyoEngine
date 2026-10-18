#include "engine/vfs_cgltf.h"
#include "engine/anim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "engine/model.h"

#include "cgltf.h"

namespace ukiyo {
namespace {

// Name tokens that swap sides for mirrored clips.
const std::pair<std::string, std::string> kMirrorSwaps[] = {
    {"_l", "_r"}, {"_r", "_l"}, {"_L", "_R"}, {"_R", "_L"}, {"Left", "Right"}, {"Right", "Left"}, {"left", "right"}, {"right", "left"},
};

// Universal Animation Library (Quaternius) source bone -> VRM humanoid bone.
struct BoneName { const char* src; const char* humanoid; };
const BoneName kUalBones[] = {
    {"pelvis", "hips"},          {"spine_01", "spine"},        {"spine_02", "chest"},
    {"spine_03", "upperChest"},  {"neck_01", "neck"},          {"Head", "head"},
    {"clavicle_l", "leftShoulder"}, {"upperarm_l", "leftUpperArm"}, {"lowerarm_l", "leftLowerArm"},
    {"hand_l", "leftHand"},      {"thigh_l", "leftUpperLeg"},  {"calf_l", "leftLowerLeg"},
    {"foot_l", "leftFoot"},      {"ball_l", "leftToes"},
    {"clavicle_r", "rightShoulder"}, {"upperarm_r", "rightUpperArm"}, {"lowerarm_r", "rightLowerArm"},
    {"hand_r", "rightHand"},     {"thigh_r", "rightUpperLeg"}, {"calf_r", "rightLowerLeg"},
    {"foot_r", "rightFoot"},     {"ball_r", "rightToes"},
    {"index_01_l", "leftIndexProximal"},  {"index_02_l", "leftIndexIntermediate"},  {"index_03_l", "leftIndexDistal"},
    {"middle_01_l", "leftMiddleProximal"}, {"middle_02_l", "leftMiddleIntermediate"}, {"middle_03_l", "leftMiddleDistal"},
    {"ring_01_l", "leftRingProximal"},    {"ring_02_l", "leftRingIntermediate"},    {"ring_03_l", "leftRingDistal"},
    {"pinky_01_l", "leftLittleProximal"}, {"pinky_02_l", "leftLittleIntermediate"}, {"pinky_03_l", "leftLittleDistal"},
    {"thumb_01_l", "leftThumbMetacarpal"}, {"thumb_02_l", "leftThumbProximal"},   {"thumb_03_l", "leftThumbDistal"},
    {"index_01_r", "rightIndexProximal"},  {"index_02_r", "rightIndexIntermediate"},  {"index_03_r", "rightIndexDistal"},
    {"middle_01_r", "rightMiddleProximal"}, {"middle_02_r", "rightMiddleIntermediate"}, {"middle_03_r", "rightMiddleDistal"},
    {"ring_01_r", "rightRingProximal"},    {"ring_02_r", "rightRingIntermediate"},    {"ring_03_r", "rightRingDistal"},
    {"pinky_01_r", "rightLittleProximal"}, {"pinky_02_r", "rightLittleIntermediate"}, {"pinky_03_r", "rightLittleDistal"},
    {"thumb_01_r", "rightThumbMetacarpal"}, {"thumb_02_r", "rightThumbProximal"},   {"thumb_03_r", "rightThumbDistal"},
};

glm::quat quatFromGltf(const float* xyzw) { return glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]); }

// Shortest-arc blend of two quaternions.
glm::quat qslerp(const glm::quat& a, const glm::quat& b, float t) {
    return glm::normalize(glm::slerp(a, b, t));
}

float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Index of the last key <= t (clamped to [0, n-2] for interpolation).
size_t keyIndex(const std::vector<float>& times, float t) {
    auto it = std::upper_bound(times.begin(), times.end(), t);
    size_t k = it == times.begin() ? 0 : static_cast<size_t>(it - times.begin() - 1);
    return std::min(k, times.size() - 1);
}

} // namespace

std::unique_ptr<AnimLibrary> AnimLibrary::load(const std::string& path) {
    cgltf_options opts{};
    vfs::useForCgltf(opts);
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success)
        throw std::runtime_error("cgltf could not parse animation library " + path);
    std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(data, cgltf_free);
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success)
        throw std::runtime_error("cgltf could not load buffers of " + path);

    auto lib = std::make_unique<AnimLibrary>();
    lib->path_ = path;
    SourceRig& rig = lib->rig_;
    const size_t nn = data->nodes_count;
    rig.names.resize(nn);
    rig.parent.assign(nn, -1);
    rig.restT.assign(nn, glm::vec3(0.0f));
    rig.restR.assign(nn, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    rig.restS.assign(nn, glm::vec3(1.0f));
    for (size_t i = 0; i < nn; ++i) {
        const cgltf_node& cn = data->nodes[i];
        rig.names[i] = cn.name ? cn.name : "";
        rig.parent[i] = cn.parent ? static_cast<int>(cn.parent - data->nodes) : -1;
        if (cn.has_matrix) {
            glm::vec3 skew, t, s;
            glm::vec4 persp;
            glm::quat r;
            glm::mat4 m = glm::make_mat4(cn.matrix);
            glm::decompose(m, s, r, t, skew, persp);
            rig.restT[i] = t;
            rig.restR[i] = r;
            rig.restS[i] = s;
        } else {
            if (cn.has_translation) rig.restT[i] = glm::make_vec3(cn.translation);
            if (cn.has_rotation) rig.restR[i] = quatFromGltf(cn.rotation);
            if (cn.has_scale) rig.restS[i] = glm::make_vec3(cn.scale);
        }
    }

    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& ca = data->animations[a];
        AnimClip clip;
        clip.name = ca.name ? ca.name : ("anim_" + std::to_string(a));
        for (cgltf_size c = 0; c < ca.channels_count; ++c) {
            const cgltf_animation_channel& ch = ca.channels[c];
            if (!ch.target_node || !ch.sampler || ch.target_path == cgltf_animation_path_type_weights) continue;
            const cgltf_animation_sampler& sm = *ch.sampler;
            AnimTrack tr;
            tr.node = static_cast<int>(ch.target_node - data->nodes);
            tr.path = ch.target_path == cgltf_animation_path_type_translation ? AnimTrack::Path::Translation
                    : ch.target_path == cgltf_animation_path_type_scale       ? AnimTrack::Path::Scale
                                                                              : AnimTrack::Path::Rotation;
            tr.interp = sm.interpolation == cgltf_interpolation_type_step           ? AnimTrack::Interp::Step
                      : sm.interpolation == cgltf_interpolation_type_cubic_spline ? AnimTrack::Interp::Cubic
                                                                                  : AnimTrack::Interp::Linear;
            const cgltf_size keys = sm.input->count;
            tr.times.resize(keys);
            for (cgltf_size k = 0; k < keys; ++k) cgltf_accessor_read_float(sm.input, k, &tr.times[k], 1);
            const int comps = tr.path == AnimTrack::Path::Rotation ? 4 : 3;
            const cgltf_size outCount = sm.output->count;
            tr.values.resize(outCount * comps);
            for (cgltf_size k = 0; k < outCount; ++k)
                cgltf_accessor_read_float(sm.output, k, &tr.values[k * comps], comps);
            if (keys == 0) continue;
            clip.duration = std::max(clip.duration, tr.times.back());
            clip.tracks.push_back(std::move(tr));
        }
        lib->clips_.push_back(std::move(clip));
    }
    lib->source_ = lib->clips_;
    return lib;
}

int AnimLibrary::find(const std::string& name) const {
    for (size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].name == name) return static_cast<int>(i);
    return -1;
}

void sampleClip(const AnimClip& clip, const SourceRig& rig, float t, std::vector<glm::vec3>& outT,
                std::vector<glm::quat>& outR) {
    outT = rig.restT;
    outR = rig.restR;
    float tl = t;
    if (clip.duration > 0.0f) {
        tl = std::fmod(t, clip.duration);
        if (tl < 0.0f) tl += clip.duration;
    }
    for (const AnimTrack& tr : clip.tracks) {
        // Scale tracks are not used by the retargeter (rest scale is kept).
        if (tr.path == AnimTrack::Path::Scale) continue;
        if (tr.node < 0 || tr.node >= static_cast<int>(outT.size())) continue;
        const size_t n = tr.times.size();
        const size_t k = keyIndex(tr.times, tl);
        const size_t k1 = std::min(k + 1, n - 1);
        const bool isRot = tr.path == AnimTrack::Path::Rotation;
        const int comps = isRot ? 4 : 3;
        auto valueAt = [&](size_t key, int which) -> const float* {
            // Cubic splines store (in-tangent, value, out-tangent) per key.
            const size_t idx = tr.interp == AnimTrack::Interp::Cubic ? key * 3 + which : key;
            return &tr.values[idx * comps];
        };
        if (tr.interp == AnimTrack::Interp::Step || n == 1 || k == k1) {
            const float* v = valueAt(k, 1);
            if (isRot) outR[tr.node] = glm::normalize(quatFromGltf(v));
            else outT[tr.node] = glm::make_vec3(v);
            continue;
        }
        const float t0 = tr.times[k], t1 = tr.times[k1];
        const float dt = t1 - t0;
        const float s = dt > 1e-9f ? std::clamp((tl - t0) / dt, 0.0f, 1.0f) : 0.0f;
        if (tr.interp == AnimTrack::Interp::Cubic) {
            const float* p0 = valueAt(k, 1);
            const float* m0 = valueAt(k, 2);   // out-tangent of key k
            const float* p1 = valueAt(k1, 1);
            const float* m1 = valueAt(k1, 0);  // in-tangent of key k1
            const float s2 = s * s, s3 = s2 * s;
            const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s;
            const float h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
            float v[4] = {0, 0, 0, 0};
            for (int c = 0; c < comps; ++c)
                v[c] = h00 * p0[c] + h10 * dt * m0[c] + h01 * p1[c] + h11 * dt * m1[c];
            if (isRot) outR[tr.node] = glm::normalize(quatFromGltf(v));
            else outT[tr.node] = glm::vec3(v[0], v[1], v[2]);
        } else {
            const float* a = valueAt(k, 0);
            const float* b = valueAt(k1, 0);
            if (isRot) {
                outR[tr.node] = qslerp(quatFromGltf(a), quatFromGltf(b), s);
            } else {
                outT[tr.node] = glm::mix(glm::make_vec3(a), glm::make_vec3(b), s);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// AnimPlayer

std::vector<IdleVariant> AnimPlayer::defaultIdleVariants() {
    // UAL1 has two standing idles without props. The hand-on-waist variants reuse them with
    // the waist IK goal (side chosen at random). Prop idles (sword, torch, pistol) are left out.
    return {
        {"Idle_Loop", 2, 4, false},
        {"Idle_Talking_Loop", 2, 3, false},
        {"Idle_Loop", 2, 4, true},
        {"Idle_Talking_Loop", 2, 3, true},
    };
}

void AnimPlayer::bind(const Model& model, const AnimLibrary& lib) {
    lib_ = &lib;
    const SourceRig& rig = lib.rig();
    const size_t nt = model.nodes.size();
    const size_t ns = rig.names.size();

    // Bone map: source node -> target node via humanoid names.
    srcForTarget_.assign(nt, -1);
    std::vector<int> srcNodeByName(ns, -1);
    for (size_t s = 0; s < ns; ++s) {
        for (const BoneName& bn : kUalBones) {
            if (rig.names[s] != bn.src) continue;
            auto it = model.humanoid.find(bn.humanoid);
            if (it != model.humanoid.end() && it->second >= 0 && it->second < static_cast<int>(nt))
                srcForTarget_[it->second] = static_cast<int>(s);
        }
    }

    frame_ = model.isVRM0 ? glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f))
                          : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    // Target rest local TRS (decompose matrix nodes) and rest world transforms.
    targetRestT_.assign(nt, glm::vec3(0.0f));
    targetRestR_.assign(nt, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    for (size_t i = 0; i < nt; ++i) {
        const Node& n = model.nodes[i];
        if (n.hasMatrix) {
            glm::vec3 skew, s;
            glm::vec4 persp;
            glm::decompose(n.matrix, s, targetRestR_[i], targetRestT_[i], skew, persp);
        } else {
            targetRestT_[i] = n.translation;
            targetRestR_[i] = n.baseRotation;
        }
    }
    targetWorldRestP_.assign(nt, glm::vec3(0.0f));
    targetWorldRestR_.assign(nt, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    for (int i : model.updateOrder) {
        const int p = model.nodes[i].parent;
        const glm::vec3 pp = p >= 0 ? targetWorldRestP_[p] : glm::vec3(0.0f);
        const glm::quat pr = p >= 0 ? targetWorldRestR_[p] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        targetWorldRestP_[i] = pp + pr * targetRestT_[i];
        targetWorldRestR_[i] = pr * targetRestR_[i];
    }

    // Source hierarchy: parents-before-children order and rest world transforms.
    srcOrder_.clear();
    std::vector<bool> done(ns, false);
    std::function<void(int)> visit = [&](int i) {
        if (done[i]) return;
        if (rig.parent[i] >= 0) visit(rig.parent[i]);
        done[i] = true;
        srcOrder_.push_back(i);
    };
    for (size_t i = 0; i < ns; ++i) visit(static_cast<int>(i));

    // Mirror pairs: a bone pairs with the bone whose name has the other side's token (_l/_r, _L/_R, Left/Right).
    // Bones with no partner pair with themselves (central bones).
    srcMirror_.assign(ns, -1);
    for (size_t s = 0; s < ns; ++s) {
        srcMirror_[s] = static_cast<int>(s);
        const std::string cand = mirrorBoneCandidate(rig.names[s]);
        if (cand.empty()) continue;
        for (size_t t = 0; t < ns; ++t)
            if (rig.names[t] == cand) { srcMirror_[s] = static_cast<int>(t); break; }
    }
    sourceWorldRestP_.assign(ns, glm::vec3(0.0f));
    sourceWorldRestR_.assign(ns, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    for (int i : srcOrder_) {
        const int p = rig.parent[i];
        const glm::vec3 pp = p >= 0 ? sourceWorldRestP_[p] : glm::vec3(0.0f);
        const glm::quat pr = p >= 0 ? sourceWorldRestR_[p] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        sourceWorldRestP_[i] = pp + pr * rig.restT[i];
        sourceWorldRestR_[i] = pr * rig.restR[i];
    }

    // Hips: translation scale from leg length (hips to foot, in model space).
    hipsNode_ = -1;
    auto hipsIt = model.humanoid.find("hips");
    auto footIt = model.humanoid.find("leftFoot");
    int srcPelvis = -1, srcFoot = -1;
    for (size_t s = 0; s < ns; ++s) {
        if (rig.names[s] == "pelvis") srcPelvis = static_cast<int>(s);
        if (rig.names[s] == "foot_l") srcFoot = static_cast<int>(s);
    }
    if (hipsIt != model.humanoid.end() && footIt != model.humanoid.end() && srcPelvis >= 0 && srcFoot >= 0) {
        hipsNode_ = hipsIt->second;
        const float tLeg = targetWorldRestP_[hipsNode_].y - targetWorldRestP_[footIt->second].y;
        const float sLeg = sourceWorldRestP_[srcPelvis].y - sourceWorldRestP_[srcFoot].y;
        hipScale_ = sLeg > 1e-4f ? std::clamp(tLeg / sLeg, 0.2f, 3.0f) : 1.0f;
    }
    int mapped = 0;
    for (int s : srcForTarget_) mapped += s >= 0;
    std::printf("anim: bound %d bones, hips scale %.3f\n", mapped, hipScale_);
    bound_ = true;
}

void AnimPlayer::beginClip(int clipIndex, float now, float fade, float startFraction) {
    const AnimClip& clip = lib_->clips()[clipIndex];
    const int next = 1 - activeLayer_;
    // The outgoing layer keeps its own clock and fades out over `fade` seconds.
    if (layers_[activeLayer_].active) layers_[activeLayer_].fadeTime = fade;
    Layer& l = layers_[next];
    l = Layer{};   // a reused layer must not keep blend members from an earlier blend
    l.clip = clipIndex;
    // The loop clock starts `startFraction` of the way into the clip, so the clip begins mid-way.
    l.start = now - std::clamp(startFraction, 0.0f, 1.0f) * std::max(clip.duration, 0.0f);
    l.fadeFrom = now;
    l.fadeTime = std::max(fade, 1e-3f);
    l.active = true;
    activeLayer_ = next;
    currentName_ = clip.name;
    changes_++;
}

void AnimPlayer::play(const std::string& clip, float fade, float startFraction) {
    idle_ = false;
    int ci = lib_ ? lib_->find(clip) : -1;
    if (ci < 0) throw std::runtime_error("animation clip not found: " + clip);
    beginClip(ci, lastTime_, fade, startFraction);
}

glm::quat mirrorRotation(const glm::quat& q) { return glm::quat(q.w, q.x, -q.y, -q.z); }
glm::vec3 mirrorTranslation(const glm::vec3& v) { return glm::vec3(-v.x, v.y, v.z); }

std::string mirrorBoneCandidate(const std::string& name) {
    for (const auto& sw : kMirrorSwaps) {
        const size_t at = name.find(sw.first);
        if (at == std::string::npos) continue;
        return name.substr(0, at) + sw.second + name.substr(at + sw.first.size());
    }
    return std::string();
}

void AnimPlayer::playBlend(const std::vector<std::string>& clips, const std::vector<float>& weights, float fade,
                           const std::vector<MotionOptions>& options) {
    idle_ = false;
    if (!lib_) throw std::runtime_error("animation library not bound");
    if (clips.empty()) throw std::runtime_error("blend has no clips");
    std::vector<int> ids;
    ids.reserve(clips.size());
    for (const std::string& c : clips) {
        const int ci = lib_->find(c);
        if (ci < 0) throw std::runtime_error("animation clip not found: " + c);
        ids.push_back(ci);
    }
    const int next = 1 - activeLayer_;
    if (layers_[activeLayer_].active) layers_[activeLayer_].fadeTime = fade;
    Layer& l = layers_[next];
    l = Layer{};
    std::string name;
    for (size_t k = 0; k < ids.size(); ++k) {
        const MotionOptions opt = k < options.size() ? options[k] : MotionOptions{};
        Member mem;
        mem.clip = ids[k];
        mem.start = lastTime_;
        mem.timeScale = opt.timeScale > 0.0f ? opt.timeScale : 1.0f;
        mem.mirror = opt.mirror;
        l.members.push_back(mem);
        l.weights.push_back(k < weights.size() ? std::max(weights[k], 0.0f) : 0.0f);
        if (!name.empty()) name += " + ";
        name += lib_->clips()[ids[k]].name;
    }
    l.fadeFrom = lastTime_;
    l.fadeTime = std::max(fade, 1e-3f);
    l.active = true;
    activeLayer_ = next;
    currentName_ = name;
    changes_++;
}

void AnimPlayer::setBlendWeights(const std::vector<float>& weights) {
    Layer& l = layers_[activeLayer_];
    if (!l.active || l.members.empty()) return;
    for (size_t k = 0; k < l.weights.size(); ++k)
        l.weights[k] = k < weights.size() ? std::max(weights[k], 0.0f) : 0.0f;
}

void AnimPlayer::startIdle(uint32_t seed, std::vector<IdleVariant> variants, float fade) {
    idle_ = true;
    rng_ = seed * 2654435761u + 0x9E3779B9u;
    if (rng_ == 0) rng_ = 1;
    variants_ = variants.empty() ? defaultIdleVariants() : std::move(variants);
    idleFade_ = fade;
    variantIndex_ = -1;
    loopsLeft_ = 0;
    pendingStart_ = true;
}

void AnimPlayer::stop() {
    idle_ = false;
    for (Layer& l : layers_) l = Layer{};
    activeLayer_ = 0;
    currentName_.clear();
}

uint32_t AnimPlayer::nextRandom() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void AnimPlayer::pickNextVariant(float now) {
    int n = static_cast<int>(variants_.size());
    if (n == 0) return;
    int pick = variantIndex_;
    if (n == 1) pick = 0;
    else {
        do pick = static_cast<int>(nextRandom() % static_cast<uint32_t>(n));
        while (pick == variantIndex_);
    }
    variantIndex_ = pick;
    const IdleVariant& v = variants_[pick];
    const int ci = lib_->find(v.clip);
    if (ci < 0) throw std::runtime_error("idle clip not found: " + v.clip);
    const int span = std::max(0, v.maxLoops - v.minLoops);
    loopsLeft_ = v.minLoops + (span > 0 ? static_cast<int>(nextRandom() % static_cast<uint32_t>(span + 1)) : 0);
    handOnHipWanted_ = v.handOnHip;
    if (v.handOnHip) handSide_ = static_cast<int>(nextRandom() & 1u);
    beginClip(ci, now, idleFade_);
    loopEnd_ = now + lib_->clips()[ci].duration * static_cast<float>(loopsLeft_);
    std::printf("anim: t=%.2fs variant %d %s, %d loops%s\n", now, pick, v.clip.c_str(), loopsLeft_,
                v.handOnHip ? (handSide_ ? ", hand on left hip" : ", hand on right hip") : "");
}

void AnimPlayer::update(Model& model, float time, float dt) {
    if (!bound_) return;
    lastTime_ = time;
    if (idle_) {
        if (pendingStart_) {
            pendingStart_ = false;
            pickNextVariant(time);
        } else if (layers_[activeLayer_].active && time >= loopEnd_) {
            pickNextVariant(time);
        }
    }

    // Hand-on-waist goal levels: ease toward the current variant's request.
    const float k = 1.0f - std::exp(-std::max(dt, 0.0f) * 3.0f);
    for (int side = 0; side < 2; ++side) {
        float want = (idle_ && handOnHipWanted_ && handSide_ == side) || forcedHip_ == side ? 1.0f : 0.0f;
        hipLevel_[side] += (want - hipLevel_[side]) * k;
        model.arms[side].hipWeight = hipLevel_[side];
    }

    blendPose(model, time, dt);
}

void AnimPlayer::blendPose(Model& model, float now, float dt) {
    (void)dt;
    const SourceRig& rig = lib_->rig();
    const size_t nt = model.nodes.size();
    const size_t ns = rig.names.size();

    // Layer weights: the active clip fades in, the outgoing clip fades out.
    float wCur = 1.0f;
    const Layer& cur = layers_[activeLayer_];
    if (!cur.active) {
        // Nothing playing: keep the rest pose.
        model.poseRot.resize(nt);
        model.poseTrans.resize(nt);
        for (size_t i = 0; i < nt; ++i) {
            model.poseRot[i] = model.nodes[i].baseRotation;
            model.poseTrans[i] = model.nodes[i].translation;
        }
        model.poseActive.assign(nt, 0);
        return;
    }
    if (cur.fadeTime > 0.0f) wCur = smooth01((now - cur.fadeFrom) / cur.fadeTime);
    const Layer& prev = layers_[1 - activeLayer_];
    float wPrev = 0.0f;
    if (prev.active) {
        wPrev = 1.0f - wCur;
        if (wPrev <= 1e-4f) {
            layers_[1 - activeLayer_] = Layer{};
            wPrev = 0.0f;
        }
    }
    wCur = 1.0f - wPrev;

    // Per layer: retargeted local pose for every target node.
    std::vector<glm::vec3> srcT;
    std::vector<glm::quat> srcR;
    std::vector<glm::vec3> srcWP(ns);
    std::vector<glm::quat> srcWR(ns);
    std::vector<glm::vec3> tWP(nt);
    std::vector<glm::quat> tWR(nt);

    // One clip's pose, retargeted onto the model, at clip time t.
    auto clipPose = [&](int clipIndex, float t, std::vector<glm::quat>& outR, std::vector<glm::vec3>& outT, bool mirror) {
        const AnimClip& clip = lib_->clips()[clipIndex];
        sampleClip(clip, rig, t, srcT, srcR);
        if (mirror) {
            // Each side takes the other side's sampled pose, reflected; central bones reflect in place.
            std::vector<glm::vec3> mT(srcT.size());
            std::vector<glm::quat> mR(srcR.size());
            for (size_t i = 0; i < mR.size(); ++i) {
                const size_t j = (i < srcMirror_.size() && srcMirror_[i] >= 0) ? static_cast<size_t>(srcMirror_[i]) : i;
                mR[i] = mirrorRotation(srcR[j]);
                mT[i] = mirrorTranslation(srcT[j]);
            }
            srcR.swap(mR);
            srcT.swap(mT);
        }
        for (int i : srcOrder_) {
            const int p = rig.parent[i];
            const glm::vec3 pp = p >= 0 ? srcWP[p] : glm::vec3(0.0f);
            const glm::quat pr = p >= 0 ? srcWR[p] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            srcWP[i] = pp + pr * srcT[i];
            srcWR[i] = pr * srcR[i];
        }
        outR.resize(nt);
        outT.resize(nt);
        for (int n : model.updateOrder) {
            const int p = model.nodes[n].parent;
            const glm::vec3 pp = p >= 0 ? tWP[p] : glm::vec3(0.0f);
            const glm::quat pr = p >= 0 ? tWR[p] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            glm::quat lr = targetRestR_[n];
            glm::vec3 lt = targetRestT_[n];
            const int s = srcForTarget_[n];
            if (s >= 0) {
                // World-space delta from the source rest, applied to the target rest.
                const glm::quat dqSrc = srcWR[s] * glm::inverse(sourceWorldRestR_[s]);
                const glm::quat dq = glm::normalize(frame_ * dqSrc * glm::inverse(frame_));
                const glm::quat desiredR = glm::normalize(dq * targetWorldRestR_[n]);
                lr = glm::normalize(glm::inverse(pr) * desiredR);
                if (n == hipsNode_) {
                    const glm::vec3 desiredP = targetWorldRestP_[n] + hipScale_ * (frame_ * (srcWP[s] - sourceWorldRestP_[s]));
                    lt = glm::inverse(pr) * (desiredP - pp);

                }
            }
            outR[n] = lr;
            outT[n] = lt;
            tWR[n] = pr * lr;
            tWP[n] = pp + pr * lt;
        }
    };

    // A layer's pose. A single clip is one pose. A blend mixes its members per bone by their weights.
    auto layerPose = [&](const Layer& l, std::vector<glm::quat>& outR, std::vector<glm::vec3>& outT) {
        if (l.members.empty()) {
            clipPose(l.clip, now - l.start, outR, outT, false);
            return;
        }
        std::vector<std::vector<glm::quat>> memR;
        std::vector<std::vector<glm::vec3>> memT;
        std::vector<float> w;
        for (size_t k = 0; k < l.members.size(); ++k) {
            const float wk = k < l.weights.size() ? l.weights[k] : 0.0f;
            if (wk <= 0.0f) continue;
            memR.emplace_back();
            memT.emplace_back();
            clipPose(l.members[k].clip, (now - l.members[k].start) * l.members[k].timeScale, memR.back(), memT.back(),
                     l.members[k].mirror);
            w.push_back(wk);
        }
        if (w.empty()) {
            // Every weight is zero: hold the first member's pose rather than leave the model unposed.
            clipPose(l.members[0].clip, (now - l.members[0].start) * l.members[0].timeScale, outR, outT,
                     l.members[0].mirror);
            return;
        }
        float total = 0.0f;
        for (float v : w) total += v;
        outR.resize(nt);
        outT.resize(nt);
        std::vector<glm::quat> q(w.size());
        for (size_t n = 0; n < nt; ++n) {
            glm::vec3 t(0.0f);
            for (size_t j = 0; j < w.size(); ++j) {
                q[j] = memR[j][n];
                t += w[j] * memT[j][n];
            }
            outR[n] = blendRotations(q, w);
            outT[n] = t / total;
        }
    };

    std::vector<glm::quat> rCur, rPrev;
    std::vector<glm::vec3> tCur, tPrev;
    layerPose(cur, rCur, tCur);
    if (wPrev > 0.0f) layerPose(prev, rPrev, tPrev);

    model.poseRot.resize(nt);
    model.poseTrans.resize(nt);
    model.poseActive.assign(nt, 1);
    for (size_t n = 0; n < nt; ++n) {
        if (wPrev > 0.0f) {
            model.poseRot[n] = qslerp(rPrev[n], rCur[n], wCur);
            model.poseTrans[n] = glm::mix(tPrev[n], tCur[n], wCur);
        } else {
            model.poseRot[n] = rCur[n];
            model.poseTrans[n] = tCur[n];
        }
    }
}

} // namespace ukiyo

namespace ukiyo {

glm::quat blendRotations(const std::vector<glm::quat>& rotations, const std::vector<float>& weights) {
    glm::quat acc(0.0f, 0.0f, 0.0f, 0.0f);
    glm::quat ref(1.0f, 0.0f, 0.0f, 0.0f);
    bool have = false;
    const size_t n = std::min(rotations.size(), weights.size());
    for (size_t i = 0; i < n; ++i) {
        if (weights[i] <= 0.0f) continue;
        if (!have) {
            ref = rotations[i];
            have = true;
        }
        const glm::quat q = glm::dot(rotations[i], ref) < 0.0f ? -rotations[i] : rotations[i];
        acc += q * weights[i];
    }
    if (!have) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const float len = glm::length(acc);
    return len > 1e-8f ? acc / len : ref;
}

std::vector<AnimClip> applyClipEdits(const SourceRig& rig, const std::vector<AnimClip>& source, const ClipEdits& edits) {
    std::vector<AnimClip> out = source;
    auto nodeIndex = [&rig](const std::string& name) {
        for (size_t i = 0; i < rig.names.size(); ++i)
            if (rig.names[i] == name) return static_cast<int>(i);
        return -1;
    };
    for (size_t ci = 0; ci < out.size(); ++ci) {
        AnimClip& clip = out[ci];
        for (const ClipEdits::TrackEdit& te : edits.tracks) {
            if (te.clip != clip.name) continue;
            const int node = nodeIndex(te.node);
            if (node < 0) continue;
            auto same = [&](const AnimTrack& t) { return t.node == node && t.path == te.path; };
            auto it = std::find_if(clip.tracks.begin(), clip.tracks.end(), same);
            if (te.times.empty()) {
                if (it != clip.tracks.end()) clip.tracks.erase(it);
                continue;
            }
            const size_t comps = te.path == AnimTrack::Path::Rotation ? 4 : 3;
            if (te.values.size() != te.times.size() * comps) continue;
            AnimTrack track;
            track.node = node;
            track.path = te.path;
            track.interp = te.interp == AnimTrack::Interp::Step ? AnimTrack::Interp::Step : AnimTrack::Interp::Linear;
            track.times = te.times;
            track.values = te.values;
            if (it != clip.tracks.end()) *it = std::move(track);
            else clip.tracks.push_back(std::move(track));
        }
        for (const ClipEdits::EventSet& es : edits.eventSets)
            if (es.clip == clip.name) clip.events = es.events;
        float end = source[ci].duration;
        for (const AnimTrack& t : clip.tracks)
            if (!t.times.empty()) end = std::max(end, t.times.back());
        for (const AnimEvent& e : clip.events) end = std::max(end, e.time);
        clip.duration = end;
    }
    return out;
}

std::vector<AnimEvent> eventsBetween(const AnimClip& clip, float t0, float t1) {
    std::vector<AnimEvent> out;
    for (const AnimEvent& e : clip.events)
        if (e.time > t0 && e.time <= t1) out.push_back(e);
    return out;
}

} // namespace ukiyo
