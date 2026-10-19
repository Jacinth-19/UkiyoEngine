#include "engine/motion_match.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
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

bool loadMotionBvh(const std::string& text, MotionClip* out, std::string* error) {
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
            glm::mat4 local = glm::translate(glm::mat4(1.0f), joint.offset);
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

    *out = std::move(clip);
    return true;
}

bool loadMotionBvhFile(const std::string& path, MotionClip* out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "could not open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    const bool ok = loadMotionBvh(text, out, error);
    if (ok && out) out->name = path.substr(path.find_last_of("/\\") + 1);
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
        const int root = clip.root;
        const int leftFoot = clip.indexOf("LeftFoot") >= 0 ? clip.indexOf("LeftFoot") : clip.indexOf("leftFoot");
        const int rightFoot = clip.indexOf("RightFoot") >= 0 ? clip.indexOf("RightFoot") : clip.indexOf("rightFoot");
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
    for (int a = 0; a < kMotionDims; ++a) {
        float mn = kInf, mx = -kInf;
        for (int i = lo; i < hi; ++i) {
            const float v = entries_[size_t((*order)[size_t(i)])].feature[size_t(a)];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (mx - mn > bestSpread) {
            bestSpread = mx - mn;
            axis = a;
        }
    }
    const int mid = (lo + hi) / 2;
    std::nth_element(order->begin() + lo, order->begin() + mid, order->begin() + hi, [&](int a, int b) {
        return entries_[size_t(a)].feature[size_t(axis)] < entries_[size_t(b)].feature[size_t(axis)];
    });
    const int node = int(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[size_t(node)].entry = (*order)[size_t(mid)];
    nodes_[size_t(node)].axis = axis;
    nodes_[size_t(node)].split = entries_[size_t((*order)[size_t(mid)])].feature[size_t(axis)];
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
    const float d = weightedDistance(query, entries_[size_t(n.entry)].feature);
    if (int(best->size()) < k) {
        best->emplace_back(d, n.entry);
        std::push_heap(best->begin(), best->end());
    } else if (d < best->front().first) {
        std::pop_heap(best->begin(), best->end());
        best->back() = {d, n.entry};
        std::push_heap(best->begin(), best->end());
    }
    const float delta = query[size_t(n.axis)] - n.split;
    const float plane = settings_.weights[size_t(n.axis)] * delta * delta;
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
    const float speed = glm::length(glm::vec2(velocity.x, velocity.z));
    const float yaw = speed > 0.05f ? std::atan2(velocity.x, velocity.z) : facingYaw;
    const glm::quat rot = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    for (int i = 0; i < 4; ++i) {
        const float t = float(i + 1) * settings_.trajectoryStep;
        // The database's local frame points forward along +z, so a desired speed is a straight line up that axis.
        q[size_t(i * 2 + 0)] = 0.0f;
        q[size_t(i * 2 + 1)] = speed * t;
    }
    const glm::vec3 forward = rot * glm::vec3(0.0f, 0.0f, 1.0f);
    q[8] = forward.x;
    q[9] = forward.z;
    return q;
}

float MotionDatabase::continuationCost(const std::array<float, kMotionDims>& query, int from, float seconds) const {
    if (from < 0 || from >= size()) return kInf;
    const MotionEntry& e = entries_[size_t(from)];
    const MotionClip& clip = clips_[size_t(e.clip)];
    const int ahead = e.frame + int(std::lround(seconds * clip.fps));
    if (ahead >= clip.frameCount() - 1) return kInf;
    const int entry = entryAt(e.clip, ahead);
    if (entry < 0) return kInf;
    return weightedDistance(query, entries_[size_t(entry)].feature);
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
    int candidates[8];
    const int n = database_->nearest(query, 8, candidates);
    int best = -1;
    float bestCost = kInf;
    for (int i = 0; i < n; ++i) {
        const int idx = candidates[i];
        const int clipIndex = database_->entry(idx).clip;
        const int frame = database_->entry(idx).frame;
        float cost = database_->weightedDistance(query, database_->entry(idx).feature);
        // A jump to another clip, or far along this one, is a visual cut: price it.
        const int currentFrame = int(std::lround(frame_));
        const float seconds = clipIndex == lastClip_ ? std::fabs(float(frame - currentFrame)) /
                                                           std::max(1.0f, database_->clip(idx).fps)
                                                     : 1.0f;
        cost += database_->settings().continuity * seconds;
        if (cost < bestCost) {
            bestCost = cost;
            best = idx;
        }
    }
    if (costOut) *costOut = bestCost;
    return best;
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

    // The pose half of the query is where the character is now, not the entry it last jumped to.
    const int currentEntry = database_->entryAt(lastClip_, int(std::lround(frame_)));
    const auto query = database_->query(velocity_, facing_, currentEntry >= 0 ? currentEntry : index_);
    float winnerCost = 0.0f;
    const int winner = bestMatch(query, &winnerCost);
    if (winner < 0) return;

    // Compare against walking on for one match interval. Staying put would win forever if it were compared with the
    // current frame: the trajectory is relative, so "stand still" always looks like a perfect local fit.
    const float stay = currentEntry >= 0 ? database_->continuationCost(query, currentEntry, matchInterval_) : kInf;
    if (winnerCost + database_->settings().switchMargin >= stay && blend_ >= 1.0f) return;

    // Sample the outgoing pose before the jump, so the blend starts from what is on screen.
    std::vector<glm::vec3> outPositions;
    glm::vec3 outRoot(0.0f);
    glm::quat outRot(1.0f, 0.0f, 0.0f, 0.0f);
    sample(&outPositions, &outRoot, &outRot);
    blendFrom_ = outPositions;
    blendRoot_ = outRoot;
    blendRot_ = outRot;

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
        const float w = blend_ * blend_ * (3.0f - 2.0f * blend_);
        for (size_t j = 0; j < positions->size(); ++j) (*positions)[j] = glm::mix(blendFrom_[j], (*positions)[j], w);
        if (rootPos) *rootPos = glm::mix(blendRoot_, p, w);
        if (rootRot) *rootRot = glm::slerp(blendRot_, r, w);
        return;
    }
    if (rootPos) *rootPos = p;
    if (rootRot) *rootRot = r;
}

// ---------------------------------------------------------------------------------------------------------------
// Retarget

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
    pose_.resize(sampled.size());
    for (size_t i = 0; i < sampled.size(); ++i) pose_[i] = position_ + q * (sampled[i] - root);
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
