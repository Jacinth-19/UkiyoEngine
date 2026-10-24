#include "engine/motion_match.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/model.h"

namespace ukiyo {
namespace {

constexpr float kInf = std::numeric_limits<float>::max();

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A minimal recursive-descent BVH reader. It collects joints, their offsets and the channel order, then hands the
// motion block to a plain float scan, which is all a BVH is.
struct BvhReader {
    struct Joint {
        std::string name;
        int parent = -1;
        glm::vec3 offset{0.0f};
        std::vector<std::string> channels;
    };
    std::vector<Joint> joints;
    std::string error;
    bool ok = true;

    size_t pos = 0;
    const std::string* text = nullptr;

    void skipSpace() {
        while (pos < text->size() && std::isspace(static_cast<unsigned char>((*text)[pos]))) ++pos;
    }
    std::string token() {
        skipSpace();
        const size_t start = pos;
        while (pos < text->size() && !std::isspace(static_cast<unsigned char>((*text)[pos]))) ++pos;
        return text->substr(start, pos - start);
    }
    bool expect(const char* what) {
        if (token() != what) {
            error = std::string("expected ") + what;
            ok = false;
            return false;
        }
        return true;
    }
    void fail(const std::string& why) {
        if (ok) error = why;
        ok = false;
    }

    // Reads one joint (name already consumed) up to and including its closing brace.
    int readJoint(const std::string& name, int parent) {
        const int index = int(joints.size());
        joints.push_back(Joint{name, parent, glm::vec3(0.0f), {}});
        if (!expect("{")) return index;
        for (;;) {
            const std::string t = token();
            if (!ok) return index;
            if (t == "}") break;
            if (t == "OFFSET") {
                joints[index].offset.x = std::stof(token());
                joints[index].offset.y = std::stof(token());
                joints[index].offset.z = std::stof(token());
            } else if (t == "CHANNELS") {
                const int n = std::stoi(token());
                for (int i = 0; i < n; ++i) joints[index].channels.push_back(token());
            } else if (t == "JOINT" || t == "ROOT") {
                readJoint(token(), index);
            } else if (t == "End") {
                token();   // "Site"
                readJoint("EndSite", index);
            } else {
                // Unknown keyword: skip its arguments on this line. Real files only use the four above.
            }
        }
        return index;
    }
};

glm::mat3 channelRotation(const std::string& channel, float value) {
    const float r = glm::radians(value);
    if (channel.size() >= 1 && channel[0] == 'X') return glm::mat3(glm::rotate(glm::mat4(1.0f), r, glm::vec3(1, 0, 0)));
    if (channel.size() >= 1 && channel[0] == 'Y') return glm::mat3(glm::rotate(glm::mat4(1.0f), r, glm::vec3(0, 1, 0)));
    return glm::mat3(glm::rotate(glm::mat4(1.0f), r, glm::vec3(0, 0, 1)));
}

} // namespace

int MotionClip::indexOf(const std::string& joint) const {
    const std::string want = lower(joint);
    for (size_t i = 0; i < joints.size(); ++i)
        if (lower(joints[i].name) == want) return int(i);
    return -1;
}

bool loadMotionBvh(const std::string& text, MotionClip* out, std::string* error, float unit) {
    if (!out) return false;
    BvhReader reader;
    reader.text = &text;
    if (reader.token() != "HIERARCHY") {
        if (error) *error = "not a BVH file (no HIERARCHY)";
        return false;
    }
    if (reader.token() != "ROOT") {
        if (error) *error = "BVH has no ROOT";
        return false;
    }
    const std::string rootName = reader.token();
    const int root = reader.readJoint(rootName, -1);
    if (!reader.ok) {
        if (error) *error = reader.error;
        return false;
    }
    if (reader.token() != "MOTION") {
        if (error) *error = "BVH has no MOTION block";
        return false;
    }

    // Frames: N / Frame Time: dt, then the channel values.
    std::string key = reader.token();
    if (key != "Frames:") {
        if (error) *error = "expected Frames:";
        return false;
    }
    const int frameCount = std::stoi(reader.token());
    if (reader.token() != "Frame" || reader.token() != "Time:") {
        if (error) *error = "expected Frame Time:";
        return false;
    }
    const float frameTime = std::stof(reader.token());
    if (frameCount <= 0 || frameTime <= 0.0f) {
        if (error) *error = "BVH has no frames";
        return false;
    }

    int channels = 0;
    for (const auto& j : reader.joints) channels += int(j.channels.size());

    MotionClip clip;
    clip.name = "bvh";
    clip.root = root;
    clip.fps = 1.0f / frameTime;
    clip.joints.resize(reader.joints.size());
    for (size_t i = 0; i < reader.joints.size(); ++i) {
        clip.joints[i].name = reader.joints[i].name;
        clip.joints[i].parent = reader.joints[i].parent;
        clip.joints[i].offset = reader.joints[i].offset;
    }
    clip.frames.resize(size_t(frameCount));

    reader.skipSpace();
    for (int f = 0; f < frameCount; ++f) {
        std::vector<float> values(size_t(channels), 0.0f);
        for (int c = 0; c < channels; ++c) {
            reader.skipSpace();
            if (reader.pos >= text.size()) {
                if (error) *error = "BVH motion block ended early";
                return false;
            }
            const size_t start = reader.pos;
            while (reader.pos < text.size() && !std::isspace(static_cast<unsigned char>(text[reader.pos])))
                ++reader.pos;
            values[size_t(c)] = std::stof(text.substr(start, reader.pos - start));
        }

        std::vector<glm::mat4> world(clip.joints.size(), glm::mat4(1.0f));
        int cursor = 0;
        for (size_t j = 0; j < clip.joints.size(); ++j) {
            const auto& joint = reader.joints[j];
            // A root's position channels are the root's position in world space, and its OFFSET is the same position
            // in the rest pose — MotionBuilder writes them equal on frame 0. Adding the two puts the character half
            // its own height into the air, which is exactly what LAFAN1 did here until this branch existed. Every
            // other joint's offset is a bone length and is always added.
            bool rootPositioned = false;
            if (joint.parent < 0) {
                for (const std::string& ch : joint.channels)
                    if (ch.size() > 1 && ch[1] == 'p') rootPositioned = true;
            }
            glm::mat4 local = glm::translate(glm::mat4(1.0f), rootPositioned ? glm::vec3(0.0f) : joint.offset);
            for (const std::string& ch : joint.channels) {
                const float v = values[size_t(cursor++)];
                if (ch.size() > 1 && ch[1] == 'p') {
                    if (ch[0] == 'X') local = glm::translate(local, glm::vec3(v, 0, 0));
                    else if (ch[0] == 'Y') local = glm::translate(local, glm::vec3(0, v, 0));
                    else local = glm::translate(local, glm::vec3(0, 0, v));
                } else if (ch.size() > 1 && ch[1] == 'r') {
                    local = local * glm::mat4(channelRotation(ch, v));
                }
            }
            const int parent = joint.parent;
            world[j] = (parent >= 0 ? world[size_t(parent)] : glm::mat4(1.0f)) * local;
        }

        MotionFrame& frame = clip.frames[size_t(f)];
        frame.world.resize(clip.joints.size());
        for (size_t j = 0; j < clip.joints.size(); ++j) frame.world[j] = glm::vec3(world[j][3]);
        frame.rootPos = frame.world[size_t(root)];
        const glm::mat3 rot(world[size_t(root)]);
        const glm::vec3 forward = glm::normalize(rot * glm::vec3(0.0f, 0.0f, 1.0f));
        frame.rootRot = glm::angleAxis(std::atan2(forward.x, forward.z), glm::vec3(0.0f, 1.0f, 0.0f));
        // Unroll the yaw so a capture that turns past 180 degrees does not flip the local frame.
        if (f > 0) {
            const float prev = glm::eulerAngles(clip.frames[size_t(f - 1)].rootRot).y;
            const float now = glm::eulerAngles(frame.rootRot).y;
            float delta = now - prev;
            while (delta > glm::pi<float>()) delta -= 2.0f * glm::pi<float>();
            while (delta < -glm::pi<float>()) delta += 2.0f * glm::pi<float>();
            frame.rootRot = glm::angleAxis(prev + delta, glm::vec3(0.0f, 1.0f, 0.0f));
        }
    }

    // The root joint's local axes are not the character's axes. Measured on the whole corpus, the hips' local +Z
    // points 90 degrees away from where the toes point, consistently across all 77 LAFAN1 captures — so a yaw read
    // off the hips' +Z is a quarter turn from the way the performer is facing, and the character is drawn walking
    // sideways. Take the character's forward from the toes (the only part of the skeleton that unambiguously points
    // where the body is going) at the rest pose, and rotate every frame's yaw by the difference. Derived per file
    // instead of hard-coding -90 degrees, so a skeleton with a different rest orientation still lands right.
    {
        const int lf = clip.indexOf("LeftFoot"), rf = clip.indexOf("RightFoot");
        // A named toe if the skeleton has one; otherwise the foot's own child, which is where a BVH without toe joints
        // (only an End Site) puts the same information.
        const auto toeOf = [&](int foot, const char* name) {
            const int named = clip.indexOf(name);
            if (named >= 0 || foot < 0) return named;
            for (size_t j = 0; j < clip.joints.size(); ++j)
                if (clip.joints[j].parent == foot) return int(j);
            return -1;
        };
        const int lt = toeOf(lf, "LeftToe"), rt = toeOf(rf, "RightToe");
        const MotionFrame& rest = clip.frames.front();
        glm::vec3 toe(0.0f);
        if (lf >= 0 && lt >= 0) toe += rest.world[size_t(lt)] - rest.world[size_t(lf)];
        if (rf >= 0 && rt >= 0) toe += rest.world[size_t(rt)] - rest.world[size_t(rf)];
        toe.y = 0.0f;
        if (glm::length(toe) > 1e-4f) {
            const glm::vec3 hips = rest.rootRot * glm::vec3(0.0f, 0.0f, 1.0f);
            float offset = std::atan2(toe.x, toe.z) - std::atan2(hips.x, hips.z);
            while (offset > glm::pi<float>()) offset -= 2.0f * glm::pi<float>();
            while (offset < -glm::pi<float>()) offset += 2.0f * glm::pi<float>();
            for (MotionFrame& f : clip.frames) {
                const float yaw = std::atan2(2.0f * (f.rootRot.w * f.rootRot.y), 1.0f - 2.0f * f.rootRot.y * f.rootRot.y);
                f.rootRot = glm::angleAxis(yaw + offset, glm::vec3(0.0f, 1.0f, 0.0f));
            }
        }
    }

    // Units. A capture in centimetres (LAFAN1, MotionBuilder) puts the hips around 90-110; anything above 10 is
    // centimetres, and the file is brought to metres. The rest pose is checked, not the animated root, because some
    // clips start crouched.
    if (unit <= 0.0f) {
        float hipsHeight = 0.0f;
        for (const MotionFrame& f : clip.frames) hipsHeight = std::max(hipsHeight, f.rootPos.y);
        unit = hipsHeight > 10.0f ? 0.01f : 1.0f;
    }
    if (std::fabs(unit - 1.0f) > 1e-6f) {
        for (MotionJoint& j : clip.joints) j.offset *= unit;
        for (MotionFrame& f : clip.frames) {
            for (glm::vec3& p : f.world) p *= unit;
            f.rootPos *= unit;
        }
        clip.unitScale = unit;
    }

    *out = std::move(clip);
    return true;
}

void recentreMotionClip(MotionClip* clip) {
    if (!clip || clip->frames.empty()) return;
    glm::vec3 origin = clip->frames.front().rootPos;
    origin.y = 0.0f;
    // And the lowest joint over the whole clip, so the feet sit on 0. Seeded from the first frame rather than from
    // zero: a capture can be entirely above the ground, and a zero start would then leave it floating.
    float lowest = std::numeric_limits<float>::max();
    for (const MotionFrame& f : clip->frames)
        for (const glm::vec3& p : f.world) lowest = std::min(lowest, p.y);
    if (lowest == std::numeric_limits<float>::max()) lowest = 0.0f;
    for (MotionFrame& f : clip->frames) {
        for (glm::vec3& p : f.world) p = p - origin - glm::vec3(0.0f, lowest, 0.0f);
        f.rootPos = f.rootPos - origin - glm::vec3(0.0f, lowest, 0.0f);
    }
    clip->ground = 0.0f;
}

int trimRestLeadIn(MotionClip* clip, float minSeconds) {
    if (!clip || clip->frames.empty() || clip->fps <= 0.0f) return 0;
    const int lh = clip->indexOf("LeftHand"), rh = clip->indexOf("RightHand");
    if (lh < 0 || rh < 0) return 0;
    const size_t a = size_t(lh), b = size_t(rh);
    if (clip->frames.front().world.size() <= std::max(a, b)) return 0;
    const auto armsOut = [&](const MotionFrame& f) {
        return glm::length(f.world[a] - f.world[b]) > 1.15f;   // wider than any shoulder, arms held sideways
    };
    // "Not moving" has to be a per-frame test, not drift from frame 0. The suit calibration is not perfectly still —
    // the arms creep a couple of centimetres a second — so a drift test either never triggers or, told to ignore
    // drift, trims a real transition. The speed of the fastest joint separates the two cleanly: the hold moves under
    // 0.01 m per frame, the first real frames move 0.03 m or more.
    const float quiet = 0.30f / clip->fps;
    const auto still = [&](int f) {
        if (f + 1 >= clip->frameCount()) return true;
        const MotionFrame& x = clip->frames[size_t(f)];
        const MotionFrame& y = clip->frames[size_t(f + 1)];
        for (size_t j = 0; j < x.world.size() && j < y.world.size(); ++j)
            if (glm::length(y.world[j] - x.world[j]) > quiet) return false;
        return true;
    };
    int end = 0;
    while (end < clip->frameCount() && armsOut(clip->frames[size_t(end)]) && still(end)) ++end;
    if (float(end) / clip->fps < minSeconds || end >= clip->frameCount() - 1) return 0;
    clip->frames.erase(clip->frames.begin(), clip->frames.begin() + end);
    return end;
}

int loadMotionBvhFolder(const std::string& folder, std::vector<MotionClip>* clips, std::string* error, float unit) {
    if (!clips) return 0;
    std::vector<std::string> paths;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".bvh")
            paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    int loaded = 0, failed = 0;
    std::string firstError;
    for (const std::string& path : paths) {
        MotionClip clip;
        std::string why;
        if (!loadMotionBvhFile(path, &clip, &why, unit)) {
            ++failed;
            if (firstError.empty()) firstError = why;
            continue;
        }
        recentreMotionClip(&clip);
        clips->push_back(std::move(clip));
        ++loaded;
    }
    if (error) {
        *error = "loaded " + std::to_string(loaded) + " of " + std::to_string(paths.size()) + " BVH files";
        if (failed > 0) *error += " (" + std::to_string(failed) + " failed, first: " + firstError + ")";
    }
    return loaded;
}

