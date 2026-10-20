// Motion matching: the animation layer behind a controllable character.
//
// The idea is older than the name: instead of a state machine that blends between clips, keep one database of every
// pose in every clip, describe the pose you are in and the path you want to take next as a small feature vector, and
// once every few frames jump to the frame in the database that fits best. Locomotion stops being "walk, run, turn"
// and becomes "the eight hundredth frame of walk5".
//
// This is a port of the method in Jacinth-19/MotionMatchingInUnity, with the Unity-specific parts replaced:
//
//   * the database is built from BVH motion-capture files (the reference ships LAFAN1) as well as from the engine's
//     own clips, because BVH is the format the data actually comes in;
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
// Everything is measured against the root's yaw, so a walk that curves left and a walk that curves right are the same
// data, and the same database serves any heading.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ukiyo {

struct Model;

// 24 trajectory and foot features plus 9 pose features: the head, left hand and right hand relative to the root, in
// the character's own frame. The pose block matters more than it looks. Without it the search only knows where the
// character is going, so a frame with the same velocity but the arms at the opposite end of their swing scores exactly
// as well as the one the body is already in — and the blend then has to travel half a metre to fix it.
inline constexpr int kMotionDims = 33;

// The tree is built on a subset of those dimensions, and only they decide which entries are candidates; the remaining
// dimensions are applied by the re-rank, which scores every candidate with the full cost.
//
// This is not an optimisation detail, it is what makes the search usable at corpus scale. A k-d tree over all 33
// dimensions degenerates: measured on the 77-capture corpus (490k entries), a query visited most of the tree and took
// 21-24 ms — slower than a brute-force scan over the whole database (13 ms), and worse the larger k got. In the 17
// dimensions below the same tree answers in 0.16-0.27 ms, because the points are far from uniformly distributed along
// them and pruning works. They are chosen for what separates one kind of motion from another:
// where the root goes and how fast, which way the body faces, and how high the head is — a crawl and a run can share
// a trajectory, but they cannot share a head height.
inline constexpr int kIndexDims = 17;
inline constexpr std::array<int, kIndexDims> kIndexFeatures = {
    0, 1, 2, 3, 4, 5, 6, 7,       // the four future positions of the root
    8, 9,                          // facing
    22, 23,                        // root velocity
    24, 25, 26,                    // head, relative to the root
    10, 12,                        // left foot x and z
};

// The pose block, at the end of the feature vector (dims 24..32): the head, left hand and right hand relative to the
// root. Kept as its own array on the entry too, so the re-rank and any tool can read it without knowing the layout.
inline constexpr int kPoseDims = 9;

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
    float unitScale = 1.0f;   // what the loader divided by: 0.01 for a centimetre capture, 1 for metres
    // The lowest point any joint reaches over the whole clip. A clip's root is the hips, about a metre above the
    // floor, so a controller that puts the root on the ground buries the character to the waist; this is the offset
    // that tells the two apart. Filled by MotionDatabase::build and by recentreMotionClip (which makes it 0).
    float ground = 0.0f;
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
//
// `unit` scales the joint offsets and the root translation into metres. 0 means "work it out": if the rest pose's
// hips stand higher than 10 units it is centimetres (LAFAN1, most MotionBuilder exports) and the file is divided by
// 100; otherwise it is already metres. A 10 m hip height is not a thing, so the guess is safe, and passing 1 or 100
// explicitly overrides it.
bool loadMotionBvh(const std::string& text, MotionClip* out, std::string* error = nullptr, float unit = 0.0f);
bool loadMotionBvhFile(const std::string& path, MotionClip* out, std::string* error = nullptr, float unit = 0.0f);

// Every .bvh in `folder`, sorted by name, loaded into `clips`. Skips one that fails and keeps going, reporting the
// count in `error` when asked. The capture directory is not vendored (LAFAN1 is a research licence), so this is how a
// project points the matcher at a real corpus: copy the folder in and pass its path.
int loadMotionBvhFolder(const std::string& folder, std::vector<MotionClip>* clips, std::string* error = nullptr,
                        float unit = 0.0f);

