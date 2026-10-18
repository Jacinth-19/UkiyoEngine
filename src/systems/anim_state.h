// Animation state machine: picks which clip plays from named parameters. It does not sample poses.
// Changes go to a play callback, which is normally AnimPlayer::play(clip, fade, startFraction).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engine/anim.h"   // MotionOptions

namespace ukiyo {

// How a state picks its clip. None plays one clip (the original states). The others are blend trees
// that mix several motions from float parameters.
enum class BlendKind : std::uint8_t {
    None,
    Linear1D,              // motions lie on the X axis; the weights interpolate between neighbours
    SimpleDirectional2D,   // motions are directions; the angle of (X, Y) picks and mixes them
    FreeformCartesian2D,   // motions sit at points in the XY plane; inverse-distance weights
    FreeformDirectional2D, // motions are directions with lengths; the input's angle and length pick and mix them
    Direct,                // each motion has its own float parameter; its weight is that value, normalised
};

// One motion of a blend tree, placed at (x, y). A 1D tree uses only x.
struct BlendMotion {
    std::string clip;
    float x = 0.0f;
    float y = 0.0f;
    float timeScale = 1.0f;   // playback speed of this clip inside the blend (1 = normal)
    bool mirror = false;      // play the clip reflected left to right
    std::string param;        // Direct only: the float parameter that sets this motion's weight
};

// Direct blend weights: each value is clamped at 0, then the values are divided by their sum. All zero when
// every value is zero (the blend then plays nothing new).
std::vector<float> directWeights(const std::vector<float>& values);

// Spreads the motions' x positions evenly between the smallest and largest x already set, in list order.
// One motion keeps its x. Used by the editor's Automate Thresholds.
std::vector<BlendMotion> spreadThresholds(std::vector<BlendMotion> motions);

// Weight of each motion at the parameter point (x, y). The weights are not negative and sum to 1,
// or are empty when there are no motions.
//  - Linear1D: the two motions on either side of x share the weight linearly. Outside the range the end motion wins.
//  - SimpleDirectional2D: the angle of (x, y) mixes the two nearest directions. Its length moves the
//    mix toward the motion at (0, 0) when there is one (idle). A length of 1 or more gives the full direction.
//  - FreeformCartesian2D: weight is proportional to 1 / distance squared. A point on a motion gives it weight 1.
//  - FreeformDirectional2D: each motion is a (direction, length). Its distance from the input is the angle
//    between them, scaled by the input's length, plus the difference in length. The motion at (0, 0) is
//    at distance equal to the input's length, so a short input leans toward idle. Several motions may share a
//    direction; they split by length. Weight is proportional to 1 / distance squared, and a motion's own
//    point gives it weight 1. The editor requires one motion at (0, 0), the idle pose (Unity's rule);
//    the weights themselves do not depend on it.
std::vector<float> blendWeights(BlendKind kind, const std::vector<BlendMotion>& motions, float x, float y);

// Stable names used in the .ukanim file: "clip", "linear1d", "simple2d", "freeform2d", "freeformdir2d".
const char* blendKindName(BlendKind kind);
BlendKind blendKindFromName(const std::string& name);

// Unity-style timing options for one transition. The defaults keep the original behaviour.
struct AnimTransitionOptions {
    // Exit time: the transition may fire only when the source clip reaches this normalized time.
    // Below 1 it fires once per loop at that fraction. At 1 or more it fires once, after that many loops.
    bool hasExitTime = false;
    float exitTime = 0.9f;
    // True: the crossfade is in seconds. False: it is a fraction of the source clip's length.
    bool fixedDuration = true;
    // Fraction of the target clip where playback starts when the transition fires.
    float offset = 0.0f;
    // Allows a transition from a state to itself. Off by default, which matches the original behaviour.
    bool toSelf = false;
};

class AnimStateMachine {
public:
    // Called when a state is entered. fadeSeconds is the crossfade; startFraction is where in the
    // target clip playback begins (0 = start, 0.5 = halfway).
    using PlayFn = std::function<void(const std::string& clip, float fadeSeconds, float startFraction)>;
    // Called with the clips of a blend state and their weights. fadeSeconds >= 0 means the state was just
    // entered and the blend should crossfade in. A negative value means the same state is still playing
    // and only the weights changed.
    using BlendFn = std::function<void(const std::vector<std::string>& clips, const std::vector<float>& weights,
                                       const std::vector<MotionOptions>& options, float fadeSeconds)>;

    struct Condition {
        enum class Op { Greater, Less, Equal, IsTrue, IsFalse, Trigger };
        std::string param;
        Op op = Op::Greater;
        float value = 0.0f;
    };

    using TransitionOptions = AnimTransitionOptions;

    void addState(const std::string& name, const std::string& clip);
    // A blend-tree state. paramX and paramY name float parameters (paramY is unused for 1D). Its duration is
    // the mean of its motions' durations, which sets exit times and fractional crossfades.
    void addBlendState(const std::string& name, BlendKind kind, const std::string& paramX, const std::string& paramY,
                       std::vector<BlendMotion> motions);
    // Transitions are checked in the order they were added. from "*" means any state.
    void addTransition(const std::string& from, const std::string& to, std::vector<Condition> when,
                       float fadeSeconds = 0.25f, float minDwellSeconds = 0.0f, TransitionOptions opts = {});
    void setDefaultState(const std::string& name) { default_ = name; }
    // Length of a clip in seconds, used for exit times and fractional crossfades. Unknown clips count as 1 s.
    void setClipDuration(const std::string& clip, float seconds) { clipDur_[clip] = seconds; }

    void setFloat(const std::string& name, float v) { floats_[name] = v; }
    void setBool(const std::string& name, bool v) { bools_[name] = v; }
    void setTrigger(const std::string& name) { triggers_[name] = true; }

    // Evaluates transitions. The first match wins, and at most one transition fires per call.
    // While a blend state is current, its weights are re-evaluated from the parameters every call.
    void update(float dt, const PlayFn& play, const BlendFn& blend = {});

    const std::string& current() const { return current_; }
    int transitions() const { return transitions_; }

private:
    struct State {
        std::string clip;
        BlendKind kind = BlendKind::None;
        std::string paramX, paramY;
        std::vector<BlendMotion> motions;
    };
    struct Transition {
        std::string from, to;
        std::vector<Condition> when;
        float fade = 0.25f;
        float minDwell = 0.0f;
        TransitionOptions opts;
    };
    bool holds(const Condition& c) const;
    bool allHold(const Transition& t) const;
    float durationOf(const std::string& clip) const;
    float stateDuration(const State& s) const;
    float currentDuration() const;
    bool exitTimeReached(const Transition& t, float prevDwell, float dwell, float clipLen) const;
    void enter(const std::string& name, float fade, float startFraction, const PlayFn& play, const BlendFn& blend);
    void applyBlend(const State& s, float fade, const BlendFn& blend) const;
    float floatOr(const std::string& name, float fallback) const;

    std::map<std::string, State> states_;
    std::vector<Transition> transitions_list_;
    std::map<std::string, float> clipDur_;
    std::string default_;
    std::string current_;
    float dwell_ = 0.0f;
    float prevDwell_ = 0.0f;
    bool started_ = false;
    int transitions_ = 0;
    std::map<std::string, float> floats_;
    std::map<std::string, bool> bools_;
    std::map<std::string, bool> triggers_;
};

} // namespace ukiyo
