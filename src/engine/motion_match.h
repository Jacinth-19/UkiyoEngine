// Motion matching, the animation layer behind a controllable character.
//
// The idea is older than the name: instead of a state machine that blends between clips, keep one database of
// every pose in every clip, describe the pose you are in and the path you want to take next as a small feature
// vector, and once every few frames jump to the frame in the database that fits best. Locomotion stops being
// "walk, run, turn" and becomes "the eight hundredth frame of walk5".
//
// This is a port of the method in Jacinth-19/MotionMatchingInUnity, with the Unity-specific parts replaced:
//
//   * the database is built from BVH motion-capture files (the reference ships LAFAN1) as well as from the
//     engine's own clips, because BVH is the format the data actually comes in;
//   * the nearest-neighbour search is a static KD-tree over the feature vectors, built once; the reference does a
//     linear scan, which is fine for a demo and not for a thousand frames;
//   * the cut is chosen with a continuity term, so the matcher prefers the frame it is already walking into over a
//     marginally better one in another clip.
//
// The features are 24 numbers, all in the character's own frame:
//
//   0..7   the future trajectory: root position at +0.15 s, +0.30 s, +0.45 s, +0.60 s, as (x, z) pairs
//   8..9   the facing direction at the end of that trajectory
//   10..12, 13..15  left and right foot position
//   16..18, 19..21  left and right foot velocity
//   22..23  the root velocity
//
// Everything is measured against the root's yaw, so a walk that curves left and a walk that curves right are the
// same data, and the same database serves any heading.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ukiyo {

struct Model;

inline constexpr int kMotionDims = 24;

struct MotionJoint {
    std::string name;
    int parent = -1;
    glm::vec3 offset{0.0f};
};

struct MotionFrame {
    std::vector<glm::vec3> world;   // one world position per joint
    glm::vec3 rootPos{0.0f};
    glm::quat rootRot{1.0f, 0.0f, 0.0f, 0.0f};   // yaw only: the character's facing
};

struct MotionClip {
    std::string name;
    float fps = 30.0f;
    int root = 0;
    std::vector<MotionJoint> joints;
    std::vector<MotionFrame> frames;

    int frameCount() const { return static_cast<int>(frames.size()); }
    float duration() const { return fps > 0.0f ? float(frames.size()) / fps : 0.0f; }
    int indexOf(const std::string& joint) const;
};

// Parses the BVH text format: HIERARCHY with OFFSET/CHANNELS per joint, then MOTION with one line of channel values
// per frame. Channel order is taken from the file, including the six-channel root (position + rotation), so LAFAN1,
// CMU and Mixamo exports all load. Returns false and fills `error` on a malformed file.
bool loadMotionBvh(const std::string& text, MotionClip* out, std::string* error = nullptr);
bool loadMotionBvhFile(const std::string& path, MotionClip* out, std::string* error = nullptr);

struct MotionSettings {
    float trajectoryStep = 0.15f;     // seconds between the four future-trajectory samples
    float velocityWindow = 0.10f;     // seconds either side of a frame for finite-difference velocity
    float continuity = 0.35f;         // cost per second of frame jump, which keeps the walk coherent
    float switchMargin = 0.02f;       // a new match must beat staying on the current frame by this much
    // Feature weights. Trajectory dominates, because where the player wants to go is what the cut is for.
    std::array<float, kMotionDims> weights{
        1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,    // trajectory
        0.60f, 0.60f,                                              // facing
        0.55f, 0.55f, 0.55f, 0.55f, 0.55f, 0.55f,                  // foot positions
        0.40f, 0.40f, 0.40f, 0.40f, 0.40f, 0.40f,                  // foot velocities
        0.50f, 0.50f,                                              // root velocity
    };
};

// One pose in the database: where it came from in the clips and what it looks like as features.
struct MotionEntry {
    int clip = 0;
    int frame = 0;
    std::array<float, kMotionDims> feature{};
};

// A static, balanced KD-tree over the entries. Splits alternate with the widest-spread dimension; a query walks the
// tree and prunes subtrees whose split plane is already further than the current worst neighbour.
class MotionDatabase {
public:
    bool build(std::vector<MotionClip> clips, const MotionSettings& settings = {}, std::string* error = nullptr);