// Re-centres a clip so its first frame's root is at the origin and its feet are on y = 0, without touching the
// motion itself. Capture files start wherever the performer happened to stand, and four clips from four sessions
// would otherwise be metres apart in the pose data the matcher blends.
void recentreMotionClip(MotionClip* clip);

// Drops the rest-pose lead-in that a motion-capture export puts at the head of a file: the performer stands in a
// T-pose while the suit calibrates, and LAFAN1 keeps that hold for around three seconds. It is not motion, but the
// database indexes every frame it is given, so without this the matcher can match *into* the T-pose, and a player
// started at frame 0 stands with its arms out for three seconds while the controller asks it to run. Detected by
// shape rather than by name: both hands held wide apart (arms out) and the whole skeleton not moving, for at least
// `minSeconds`. An idle clip with the arms down is left alone. Returns the number of frames removed.
int trimRestLeadIn(MotionClip* clip, float minSeconds = 0.5f);

struct MotionSettings {
    float trajectoryStep = 0.15f;     // seconds between the four future-trajectory samples
    float velocityWindow = 0.10f;     // seconds either side of a frame for finite-difference velocity
    float continuity = 0.35f;         // cost per second of frame jump, which keeps the walk coherent
    float switchMargin = 0.02f;       // a new match must beat staying on the current frame by this much
    // Cost per metre of mean body-pose difference, added to the trajectory cost of every candidate and to the cost of
    // staying. It has to be part of the cost, not a tie-break applied afterwards: the switch decision compares the
    // winner with staying, and a term that only applies to one side of that comparison makes the decision meaningless.
    // Without it, a corpus that contains rolls, dances and getting off the ground matches "run forward at 1.6 m/s"
    // with a forward roll, because a roll does travel forward at about that speed and the trajectory features do not
    // care that the character is horizontal.
    float poseWeight = 1.0f;
    int rerankCandidates = 48;        // how many trajectory neighbours the pose term re-orders (1..64)
    // Feature weights. Trajectory dominates, because where the player wants to go is what the cut is for.
    std::array<float, kMotionDims> weights{
        1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,    // trajectory
        0.60f, 0.60f,                                              // facing
        0.55f, 0.55f, 0.55f, 0.55f, 0.55f, 0.55f,                  // foot positions
        0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f,                  // foot velocities
        0.50f, 0.50f,                                              // root velocity
        // Pose. Lighter than the trajectory: where the player wants to go still wins, and the pose decides between
        // the frames that go there equally well.
        0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f, 0.45f,
    };
};

// One pose in the database: where it came from in the clips and what it looks like as features.
struct MotionEntry {
    int clip = 0;
    int frame = 0;
    std::array<float, kMotionDims> feature{};
    std::array<float, kPoseDims> pose{};   // also the last kPoseDims entries of `feature`
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

    // The distance the tree prunes with: the kIndexFeatures subset only. `nearest` returns the closest entries under
    // this metric; `cost` is what ranks them afterwards.
    float indexDistance(const std::array<float, kMotionDims>& a,
                        const std::array<float, kMotionDims>& b) const;

    // The cost of one candidate against `query`, including the body-pose term against `poseFrom`. One function for
    // every comparison, so a candidate and the motion already playing are priced the same way.
    float cost(const std::array<float, kMotionDims>& query, int entry, int poseFrom) const;

    // Mean distance, in metres, between the body poses of two entries: every joint, in the character's own frame,
    // relative to the root. The whole skeleton rather than a few landmark joints — the landmark subset cannot tell a
    // run from a forward roll, and a full 27-joint comparison costs a few microseconds and needs no extra memory
    // because the frames are already in the clips.
    float poseDistance(int a, int b) const;