bool loadMotionBvhFile(const std::string& path, MotionClip* out, std::string* error, float unit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "could not open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    const bool ok = loadMotionBvh(text, out, error, unit);
    if (ok && out) {
        out->name = path.substr(path.find_last_of("/\\") + 1);
        // Do this before recentring: the rest frames are where the performer is standing, and they move the origin.
        trimRestLeadIn(out);
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Database

bool MotionDatabase::build(std::vector<MotionClip> clips, const MotionSettings& settings, std::string* error) {
    clips_ = std::move(clips);
    settings_ = settings;
    entries_.clear();
    nodes_.clear();
    root_ = -1;
    if (clips_.empty()) {
        if (error) *error = "no clips";
        return false;
    }

    clipStart_.assign(clips_.size(), -1);
    for (size_t c = 0; c < clips_.size(); ++c) {
        const MotionClip& clip = clips_[c];
        if (clip.frames.size() < 2) continue;
        clipStart_[c] = int(entries_.size());
        // The clip's floor: the lowest any joint gets. See MotionClip::ground.
        {
            float low = std::numeric_limits<float>::max();
            for (const MotionFrame& f : clip.frames)
                for (const glm::vec3& p : f.world) low = std::min(low, p.y);
            if (low != std::numeric_limits<float>::max()) clips_[c].ground = low;
        }
        const int root = clip.root;
        const int leftFoot = clip.indexOf("LeftFoot") >= 0 ? clip.indexOf("LeftFoot") : clip.indexOf("leftFoot");
        const int rightFoot = clip.indexOf("RightFoot") >= 0 ? clip.indexOf("RightFoot") : clip.indexOf("rightFoot");
        // The pose descriptor's three joints, by their common names across the corpora.
        const int poseJoints[3] = {clip.indexOf("Head") >= 0 ? clip.indexOf("Head") : clip.indexOf("Neck"),
                                   clip.indexOf("LeftHand") >= 0 ? clip.indexOf("LeftHand") : clip.indexOf("LeftForeArm"),
                                   clip.indexOf("RightHand") >= 0 ? clip.indexOf("RightHand")
                                                                  : clip.indexOf("RightForeArm")};
        const float fps = clip.fps;
        const int window = std::max(1, int(settings_.velocityWindow * fps));
        auto sample = [&](int frame, int joint) {
            const int f = std::clamp(frame, 0, clip.frameCount() - 1);
            return clip.frames[size_t(f)].world[size_t(std::max(0, joint))];
        };
        auto localOf = [&](int frame, glm::vec3 p) {
            const MotionFrame& f = clip.frames[size_t(std::clamp(frame, 0, clip.frameCount() - 1))];
            return glm::inverse(f.rootRot) * (p - f.rootPos);
        };
        for (int frame = 0; frame < clip.frameCount(); ++frame) {
            MotionEntry e;
            e.clip = int(c);
            e.frame = frame;
            const MotionFrame& f = clip.frames[size_t(frame)];
            for (int i = 0; i < 4; ++i) {
                const int ahead = std::min(clip.frameCount() - 1, frame + int(std::lround((i + 1) * settings_.trajectoryStep * fps)));
                const glm::vec3 p = clip.frames[size_t(ahead)].rootPos;
                const glm::vec3 local = glm::inverse(f.rootRot) * (p - f.rootPos);
                e.feature[size_t(i * 2 + 0)] = local.x;
                e.feature[size_t(i * 2 + 1)] = local.z;
            }
            {
                const int ahead = std::min(clip.frameCount() - 1,
                                           frame + int(std::lround(4.0f * settings_.trajectoryStep * fps)));
                const glm::vec3 forward = clip.frames[size_t(ahead)].rootRot * glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::vec3 local = glm::inverse(f.rootRot) * forward;
                e.feature[8] = local.x;
                e.feature[9] = local.z;
            }
            const glm::vec3 lf = localOf(frame, sample(frame, leftFoot));
            const glm::vec3 rf = localOf(frame, sample(frame, rightFoot));
            e.feature[10] = lf.x; e.feature[11] = lf.y; e.feature[12] = lf.z;
            e.feature[13] = rf.x; e.feature[14] = rf.y; e.feature[15] = rf.z;
            const glm::vec3 lfv = (sample(frame + window, leftFoot) - sample(frame - window, leftFoot)) /
                                  (2.0f * float(window) / fps);
            const glm::vec3 rfv = (sample(frame + window, rightFoot) - sample(frame - window, rightFoot)) /
                                  (2.0f * float(window) / fps);
            const glm::vec3 lfl = glm::inverse(f.rootRot) * lfv;
            const glm::vec3 rfl = glm::inverse(f.rootRot) * rfv;
            e.feature[16] = lfl.x; e.feature[17] = lfl.y; e.feature[18] = lfl.z;
            e.feature[19] = rfl.x; e.feature[20] = rfl.y; e.feature[21] = rfl.z;
            const glm::vec3 rootVel = (sample(frame + window, root) - sample(frame - window, root)) /
                                      (2.0f * float(window) / fps);
            const glm::vec3 rootLocal = glm::inverse(f.rootRot) * rootVel;
            e.feature[22] = rootLocal.x;
            e.feature[23] = rootLocal.z;
            for (int k = 0; k < 3; ++k) {
                const glm::vec3 local = poseJoints[k] >= 0 ? localOf(frame, sample(frame, poseJoints[k])) : glm::vec3(0.0f);
                for (int axis = 0; axis < 3; ++axis) {
                    e.pose[size_t(k * 3 + axis)] = (&local.x)[axis];
                    e.feature[size_t(24 + k * 3 + axis)] = (&local.x)[axis];
                }
            }
            entries_.push_back(e);
        }
    }
    if (entries_.empty()) {
        if (error) *error = "no usable frames";
        return false;
    }

    std::vector<int> order(entries_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    nodes_.reserve(entries_.size());
    root_ = buildNode(&order, 0, int(order.size()));
    return root_ >= 0;
}

int MotionDatabase::buildNode(std::vector<int>* order, int lo, int hi) {
    if (lo >= hi) return -1;
    int axis = 0;
    float bestSpread = -1.0f;
    for (int a = 0; a < kIndexDims; ++a) {
        const int dim = kIndexFeatures[size_t(a)];
        float mn = kInf, mx = -kInf;
        for (int i = lo; i < hi; ++i) {
            const float v = entries_[size_t((*order)[size_t(i)])].feature[size_t(dim)];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (mx - mn > bestSpread) {
            bestSpread = mx - mn;
            axis = a;
        }
    }
    const int dim = kIndexFeatures[size_t(axis)];
    const int mid = (lo + hi) / 2;
    std::nth_element(order->begin() + lo, order->begin() + mid, order->begin() + hi, [&](int a, int b) {
        return entries_[size_t(a)].feature[size_t(dim)] < entries_[size_t(b)].feature[size_t(dim)];
    });
    const int node = int(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[size_t(node)].entry = (*order)[size_t(mid)];
    nodes_[size_t(node)].axis = axis;
    nodes_[size_t(node)].split = entries_[size_t((*order)[size_t(mid)])].feature[size_t(dim)];
    nodes_[size_t(node)].left = buildNode(order, lo, mid);
    nodes_[size_t(node)].right = buildNode(order, mid + 1, hi);
    return node;
}

int MotionDatabase::entryAt(int clip, int frame) const {
    if (clip < 0 || clip >= int(clipStart_.size()) || clipStart_[size_t(clip)] < 0) return -1;
    const MotionClip& c = clips_[size_t(clip)];
    if (frame < 0 || frame >= c.frameCount()) return -1;
    return clipStart_[size_t(clip)] + frame;
}

float MotionDatabase::indexDistance(const std::array<float, kMotionDims>& a,
                                    const std::array<float, kMotionDims>& b) const {
    float sum = 0.0f;
    for (int i = 0; i < kIndexDims; ++i) {
        const int d = kIndexFeatures[size_t(i)];
        const float diff = a[size_t(d)] - b[size_t(d)];
        sum += settings_.weights[size_t(d)] * diff * diff;
    }
    return sum;
}

float MotionDatabase::weightedDistance(const std::array<float, kMotionDims>& a,
                                       const std::array<float, kMotionDims>& b) const {
    float sum = 0.0f;
    for (int i = 0; i < kMotionDims; ++i) {
        const float d = a[size_t(i)] - b[size_t(i)];
        sum += settings_.weights[size_t(i)] * d * d;
    }
    return sum;
}

void MotionDatabase::search(int node, const std::array<float, kMotionDims>& query, int k,
                            std::vector<std::pair<float, int>>* best) const {
    if (node < 0) return;
    const Node& n = nodes_[size_t(node)];
    const float d = indexDistance(query, entries_[size_t(n.entry)].feature);
    if (int(best->size()) < k) {
        best->emplace_back(d, n.entry);
        std::push_heap(best->begin(), best->end());
    } else if (d < best->front().first) {
        std::pop_heap(best->begin(), best->end());
        best->back() = {d, n.entry};
        std::push_heap(best->begin(), best->end());
    }
    const int dim = kIndexFeatures[size_t(n.axis)];
    const float delta = query[size_t(dim)] - n.split;
    const float plane = settings_.weights[size_t(dim)] * delta * delta;
    const int near = delta < 0.0f ? n.left : n.right;
    const int far = delta < 0.0f ? n.right : n.left;
    search(near, query, k, best);
    if (plane < (int(best->size()) < k ? kInf : best->front().first)) search(far, query, k, best);
}

int MotionDatabase::nearest(const std::array<float, kMotionDims>& query, int k, int* out) const {
    if (!out || k <= 0 || root_ < 0) return 0;
    std::vector<std::pair<float, int>> best;
    best.reserve(size_t(k) + 1);
    search(root_, query, k, &best);
    std::sort(best.begin(), best.end());
    const int count = std::min<int>(int(best.size()), k);
    for (int i = 0; i < count; ++i) out[i] = best[size_t(i)].second;
    return count;
}

std::array<float, kMotionDims> MotionDatabase::query(glm::vec3 velocity, float facingYaw, int poseFrom) const {
    std::array<float, kMotionDims> q{};
    if (poseFrom >= 0 && poseFrom < size()) q = entries_[size_t(poseFrom)].feature;
    // The pose half of the query is the current frame's, which is what the cost function compares against.
    // Every feature in the database is expressed in the character's own frame, so the query must be too. A desired
    // world velocity becomes a local one by taking the facing out of it; for the common case — the controller turns
    // the character to face where it is going — that is a straight line up +z, and the local forward is (0, 1).
    const float yaw = glm::length(glm::vec2(velocity.x, velocity.z)) > 0.05f
                          ? std::atan2(velocity.x, velocity.z)
                          : facingYaw;
    const glm::vec3 localVelocity = glm::angleAxis(-yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * velocity;
    for (int i = 0; i < 4; ++i) {
        const float t = float(i + 1) * settings_.trajectoryStep;
        q[size_t(i * 2 + 0)] = localVelocity.x * t;
        q[size_t(i * 2 + 1)] = localVelocity.z * t;
    }
    q[8] = 0.0f;
    q[9] = 1.0f;
    // The root velocity is part of the trajectory, not of the pose. Copying the pose entry's feature wholesale left it
    // holding whatever speed the character already had, so a query for 1.6 m/s asked for 1.6 m of travel over the
    // next 0.6 s and, at the same time, for the root to stand still — and the search then preferred slow frames that
    // happened to drift forward, which is how a run query ended up matching a crawl.
    q[22] = localVelocity.x;
    q[23] = localVelocity.z;
    return q;
}

float MotionDatabase::cost(const std::array<float, kMotionDims>& query, int entry, int poseFrom) const {
    if (entry < 0 || entry >= size()) return kInf;
    float total = weightedDistance(query, entries_[size_t(entry)].feature);
    if (settings_.poseWeight > 0.0f && poseFrom >= 0)
        total += settings_.poseWeight * poseDistance(entry, poseFrom);
    return total;
}

float MotionDatabase::poseDistance(int a, int b) const {
    if (a < 0 || b < 0 || a >= size() || b >= size()) return 0.0f;
    const MotionEntry& ea = entries_[size_t(a)];
    const MotionEntry& eb = entries_[size_t(b)];
    const MotionFrame& fa = clips_[size_t(ea.clip)].frames[size_t(ea.frame)];
    const MotionFrame& fb = clips_[size_t(eb.clip)].frames[size_t(eb.frame)];
    const size_t n = std::min(fa.world.size(), fb.world.size());
    if (n == 0) return 0.0f;
    const glm::quat ia = glm::inverse(fa.rootRot);
    const glm::quat ib = glm::inverse(fb.rootRot);
    float sum = 0.0f;
    for (size_t j = 0; j < n; ++j) {
        const glm::vec3 la = ia * (fa.world[j] - fa.rootPos);
        const glm::vec3 lb = ib * (fb.world[j] - fb.rootPos);
        sum += glm::length(la - lb);
    }
    return sum / float(n);
}

float MotionDatabase::continuationCost(const std::array<float, kMotionDims>& query, int from, float seconds,
                                       int poseFrom) const {
    if (from < 0 || from >= size()) return kInf;
    const MotionEntry& e = entries_[size_t(from)];
    const MotionClip& clip = clips_[size_t(e.clip)];
    const int ahead = e.frame + int(std::lround(seconds * clip.fps));
    if (ahead >= clip.frameCount() - 1) return kInf;
    const int entry = entryAt(e.clip, ahead);
    if (entry < 0) return kInf;
    return cost(query, entry, poseFrom);
}

// ---------------------------------------------------------------------------------------------------------------
// Player

void MotionPlayer::reset() {
    index_ = -1;
    lastClip_ = -1;
    frame_ = 0.0f;
    sinceMatch_ = 0.0f;
    blend_ = 1.0f;
    matches_ = 0;
    cost_ = 0.0f;
    lastMatchOffset_ = 0.0f;
    blendFrom_.clear();
    velocity_ = glm::vec3(0.0f);
    facing_ = 0.0f;
}

void MotionPlayer::start(int entry, float frame) {
    if (!database_ || entry < 0 || entry >= database_->size()) return;
    index_ = entry;
    lastClip_ = database_->entry(entry).clip;
    frame_ = frame > 0.0f ? frame : float(database_->entry(entry).frame);
    blend_ = 1.0f;
    sinceMatch_ = 0.0f;
    blendFrom_.clear();
}

void MotionPlayer::setTarget(glm::vec3 velocity, float facingYaw) {
    velocity_ = velocity;
    facing_ = facingYaw;
}

int MotionPlayer::bestMatch(const std::array<float, kMotionDims>& query, float* costOut) const {
    // Two passes. The KD-tree answers "where is the character going"; the pose pass then asks "and what is the body
    // doing". Without the second pass a 1.4 m/s walk queries the walk clip at the opposite phase just as readily as
    // the one the character is already in, and the blend has to travel half a metre of arm swing to fix it — which is
    // exactly the pop inertialization exists to avoid.
    const MotionSettings& settings = database_->settings();
    const int k = std::clamp(settings.rerankCandidates, 1, 64);
    std::vector<int> candidates(size_t(k), -1);
    const int n = database_->nearest(query, k, candidates.data());

    const int currentFrame = int(std::lround(frame_));
    // The pose to compare against is the one *one match interval from now*, not the one on screen. The query is
    // compared with the candidate's frame at the start of its trajectory, and staying is measured at the end of the
    // interval; comparing a candidate at `t` with a pose at `t` makes every candidate win by half an interval and the
    // player then jumps back to the frame it just left, forever. Asking for the pose at `t + interval` puts the
    // continuation frame exactly on the query, which is what makes "stay" and "keep going" score the same.
    const double clipFps = lastClip_ >= 0 ? database_->clips()[size_t(lastClip_)].fps : 30.0;
    const int predictedFrame = currentFrame + int(std::lround(double(matchInterval_) * double(clipFps)));
    const int predictedEntry = database_->entryAt(lastClip_, predictedFrame);

    struct Scored {
        int idx = -1;
        float cost = kInf;
    };
    std::vector<Scored> scored;
    scored.reserve(size_t(std::max(1, n)));
    for (int i = 0; i < n; ++i) {
        const int idx = candidates[size_t(i)];
        if (idx < 0) continue;
        const int clipIndex = database_->entry(idx).clip;
        const int frame = database_->entry(idx).frame;
        float cost = database_->cost(query, idx, predictedEntry);
        // A jump to another clip is a visual cut: price it. A jump *backwards* along this one is a rewind, which is
        // just as visible and must not be cheaper than a short hop forward — otherwise the cheapest move is always to
        // return to the frame just played, and the character animates in place while the controller walks it forward.
        const float fps = std::max(1.0f, database_->clip(idx).fps);
        float seconds = 1.0f;
        if (clipIndex == lastClip_) {
            const float ahead = float(frame - currentFrame) / fps;
            seconds = ahead >= 0.0f ? ahead : 1.0f;
        }
        cost += settings.continuity * seconds;
        scored.push_back({idx, cost});
    }
    if (scored.empty()) return -1;

    const auto best = std::min_element(scored.begin(), scored.end(),
                                       [](const Scored& a, const Scored& b) { return a.cost < b.cost; });
    if (costOut) *costOut = best->cost;
    return best->idx;
}

void MotionPlayer::update(float dt) {
    if (!database_ || database_->empty() || dt <= 0.0f) return;
    if (index_ < 0) start(0);
    const MotionClip& clip = database_->clip(index_);
    frame_ += dt * clip.fps;
    if (frame_ >= float(clip.frameCount() - 1)) {
        // A looping clip wraps; a one-shot leaves the last frame up and lets the next match take over.
        frame_ = float(clip.frameCount() - 1);
    }
    sinceMatch_ += dt;
    if (blend_ < 1.0f) blend_ = std::min(1.0f, blend_ + dt / blendTime_);

    if (sinceMatch_ < matchInterval_) return;
    sinceMatch_ = 0.0f;

    // The pose half of the query is where the character will be one interval from now, for the same reason the
    // re-rank uses it: a candidate is scored at the start of its trajectory, so the query has to describe the pose the
    // trajectory should *arrive* at. Using the pose on screen makes every candidate a half-interval stale and the
    // winner is the frame just left.
    const int currentFrame = int(std::lround(frame_));
    const int predictedFrame = currentFrame + int(std::lround(double(matchInterval_) * double(clip.fps)));
    const int currentEntry = database_->entryAt(lastClip_, currentFrame);
    const int predictedEntry = database_->entryAt(lastClip_, predictedFrame);
    const auto query = database_->query(velocity_, facing_, predictedEntry >= 0 ? predictedEntry : index_);
    float winnerCost = 0.0f;
    const int winner = bestMatch(query, &winnerCost);
    if (winner < 0) return;

    // Compare against walking on for one match interval. Staying put would win forever if it were compared with the
    // current frame: the trajectory is relative, so "stand still" always looks like a perfect local fit.
    // From the frame on screen, not the predicted entry: continuationCost walks forward by `matchInterval_` itself,
    // and starting from the predicted entry would price an extra interval of mismatch against staying — which made
    // every match look better than walking on by exactly that much.
    const float stay = currentEntry >= 0
                           ? database_->continuationCost(query, currentEntry, matchInterval_,
                                                          predictedEntry >= 0 ? predictedEntry : currentEntry)
                           : kInf;
    if (std::getenv("UKIYO_MOTION_TRACE"))
        std::fprintf(stderr, "  check winner %.3f  stay %.3f  (margin %.3f)  cur %d/%d  win %d/%d  pred %d\n",
                     double(winnerCost), double(stay), double(database_->settings().switchMargin), lastClip_,
                     currentFrame, database_->entry(winner).clip, database_->entry(winner).frame, predictedFrame);
    // The margin always applies, including mid-blend. It used to be skipped while a blend was still running, on the
    // theory that a clearly better match should be allowed to take over; but matches arrive every 0.16 s and a blend
    // lasts 0.20 s, so *every* match lands mid-blend, and the guard turned into "always switch to the cheapest
    // candidate" — which picks the frame just left and loops the animation over ten frames while the controller walks.
    // If a match is worth taking during a blend, it is worth taking because it beats staying by the margin.
    if (winnerCost + database_->settings().switchMargin >= stay) return;

    // Sample what is on screen — pose *and* velocity — and what the incoming frame will be, then combine them with
    // whatever offset is still decaying. Matches arrive every 0.16 s and a blend lasts 0.20 s, so a jump usually
    // lands mid-blend; replacing the offset outright would throw away the motion still on screen and, worse, make
    // every jump measure against a pose that is itself offset, so the errors compound into a body that comes apart.
    std::vector<glm::vec3> outPositions;
    glm::vec3 outRoot(0.0f);
    glm::quat outRot(1.0f, 0.0f, 0.0f, 0.0f);
    sample(&outPositions, &outRoot, &outRot);
    const MotionClip& fromClip = database_->clip(index_);
    const int fromClipIndex = database_->entry(index_).clip;
    const int c0 = std::clamp(int(std::floor(frame_)), 0, fromClip.frameCount() - 1);
    const int c1 = std::min(c0 + 1, fromClip.frameCount() - 1);
    std::vector<glm::vec3> outVel(outPositions.size(), glm::vec3(0.0f));
    for (size_t j = 0; j < outVel.size() && j < fromClip.frames[size_t(c0)].world.size(); ++j)
        outVel[j] = (fromClip.frames[size_t(c1)].world[j] - fromClip.frames[size_t(c0)].world[j]) * fromClip.fps;
    const glm::vec3 outRootVel =
        (fromClip.frames[size_t(c1)].rootPos - fromClip.frames[size_t(c0)].rootPos) * fromClip.fps;

    // `clip()` takes an *entry* index; here we have a clip index, so go through the clip list directly.
    const MotionClip& toClip = database_->clips()[size_t(database_->entry(winner).clip)];
    const int toFrame = database_->entry(winner).frame;
    const int toNext = std::min(toFrame + 1, toClip.frameCount() - 1);
    const MotionFrame& inFrame = toClip.frames[size_t(toFrame)];
    std::vector<glm::vec3> inVel(inFrame.world.size(), glm::vec3(0.0f));
    for (size_t j = 0; j < inVel.size(); ++j)
        inVel[j] = (toClip.frames[size_t(toNext)].world[j] - inFrame.world[j]) * toClip.fps;

    // `sample()` above returns the pose *with* the decaying offset already in it, so the position part of a
    // mid-blend carry is already accounted for: adding it a second time is what made the offsets grow to a metre and
    // stay there. Only the offset's velocity is missing from that sample, and that is what has to be carried.
    const bool midBlend = blend_ < 1.0f;
    std::vector<glm::vec3> carryVel(outPositions.size(), glm::vec3(0.0f));
    if (midBlend) {
        for (size_t j = 0; j < carryVel.size() && j < blendFrom_.size(); ++j)
            carryVel[j] = inertializeVelocity(blendFrom_[j], j < blendVel_.size() ? blendVel_[j] : glm::vec3(0.0f), blend_);
    }
    const glm::vec3 carryRootVel = midBlend ? inertializeVelocity(blendRoot_, blendRootVel_, blend_) : glm::vec3(0.0f);

    // Root-relative. The two clips are metres apart in their own space — each was recentred to its own origin and
    // then walked forward from it — so subtracting absolute positions would put the whole separation of the two
    // captures into the offset and slide the character a metre off its own feet for a fifth of a second. What has
    // to carry across the seam is the *pose*, which is what is left after the root comes out.
    blendFrom_.resize(outPositions.size());
    blendVel_.resize(outPositions.size());
    for (size_t j = 0; j < outPositions.size(); ++j) {
        const glm::vec3 toLocal = j < inFrame.world.size() ? inFrame.world[j] - inFrame.rootPos : glm::vec3(0.0f);
        blendFrom_[j] = (outPositions[j] - outRoot) - toLocal;
        blendVel_[j] = carryVel[j] + (j < outVel.size() ? outVel[j] - outRootVel : glm::vec3(0.0f)) -
                       ((j < inVel.size() ? inVel[j] : glm::vec3(0.0f)) -
                        (toClip.frames[size_t(toNext)].rootPos - inFrame.rootPos) * toClip.fps) *
                           blendTime_;
    }
    // The root's own offset is zero, not `outRoot - inFrame.rootPos`. Two captures are hundreds of frames apart in
    // their own space, so that difference is metres; the controller renders `joint - root`, so a residual on the root
    // becomes a residual on every joint with the opposite sign, and the character slides sideways for the whole blend.
    // The pose is what carries across the seam, and the pose is root-relative. Only the root's *velocity* is carried,
    // so the lift at the hips does not stop dead at the seam.
    blendRoot_ = glm::vec3(0.0f);
    blendRootVel_ = carryRootVel + (outRootVel - (toClip.frames[size_t(toNext)].rootPos - inFrame.rootPos) * toClip.fps) *
                                       blendTime_;
    blendRot_ = outRot;
    {
        // Always measured, not just under the trace: the demo and any game-side telemetry want it.
        float mx = 0.0f;
        for (size_t j = 0; j < blendFrom_.size(); ++j) mx = std::max(mx, glm::length(blendFrom_[j]));
        lastMatchOffset_ = mx;
    }
    if (std::getenv("UKIYO_MOTION_TRACE")) {
        float mx = 0.0f, cv = 0.0f;
        size_t worst = 0;
        for (size_t j = 0; j < blendFrom_.size(); ++j) {
            if (glm::length(blendFrom_[j]) > mx) {
                mx = glm::length(blendFrom_[j]);
                worst = j;
            }
            cv = std::max(cv, glm::length(blendVel_[j]));
        }
        std::fprintf(stderr, "JUMP %d->%d  maxOffset %.2f @%s  rootOff %.2f  maxVel %.2f  midBlend %d\n",
                     fromClipIndex, database_->entry(winner).clip, double(mx),
                     worst < toClip.joints.size() ? toClip.joints[worst].name.c_str() : "?", double(glm::length(blendRoot_)),
                     double(cv), midBlend ? 1 : 0);
    }

    index_ = winner;
    lastClip_ = database_->entry(winner).clip;
    frame_ = float(database_->entry(winner).frame);
    blend_ = 0.0f;
    cost_ = winnerCost;
    ++matches_;
}

void MotionPlayer::sample(std::vector<glm::vec3>* positions, glm::vec3* rootPos, glm::quat* rootRot) const {
    if (!positions || !ready()) return;
    const MotionClip& clip = database_->clip(index_);
    const int count = clip.frameCount();
    const int i0 = std::clamp(int(std::floor(frame_)), 0, count - 1);
    const int i1 = std::min(i0 + 1, count - 1);
    const float t = std::clamp(frame_ - float(i0), 0.0f, 1.0f);
    const MotionFrame& a = clip.frames[size_t(i0)];
    const MotionFrame& b = clip.frames[size_t(i1)];
    positions->resize(a.world.size());
    for (size_t j = 0; j < a.world.size(); ++j) (*positions)[j] = glm::mix(a.world[j], b.world[j], t);
    const glm::vec3 p = glm::mix(a.rootPos, b.rootPos, t);
    const glm::quat r = glm::slerp(a.rootRot, b.rootRot, t);
    if (blend_ < 1.0f && blendFrom_.size() == positions->size()) {
        // Inertialization: subtract the decaying offset instead of crossing to the new pose. The offset starts at the
        // old pose's position *and* velocity, so the seam does not stop the character dead the way a crossfade does.
        const float s = blend_;
        for (size_t j = 0; j < positions->size(); ++j) {
            const glm::vec3 v = j < blendVel_.size() ? blendVel_[j] : glm::vec3(0.0f);
            (*positions)[j] += inertialize(blendFrom_[j], v, s);
        }
        if (rootPos) *rootPos = p + inertialize(blendRoot_, blendRootVel_, s);
        if (rootRot) *rootRot = glm::slerp(blendRot_, r, s);
        return;
    }
    if (rootPos) *rootPos = p;
    if (rootRot) *rootRot = r;
}

// ---------------------------------------------------------------------------------------------------------------
// Retarget

glm::vec3 inertialize(glm::vec3 p, glm::vec3 v, float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    const float s3 = s * s * s, s4 = s3 * s, s5 = s4 * s;
    return p + v * s - (p * 10.0f + v * 6.0f) * s3 + (p * 15.0f + v * 8.0f) * s4 - (p * 6.0f + v * 3.0f) * s5;
}

glm::vec3 inertializeVelocity(glm::vec3 p, glm::vec3 v, float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    const float s2 = s * s, s3 = s2 * s, s4 = s3 * s;
    return v - (p * 30.0f + v * 18.0f) * s2 + (p * 60.0f + v * 32.0f) * s3 - (p * 30.0f + v * 15.0f) * s4;
}

void MotionController::setDatabase(const MotionDatabase* database) {
    database_ = database;
    player_.setDatabase(database);
    reset();
}

void MotionController::reset(glm::vec3 position, float yaw) {
    position_ = position;
    velocity_ = glm::vec3(0.0f);
    yaw_ = yaw;
    pose_.clear();
    // A reset starts a new performance: no foot is still standing on the old one.
    for (Leg& leg : legs_) leg = Leg{};
    legsClip_ = -1;
    lockedFeet_ = 0;
    lastFootCorrection_ = 0.0f;
    player_.reset();
    if (database_ && !database_->empty()) player_.start(0);
}

void MotionController::update(glm::vec3 wishDir, float wishSpeed, float dt, float facing) {
    if (!database_ || database_->empty() || dt <= 0.0f) return;
    if (!player_.ready()) reset(position_, yaw_);

    // The ground plane only: a character's wish direction is never vertical, and the database's features are 2D.
    wishDir.y = 0.0f;
    const float len = glm::length(wishDir);
    wishDir = len > 1e-4f ? wishDir / len : glm::vec3(0.0f);
    const glm::vec3 wish = wishDir * std::max(0.0f, wishSpeed);

    // Approach the wish velocity at a finite acceleration, so the matcher never sees a step change it cannot match.
    const glm::vec3 delta = wish - velocity_;
    const float step = acceleration_ * dt;
    velocity_ += glm::length(delta) > step ? glm::normalize(delta) * step : delta;

    // Turn towards the facing by the shortest way; when decelerating the body keeps the heading it had.
    const auto wrap = [](float a) { return std::fmod(a + 3.14159265f, 6.2831853f) - 3.14159265f; };
    float diff = wrap(facing - yaw_);
    if (len < 1e-4f) diff = 0.0f;
    const float maxTurn = turnRate_ * dt;
    yaw_ = wrap(yaw_ + std::clamp(diff, -maxTurn, maxTurn));

    position_ += velocity_ * dt;
    player_.setTarget(velocity_, yaw_);
    player_.update(dt);

    // Re-express the clip pose around the character. Stripping `rootRot` matters: a turning clip already carries its
    // own yaw, and applying the controller's yaw on top of it would spin the character twice.
    std::vector<glm::vec3> sampled;
    glm::vec3 root(0.0f);
    glm::quat rootRot(1.0f, 0.0f, 0.0f, 0.0f);
    player_.sample(&sampled, &root, &rootRot);
    const glm::quat q = glm::angleAxis(yaw_, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::inverse(rootRot);
    // The clip's root is the hips, roughly a metre above its own floor. `position_` is the character's ground point,
    // so the pose is lifted by the root's height over that floor; without this a capture is buried to the waist.
    const MotionClip& clip = database_->clip(player_.index());
    const float rootHeight = root.y - clip.ground;
    pose_.resize(sampled.size());
    for (size_t i = 0; i < sampled.size(); ++i)
        pose_[i] = position_ + q * (sampled[i] - root) + glm::vec3(0.0f, rootHeight, 0.0f);

    // Foot locking runs on the finished world pose: it is the last thing that happens to a frame before it is drawn,
    // so the skinning and any debug renderer see the same feet.
    if (footLock_.enabled) {
        if (legsClip_ != player_.index()) resolveLegs(clip);
        lockFeet(clip, dt);
    }
}

// --------------------------------------------------------------------------------------------- foot locking

namespace {

const MotionJoint* findJoint(const MotionClip& clip, const char* upper, const char* lower) {
    for (const MotionJoint& j : clip.joints) {
        if (j.name == upper || j.name == lower) return &j;
    }
    return nullptr;
}

// Every joint at or under `root`, in the clip's own order. A foot correction has to move the toes with the ankle.
void collectSubtree(const MotionClip& clip, int root, std::vector<int>* out) {
    out->clear();
    if (root < 0) return;
    out->push_back(root);
    // Joints are stored parent-before-child by the BVH reader, so one forward pass is enough; a file that ordered
    // them differently still resolves because the pass repeats until it stops growing.
    for (size_t pass = 0; pass < clip.joints.size(); ++pass) {
        bool grew = false;
        for (size_t i = 0; i < clip.joints.size(); ++i) {
            const int parent = clip.joints[i].parent;
            if (parent < 0) continue;
            const bool parentIn = std::find(out->begin(), out->end(), parent) != out->end();
            const bool selfIn = std::find(out->begin(), out->end(), int(i)) != out->end();
            if (parentIn && !selfIn) { out->push_back(int(i)); grew = true; }
        }
        if (!grew) break;
    }
}

} // namespace

void MotionController::resolveLegs(const MotionClip& clip) {
    legsClip_ = player_.index();
    for (Leg& leg : legs_) leg = Leg{};
    const char* names[2][2] = {{"LeftFoot", "leftFoot"}, {"RightFoot", "rightFoot"}};
    for (int side = 0; side < 2; ++side) {
        Leg& leg = legs_[size_t(side)];
        const MotionJoint* foot = findJoint(clip, names[side][0], names[side][1]);
        if (!foot) continue;
        leg.foot = clip.indexOf(names[side][0]) >= 0 ? clip.indexOf(names[side][0]) : clip.indexOf(names[side][1]);
        if (leg.foot < 0) continue;
        // The knee is the ankle's parent and the hip is the knee's. A rig with more than two segments in the leg
        // (a twist bone, a toe base) still works: the solve uses the two outermost, which is what the pose shows.
        leg.knee = clip.joints[size_t(leg.foot)].parent;
        if (leg.knee < 0) { leg.foot = -1; continue; }
        leg.hip = clip.joints[size_t(leg.knee)].parent;
        if (leg.hip < 0) { leg.foot = -1; continue; }
        collectSubtree(clip, leg.foot, &leg.subtree);
    }
}

// Two-bone solve, then a rigid carry of the foot's subtree. Everything is metres in world space.
void MotionController::lockFeet(const MotionClip& clip, float dt) {
    lockedFeet_ = 0;
    lastFootCorrection_ = 0.0f;
    const float ground = position_.y;
    const float footprint = std::max(dt, 1e-4f);

    for (Leg& leg : legs_) {
        if (leg.foot < 0 || leg.hip < 0 || leg.knee < 0) continue;
        const glm::vec3 hip = pose_[size_t(leg.hip)];
        const glm::vec3 knee = pose_[size_t(leg.knee)];
        const glm::vec3 ankle = pose_[size_t(leg.foot)];

        // Contact is read from the clip's own motion, never from the corrected pose: a foot that has been pinned in
        // place would look stationary and the lock would never release.
        const float speed = leg.hasSample ? glm::length(ankle - leg.lastSample) / footprint : 0.0f;
        leg.lastSample = ankle;
        leg.hasSample = true;
        const bool touching = (ankle.y - ground) <= footLock_.contactHeight && speed <= footLock_.contactSpeed;

        if (touching) {
            leg.lost = 0.0f;
            if (!leg.locked) {
                leg.locked = true;
                leg.lockPos = ankle;
            } else if (glm::length(leg.lockPos - ankle) > footLock_.reach) {
                // The controller has outrun the clip's stride. Re-locking accepts the slide; clamping here would
                // straighten the leg and read worse than the skate it was hiding.
                leg.lockPos = ankle;
            }
            leg.inContact = true;
        } else {
            leg.lost += dt;
            if (leg.lost >= footLock_.releaseTime) {
                leg.locked = false;
                leg.inContact = false;
            }
        }

        // The correction to apply this frame: the full pin while locked, a decaying remainder after release.
        glm::vec3 want(0.0f);
        if (leg.locked) {
            want = leg.lockPos - ankle;
            const float len = glm::length(want);
            if (len > footLock_.reach) {
                want = want * (footLock_.reach / len);
                leg.lockPos = ankle + want;
            }
            leg.correction = want;
        } else if (glm::length(leg.correction) > 1e-5f) {
            const float fade = std::exp(-dt / std::max(footLock_.blendOut, 1e-3f) * 3.0f);
            leg.correction *= fade;
            if (glm::length(leg.correction) < 1e-5f) leg.correction = glm::vec3(0.0f);
        }
        if (leg.locked) ++lockedFeet_;
        if (glm::length(leg.correction) <= 1e-5f) continue;

        // ---- two-bone IK: hip -> knee -> ankle, with the ankle put back on the lock point.
        const glm::vec3 target = ankle + leg.correction;
        const float l1 = glm::length(knee - hip);
        const float l2 = glm::length(ankle - knee);
        if (l1 < 1e-4f || l2 < 1e-4f) continue;
        glm::vec3 toTarget = target - hip;
        float d = glm::length(toTarget);
        if (d < 1e-4f) continue;
        // Keep the leg inside its own reach: a solve outside [l1-l2, l1+l2] has no real elbow/knee.
        d = glm::clamp(d, std::fabs(l1 - l2) + 1e-3f, l1 + l2 - 1e-3f);
        const glm::vec3 u = toTarget / glm::length(toTarget);
        const glm::vec3 pole = knee - hip;
        glm::vec3 bend = pole - u * glm::dot(pole, u);
        if (glm::length(bend) < 1e-5f) bend = leg.bend;   // straight leg: reuse the last bend, or pick one below
        if (glm::length(bend) < 1e-5f) {
            bend = glm::cross(u, std::fabs(u.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
            if (glm::length(bend) < 1e-5f) continue;
        }
        bend = glm::normalize(bend);
        leg.bend = bend;
        const float a = (l1 * l1 - l2 * l2 + d * d) / (2.0f * d);
        const float h = std::sqrt(std::max(0.0f, l1 * l1 - a * a));
        const glm::vec3 newKnee = hip + u * a + bend * h;
        const glm::vec3 newTarget = hip + u * d;

        const glm::vec3 from = ankle - knee;
        const glm::vec3 to = newTarget - newKnee;
        const float fromLen = glm::length(from), toLen = glm::length(to);
        glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        if (fromLen > 1e-5f && toLen > 1e-5f) {
            const glm::vec3 f = from / fromLen, t = to / toLen;
            const float c = glm::clamp(glm::dot(f, t), -1.0f, 1.0f);
            if (c < 0.999999f) {
                glm::vec3 axis = glm::cross(f, t);
                if (glm::length(axis) < 1e-6f) axis = glm::cross(f, glm::vec3(0.0f, 0.0f, 1.0f));
                if (glm::length(axis) > 1e-6f) rot = glm::angleAxis(std::acos(c), glm::normalize(axis));
            }
        }
        // The knee, then every joint under the ankle carried rigidly about the knee. Rotating about the knee is what
        // keeps the shin's length: |newTarget - newKnee| is l2 by construction.
        pose_[size_t(leg.knee)] = newKnee;
        for (int j : leg.subtree) pose_[size_t(j)] = newKnee + rot * (pose_[size_t(j)] - knee);

        lastFootCorrection_ = std::max(lastFootCorrection_, glm::length(leg.correction));
    }
}

int MotionController::applyTo(Model& model, float scale) const {
    if (!database_ || !player_.ready() || pose_.empty()) return 0;
    MotionFrame frame;
    frame.world = pose_;
    frame.rootPos = pose_.front();
    frame.rootRot = glm::angleAxis(yaw_, glm::vec3(0.0f, 1.0f, 0.0f));
    return applyMotionFrame(model, database_->clip(player_.index()), frame, scale);
}

int applyMotionFrame(Model& model, const MotionClip& clip, const MotionFrame& frame, float scale) {
    if (frame.world.size() != clip.joints.size()) return 0;
    // Rest world positions of the model, which give each bone's own direction and length.
    std::vector<glm::vec3> rest(size_t(model.nodes.size()), glm::vec3(0.0f));
    std::vector<glm::quat> restRot(size_t(model.nodes.size()), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    for (int i : model.updateOrder) {
        const Node& n = model.nodes[size_t(i)];
        const glm::vec3 local = n.translation;
        if (n.parent >= 0) {
            rest[size_t(i)] = rest[size_t(n.parent)] + restRot[size_t(n.parent)] * local;
            restRot[size_t(i)] = restRot[size_t(n.parent)] * n.baseRotation;
        } else {
            rest[size_t(i)] = local;
            restRot[size_t(i)] = n.baseRotation;
        }
    }

    auto mapName = [&](const std::string& jointName) {
        const std::string want = lower(jointName);
        for (const auto& kv : model.humanoid) {
            const std::string key = lower(kv.first);
            if (key == want) return kv.second;
        }
        return -1;
    };

    int driven = 0;
    for (size_t j = 0; j < clip.joints.size(); ++j) {
        const int parent = clip.joints[j].parent;
        if (parent < 0) continue;
        const int node = mapName(clip.joints[j].name);
        if (node < 0) continue;
        const int modelParent = model.nodes[size_t(node)].parent;
        if (modelParent < 0) continue;

        const glm::vec3 from = frame.world[size_t(parent)];
        const glm::vec3 to = frame.world[j];
        const glm::vec3 motionDir = to - from;
        const glm::vec3 restDir = rest[size_t(node)] - rest[size_t(modelParent)];
        if (glm::dot(motionDir, motionDir) < 1e-8f || glm::dot(restDir, restDir) < 1e-8f) continue;
        const glm::vec3 a = glm::normalize(restDir);
        const glm::vec3 b = glm::normalize(motionDir);
        const float c = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
        glm::quat swing(1.0f, 0.0f, 0.0f, 0.0f);
        if (c < 0.9999f) {
            glm::vec3 axis = glm::cross(a, b);
            if (glm::dot(axis, axis) < 1e-10f) axis = glm::vec3(1.0f, 0.0f, 0.0f);
            swing = glm::angleAxis(std::acos(c), glm::normalize(axis));
        }
        const glm::quat parentRot = restRot[size_t(modelParent)];
        const glm::quat localSwing = glm::inverse(parentRot) * swing * parentRot;
        model.poseActive[size_t(node)] = 1;
        model.poseRot[size_t(node)] = glm::normalize(localSwing * model.nodes[size_t(node)].baseRotation);
        ++driven;
    }

    const int rootNode = mapName("hips") >= 0 ? mapName("hips") : (mapName("Hips") >= 0 ? mapName("Hips") : -1);
    if (rootNode >= 0) {
        model.poseActive[size_t(rootNode)] = 1;
        model.poseTrans[size_t(rootNode)] = model.nodes[size_t(rootNode)].translation + frame.rootPos * scale;
    }
    return driven;
}

} // namespace ukiyo