    int size() const { return static_cast<int>(entries_.size()); }
    bool empty() const { return entries_.empty(); }
    const MotionEntry& entry(int i) const { return entries_[static_cast<size_t>(i)]; }
    const MotionClip& clip(int i) const { return clips_[static_cast<size_t>(entries_[static_cast<size_t>(i)].clip)]; }
    int frameOf(int i) const { return entries_[static_cast<size_t>(i)].frame; }
    // The entry of a particular frame of a clip (entries are stored clip by clip, frame by frame).
    int entryAt(int clip, int frame) const;
    int clipStart(int clip) const { return clip >= 0 && clip < int(clipStart_.size()) ? clipStart_[size_t(clip)] : 0; }
    const std::vector<MotionClip>& clips() const { return clips_; }
    const MotionSettings& settings() const { return settings_; }

    // The k nearest entries to `query`, best first, as a weighted squared distance. Fewer than k may be returned.
    int nearest(const std::array<float, kMotionDims>& query, int k, int* out) const;

    // Builds a query from where the character is (the pose of database entry `poseFrom`) and where it is going.
    // `velocity` is world units per second in world space; `facingYaw` is the heading the character should have at
    // the end of the trajectory (ignored when the velocity is zero).
    std::array<float, kMotionDims> query(glm::vec3 velocity, float facingYaw, int poseFrom) const;

    float weightedDistance(const std::array<float, kMotionDims>& a,
                           const std::array<float, kMotionDims>& b) const;

    // The cost of staying on `from` and walking forward for `seconds`, against `query`. Used to decide whether a
    // match is worth switching to.
    float continuationCost(const std::array<float, kMotionDims>& query, int from, float seconds) const;

private:
    struct Node {
        int entry = -1;
        int axis = 0;
        float split = 0.0f;
        int left = -1, right = -1;
    };
    int buildNode(std::vector<int>* order, int lo, int hi);
    void search(int node, const std::array<float, kMotionDims>& query, int k,
                std::vector<std::pair<float, int>>* best) const;

    std::vector<MotionClip> clips_;
    std::vector<MotionEntry> entries_;
    std::vector<int> clipStart_;   // first entry of each clip, or -1
    std::vector<Node> nodes_;
    int root_ = -1;
    MotionSettings settings_{};
};

// Plays the database back. update() advances at the clip's own frame rate, re-queries every `matchInterval`
// seconds, and crossfades the joint positions over `blendTime` when the match jumps. The result is a pose in the
// same space as the clip: world joint positions and a yaw-only root rotation.
class MotionPlayer {
public:
    void setDatabase(const MotionDatabase* database) { database_ = database; reset(); }
    void reset();
    void start(int entry, float frame = 0.0f);

    void setTarget(glm::vec3 velocity, float facingYaw);
    void update(float dt);

    bool ready() const { return database_ && index_ >= 0; }
    int index() const { return index_; }
    int clipIndex() const { return ready() ? lastClip_ : -1; }
    float frame() const { return frame_; }
    float matchCost() const { return cost_; }
    int matchCount() const { return matches_; }

    // Interpolated pose. `positions` gets one world position per joint of the current clip.
    void sample(std::vector<glm::vec3>* positions, glm::vec3* rootPos, glm::quat* rootRot) const;

private:
    int bestMatch(const std::array<float, kMotionDims>& query, float* costOut) const;

    const MotionDatabase* database_ = nullptr;
    int index_ = -1;
    int lastClip_ = -1;
    float frame_ = 0.0f;
    glm::vec3 velocity_{0.0f};
    float facing_ = 0.0f;
    float sinceMatch_ = 0.0f;
    float blend_ = 1.0f;           // 0 at a jump, 1 once the blend is done
    float blendTime_ = 0.18f;
    float matchInterval_ = 0.16f;
    float cost_ = 0.0f;
    int matches_ = 0;
    // The pose we blended from, sampled at the moment of the jump.
    std::vector<glm::vec3> blendFrom_;
    glm::vec3 blendRoot_{0.0f};
    glm::quat blendRot_{1.0f, 0.0f, 0.0f, 0.0f};
};

// Writes a motion frame onto a model's humanoid rig, as the animation pose the rest of the engine already consumes
// (poseActive / poseRot / poseTrans). The retarget is direction-based: each bone is rotated by the swing from its
// rest direction to the motion's direction, and bone lengths are left as the model has them, so a short rig and a
// tall one both read correctly even though the capture is 1.8 m tall. Returns how many bones were driven.
int applyMotionFrame(Model& model, const MotionClip& clip, const MotionFrame& frame, float scale = 1.0f);

} // namespace ukiyo