    // The cost of staying on `from` and walking forward for `seconds`, against `query`. Used to decide whether a
    // match is worth switching to.
    float continuationCost(const std::array<float, kMotionDims>& query, int from, float seconds, int poseFrom = -1) const;

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

// The decay curve at the heart of an inertialized blend: the offset that has to be subtracted from a new clip so the
// pose leaves the old one at the old one's own velocity. `p` is the position offset at the jump, `v` the velocity
// offset, `s` the blend's progress in 0..1.
//
// It is the quintic that satisfies x(0)=p, x'(0)=v, x''(0)=0, x(1)=0, x'(1)=0, x''(1)=0, in closed form:
//
//     x(s) = p + v s - (10p + 6v) s^3 + (15p + 8v) s^4 - (6p + 3v) s^5
//
// A smoothstep crossfade also ends at zero velocity, but it *starts* at zero velocity, so the character stops dead
// for a frame at every jump. Matching x'(0) is what removes that stop; that is the whole of inertialization.
glm::vec3 inertialize(glm::vec3 p, glm::vec3 v, float s);

// The derivative of the curve above with respect to `s`, in position units per unit of normalised progress. A new
// match that lands mid-blend has to continue the offset that is still on screen, and this is what carries it over.
glm::vec3 inertializeVelocity(glm::vec3 p, glm::vec3 v, float s);

// Plays the database back. update() advances at the clip's own frame rate, re-queries every `matchInterval`
// seconds, and inertializes the joint positions over `blendTime` when the match jumps. The result is a pose in the
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
    // The largest offset the last match had to blend away, in metres. It is the size of the pop the matcher could not
    // avoid, and the single number that says whether the database is working: a corpus whose clips disagree about
    // foot placement or body size shows it here, and so does a pose term that is too weak.
    float lastMatchOffset() const { return lastMatchOffset_; }

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
    float blendTime_ = 0.20f;
    float matchInterval_ = 0.16f;
    float cost_ = 0.0f;
    float lastMatchOffset_ = 0.0f;
    int matches_ = 0;
    // At a jump: the outgoing pose and its velocity, and the incoming pose's velocity. Their difference is the
    // velocity the inertialization has to start from.
    std::vector<glm::vec3> blendFrom_;
    std::vector<glm::vec3> blendVel_;
    glm::vec3 blendRoot_{0.0f};
    glm::vec3 blendRootVel_{0.0f};
    glm::quat blendRot_{1.0f, 0.0f, 0.0f, 0.0f};
};

// Foot locking: the correction that turns a matched walk into a planted one.
//
// The matcher picks the frame whose *trajectory* is closest to the wish, but the stride it picked has its own speed.
// When the character accelerates, or crosses between two clips whose strides differ, the difference shows up as a foot
// that keeps moving after it has touched down. The controller owns the world transform, so it can see the foot in
// world space, and that is where the fix belongs.
//
// While a foot is in contact — low over the character's ground plane and slow in the clip — its world position is
// remembered. Every later frame of that contact the foot is put back on that point by a two-bone solve on the hip,
// knee and ankle, with the foot and its descendants carried rigidly by the same rotation. Nothing is keyframed and
// nothing is authored: the lock is derived from the pose the matcher already produced, and it releases the moment the
// clip lifts the foot.
//
// Two details stop it from looking like IK. The bend direction is carried from frame to frame, so the knee does not
// flip; and a lock that would over-extend the leg (the controller has outrun the clip's stride) is dropped rather than
// clamped, because a hyper-extended leg reads worse than a slide.
struct FootLockSettings {
    bool enabled = true;
    float contactHeight = 0.06f;   // metres over the character's ground plane
    float contactSpeed = 0.90f;    // metres per second, measured from the clip's own pose, not the locked one
    float releaseTime = 0.07f;     // seconds of lost contact before the lock lets go (hysteresis against a flicker)
    float reach = 0.32f;           // metres of correction; past this the stride is wrong and the lock is dropped
    float blendOut = 0.09f;        // seconds for the correction to fade after release, so the foot does not snap back
};

