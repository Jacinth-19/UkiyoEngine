// Skeletal animation: GLB clip library (Quaternius Universal Animation Library, CC0),
// humanoid retargeting onto any VRM model, crossfading and an idle state machine.
//
// Retargeting works on world-space rotation deltas. For each mapped bone the source
// animation contributes D = Sworld(t) * inverse(Sworld_rest); the target bone gets
// D * Tworld_rest. Both rigs are T-pose at rest, so the deltas map directly. Hip
// translation is scaled by the ratio of leg lengths.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ukiyo {

struct Model;

// One animated property of one source node.
struct AnimTrack {
    enum class Path : uint8_t { Translation, Rotation, Scale };
    enum class Interp : uint8_t { Linear, Step, Cubic };
    int node = -1;                 // index into SourceRig::names
    Path path = Path::Rotation;
    Interp interp = Interp::Linear;
    std::vector<float> times;
    std::vector<float> values;     // 3 floats (T, S) or 4 (R, xyzw) per key; cubic: 3 entries per key
};

// A marker on a clip's timeline, such as a footstep. Games read them with eventsBetween.
struct AnimEvent {
    float time = 0.0f;
    std::string name;
};

struct AnimClip {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimTrack> tracks;
    std::vector<AnimEvent> events;   // sorted by time when the editor writes them
};

// Editor changes to a library's clips: replaced tracks, added or removed tracks, and event lists. They
// are stored in a .ukanim file beside the library and applied on top of the glTF clips, which are never
// written to.
struct ClipEdits {
    struct TrackEdit {
        std::string clip;
        std::string node;                                      // rig node name
        AnimTrack::Path path = AnimTrack::Path::Translation;
        AnimTrack::Interp interp = AnimTrack::Interp::Linear;  // Linear or Step only
        std::vector<float> times;     // empty means the track is removed from the clip
        std::vector<float> values;    // 3 floats per key (translation), 4 (rotation xyzw)
    };
    struct EventSet {
        std::string clip;
        std::vector<AnimEvent> events;                         // replaces the clip's events
    };
    std::vector<TrackEdit> tracks;   // each replaces the source track for (clip, node, path)
    std::vector<EventSet> eventSets;
};

// Rest pose and hierarchy of the source rig (the clip library skeleton).
struct SourceRig {
    std::vector<std::string> names;
    std::vector<int> parent;
    std::vector<glm::vec3> restT;
    std::vector<glm::quat> restR;
    std::vector<glm::vec3> restS;
};

class AnimLibrary {
public:
    // Loads every animation in a glTF/GLB file. Throws std::runtime_error on failure.
    static std::unique_ptr<AnimLibrary> load(const std::string& path);

    const SourceRig& rig() const { return rig_; }
    const std::vector<AnimClip>& clips() const { return clips_; }
    // The clips as they were loaded, before any ClipEdits.
    const std::vector<AnimClip>& sourceClips() const { return source_; }
    // Replaces the playable clips (the result of applyClipEdits). Clip order must match sourceClips().
    void setClips(std::vector<AnimClip> clips) { clips_ = std::move(clips); }
    int find(const std::string& name) const;   // -1 when absent
    std::string path() const { return path_; }

private:
    SourceRig rig_;
    std::vector<AnimClip> clips_;
    std::vector<AnimClip> source_;
    std::string path_;
};

// Applies edits to a copy of the source clips. Edits for unknown clips or nodes are ignored, and a track
// whose value count does not match its key count is ignored. The result keeps the source clip order, so
// clip indices stay valid. A clip's duration grows to cover its last key or event.
std::vector<AnimClip> applyClipEdits(const SourceRig& rig, const std::vector<AnimClip>& source, const ClipEdits& edits);

// Events with time in (t0, t1]. The window does not wrap.
std::vector<AnimEvent> eventsBetween(const AnimClip& clip, float t0, float t1);

// Samples a clip at time t (wrapped to its duration). Nodes without a track keep rest.
void sampleClip(const AnimClip& clip, const SourceRig& rig, float t,
                std::vector<glm::vec3>& outT, std::vector<glm::quat>& outR);

// Weighted average of rotations. Each one is flipped to the hemisphere of the first weighted rotation
// before summing, so the average takes the short way round. Weights need not sum to 1.
// Returns identity when no weight is positive.
glm::quat blendRotations(const std::vector<glm::quat>& rotations, const std::vector<float>& weights);

// Idle state machine. It picks variants at random (seeded) and crossfades between them.
// Each variant plays a random number of loops, then the next one is chosen.
struct IdleVariant {
    std::string clip;
    int minLoops = 2;
    int maxLoops = 4;
    bool handOnHip = false;        // drives the hand-on-waist IK goal
};

// Per-clip playback options inside a blend: speed (1 = normal) and whether the clip is mirrored left to right.
struct MotionOptions {
    float timeScale = 1.0f;
    bool mirror = false;
};