// A character driven by motion matching: input in, world pose out. The controller owns the world transform — the
// clip's root motion is discarded, so a gamepad, an AI or a cutscene curve can all drive it — and every frame it asks
// the player for the pose that fits the wanted velocity, then re-expresses that pose around the character's own
// position and heading.
//
// The heading is the velocity's, eased, so a character that strafes still faces forward; that is the whole point of
// the trajectory features. Call update() once a frame, then read pose() for skinning or debug drawing.
class MotionController {
public:
    void setDatabase(const MotionDatabase* database);
    void reset(glm::vec3 position = glm::vec3(0.0f), float yaw = 0.0f);

    // `wishDir` is the desired world direction on the ground plane, `wishSpeed` metres per second, `dt` seconds.
    // A zero wishDir decelerates towards the idle clip. `facing` is the heading the body should settle to, which a
    // third-person camera usually owns; pass the velocity heading to let the character turn into its motion.
    void update(glm::vec3 wishDir, float wishSpeed, float dt, float facing);

    // World-space joint positions for the current frame, in the clip's joint order.
    const std::vector<glm::vec3>& pose() const { return pose_; }
    glm::vec3 position() const { return position_; }
    float yaw() const { return yaw_; }
    float speed() const { return glm::length(velocity_); }
    const MotionPlayer& player() const { return player_; }

    // Writes the current pose onto a model's humanoid rig, if the clip's joints line up with the rig's names.
    int applyTo(Model& model, float scale = 1.0f) const;

    // Foot locking is on by default; a clip whose feet are not named (or a debug view of the raw match) can turn it off.
    FootLockSettings& footLock() { return footLock_; }
    const FootLockSettings& footLock() const { return footLock_; }
    // How many feet were locked on the last update, and the largest correction it applied, in metres. Both are for
    // the debug overlay and for tests; neither affects playback.
    int lockedFeet() const { return lockedFeet_; }
    float lastFootCorrection() const { return lastFootCorrection_; }

private:
    // One leg, resolved from the clip's joint names once per clip. `subtree` is the foot and everything under it, so a
    // correction carries the toes (and anything else parented to the ankle) rigidly.
    struct Leg {
        int hip = -1, knee = -1, foot = -1;
        std::vector<int> subtree;
        bool inContact = false;
        bool locked = false;
        glm::vec3 lockPos{0.0f};
        glm::vec3 correction{0.0f};   // world offset currently applied, for a smooth release
        glm::vec3 lastSample{0.0f};   // the clip's own foot position last frame, before any correction
        bool hasSample = false;
        float lost = 0.0f;            // seconds since contact was last seen
        glm::vec3 bend{0.0f};         // the knee's bend direction, carried across frames so it cannot flip
    };

    void resolveLegs(const MotionClip& clip);
    void lockFeet(const MotionClip& clip, float dt);

    const MotionDatabase* database_ = nullptr;
    MotionPlayer player_;
    glm::vec3 position_{0.0f};
    glm::vec3 velocity_{0.0f};
    float yaw_ = 0.0f;
    std::vector<glm::vec3> pose_;
    float acceleration_ = 7.0f;    // m/s^2 towards the wish velocity
    float turnRate_ = 7.5f;        // radians per second towards the facing
    FootLockSettings footLock_{};
    std::array<Leg, 2> legs_{};
    int legsClip_ = -1;            // which clip the legs were resolved for, -1 before the first update
    int lockedFeet_ = 0;
    float lastFootCorrection_ = 0.0f;
};

// Writes a motion frame onto a model's humanoid rig, as the animation pose the rest of the engine already consumes
// (poseActive / poseRot / poseTrans). The retarget is direction-based: each bone is rotated by the swing from its
// rest direction to the motion's direction, and bone lengths are left as the model has them, so a short rig and a
// tall one both read correctly even though the capture is 1.8 m tall. Returns how many bones were driven.
int applyMotionFrame(Model& model, const MotionClip& clip, const MotionFrame& frame, float scale = 1.0f);

} // namespace ukiyo