// Reflection across the body's centre line (x = 0 in the model's frame). A rotation (w, x, y, z) becomes
// (w, x, -y, -z); a translation (x, y, z) becomes (-x, y, z). Applying it twice gives back the input.
glm::quat mirrorRotation(const glm::quat& q);
// The name a bone would have on the other side (upperarm_l -> upperarm_r). Empty when the name has no side token.
std::string mirrorBoneCandidate(const std::string& name);
glm::vec3 mirrorTranslation(const glm::vec3& v);

class AnimPlayer {
public:
    // Binds to a model: builds the bone map and the rest data. Call once per model.
    void bind(const Model& model, const AnimLibrary& lib);
    bool bound() const { return bound_; }

    // Explicit control. Plays `clip` on loop with a crossfade. Clears the idle machine.
    // startFraction: where in the clip playback begins (0 = start).
    void play(const std::string& clip, float fade = 0.3f, float startFraction = 0.0f);

    // Plays several clips together, mixed by weights (one per clip; a missing weight counts as 0). The clips
    // keep their own clocks, starting now, and their poses are combined per bone each frame. Clears the idle
    // machine. Throws if a clip is not in the library.
    // options, when given, is one entry per clip (time scale and mirror). Missing entries use the defaults.
    void playBlend(const std::vector<std::string>& clips, const std::vector<float>& weights, float fade = 0.3f,
                   const std::vector<MotionOptions>& options = {});
    // Changes the weights of the blend that is playing. The clips keep playing, so no crossfade is made.
    void setBlendWeights(const std::vector<float>& weights);

    // Enables the idle state machine. An empty list uses the default five variants.
    void startIdle(uint32_t seed, std::vector<IdleVariant> variants = {}, float fade = 0.6f);
    void stop();

    // Advances the clock and writes the pose overlays and hand goals into the model.
    // Call before updateModel, once per frame.
    void update(Model& model, float time, float dt);

    // Holds one hand on the waist (0 right, 1 left) until changed. -1 releases it.
    void forceHandOnHip(int side) { forcedHip_ = side; }

    // Current variant name (idle mode) or clip name.
    const std::string& current() const { return currentName_; }
    int changes() const { return changes_; }

    static std::vector<IdleVariant> defaultIdleVariants();

private:
    struct Member {
        int clip = -1;          // index into the library's clips
        float start = 0.0f;     // wall time when this member's clock started
        float timeScale = 1.0f; // clip time advances this many seconds per second
        bool mirror = false;    // the sampled pose is reflected left to right
    };
    struct Layer {
        int clip = -1;          // a single clip; unused when the layer is a blend
        float start = 0.0f;     // wall time when the clip started (its loop clock)
        float fadeFrom = 0.0f;  // wall time when this layer began fading in
        float fadeTime = 0.3f;
        bool active = false;
        std::vector<Member> members;   // a blend layer: the clips it mixes (empty for a single clip)
        std::vector<float> weights;    // one per member
    };

    void beginClip(int clipIndex, float now, float fade, float startFraction = 0.0f);
    void pickNextVariant(float now);
    void blendPose(Model& model, float now, float dt);
    uint32_t nextRandom();

    bool bound_ = false;
    const AnimLibrary* lib_ = nullptr;
    std::vector<int> srcForTarget_;      // per model node: source node or -1
    std::vector<int> srcOrder_;          // source nodes, parents first
    std::vector<int> srcMirror_;         // per source node: the node on the other side (itself when central)
    std::vector<glm::vec3> targetRestT_; // rest local per model node
    std::vector<glm::quat> targetRestR_;
    std::vector<glm::quat> targetWorldRestR_;
    std::vector<glm::vec3> targetWorldRestP_;
    std::vector<glm::vec3> sourceWorldRestP_;
    std::vector<glm::quat> sourceWorldRestR_;
    float hipScale_ = 1.0f;
    // Change of basis from the source (UAL, +Z forward) to the target model frame. VRM 0.x
    // models face -Z in glTF and carry a 180 degree root turn, so deltas are conjugated by it.
    glm::quat frame_{1.0f, 0.0f, 0.0f, 0.0f};
    int hipsNode_ = -1;

    Layer layers_[2];                    // [activeLayer_] is current; the other fades out
    int activeLayer_ = 0;
    std::string currentName_;
    int changes_ = 0;
    float lastTime_ = 0.0f;

    bool idle_ = false;
    bool pendingStart_ = false;
    std::vector<IdleVariant> variants_;
    int variantIndex_ = -1;
    int loopsLeft_ = 0;
    float loopEnd_ = 0.0f;               // wall time when the current variant stops looping
    uint32_t rng_ = 1;
    float idleFade_ = 0.6f;
    bool handOnHipWanted_ = false;
    int handSide_ = 0;                   // 0 = right, 1 = left (hand-on-waist side)
    float hipLevel_[2] = {0.0f, 0.0f};   // eased hand-on-waist levels per arm
    int forcedHip_ = -1;
};

} // namespace ukiyo
