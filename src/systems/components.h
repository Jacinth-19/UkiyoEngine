// Per-instance settings for the game systems. The editor stores these in the scene file and
// World reads them in play mode. Plain data, no Qt, no engine objects.
#pragma once

#include <string>

#include <glm/glm.hpp>

#include "systems/anim_state.h"
#include "systems/vfx.h"

namespace ukiyo {

// Rigid body. A static body only blocks; a dynamic one falls and is pushed by the physics world.
struct BodyComponent {
    bool enabled = false;
    bool dynamic = true;
    bool box = false;              // a box collider instead of a sphere
    float radius = 0.5f;           // metres, sphere collider
    glm::vec3 halfExtents{0.5f};   // metres, box collider
    float mass = 1.0f;             // kilograms, dynamic only
};

// Navmesh agent. Moves toward targets set by scripts (ukiyo.moveTo) and avoids other agents.
struct AgentComponent {
    bool enabled = false;
    float maxSpeed = 3.5f;   // metres per second
};

// Positional sound. The sound plays from the instance's position while the scene plays.
struct AudioComponent {
    bool enabled = false;
    std::string sound;       // path to a WAV file, project-relative in the scene file
    bool loop = true;
    bool playOnStart = true;
    float volume = 1.0f;     // 0 to 1, this sound's place in the mix
};

// A parameter of a custom state machine. Floats and bools are set by the game; a trigger fires once.
struct AnimParamDef {
    enum class Type : uint8_t { Float, Bool, Trigger };
    std::string name;
    Type type = Type::Float;
    float value = 0.0f;      // starting value for floats and bools
};

// A state of a custom state machine: a name and the library clip it plays (empty = the idle loop).
struct AnimStateDef {
    std::string name;
    std::string clip;
    float x = 0.0f, y = 0.0f;   // position on the editor's graph
    // A blend tree instead of a single clip. When blend is not None, `clip` is unused and the motions
    // are mixed by the float parameters paramX and paramY (paramY is unused for 1D).
    BlendKind blend = BlendKind::None;
    std::string paramX, paramY;
    std::vector<BlendMotion> motions;
};

// A transition between two states. from "*" means any state. Transitions are checked in order.
struct AnimTransitionDef {
    std::string from;
    std::string to;
    std::vector<AnimStateMachine::Condition> when;
    float fade = 0.25f;
    float minDwell = 0.0f;
    AnimStateMachine::TransitionOptions opts;   // exit time, offset, fixed duration, self transitions
};

// Locomotion. By default it is Idle, Walk, Run, chosen by the agent speed (or by a "speed" parameter).
// When `custom` is set, the states, transitions and parameters below replace the three clips.
struct AnimComponent {
    bool enabled = false;
    std::string idleClip;    // empty = the idle loop
    std::string walkClip;
    std::string runClip;
    float walkSpeed = 0.1f;  // metres per second above which Walk starts
    float runSpeed = 3.0f;   // metres per second above which Run starts

    bool custom = false;
    std::string defaultState;
    std::vector<AnimParamDef> params;
    std::vector<AnimStateDef> states;
    std::vector<AnimTransitionDef> transitions;
};

// A Lua script run each frame. It calls ukiyo.* functions and sees this instance as "instance<N>".
struct ScriptComponent {
    bool enabled = false;
    std::string path;        // project-relative .lua path
};

// A particle emitter at the instance's position.
struct EmitterComponent {
    bool enabled = false;
    EmitterSettings settings;
};

// Spin or ping-pong motion for an instance without a body. Spin turns the yaw at `speed` degrees per second.
// Ping-pong moves the instance along `axis` from its rest position to `amplitude` metres away and back, at
// `speed` cycles per second. `ease` makes the move smooth at both ends instead of straight.
struct ActionComponent {
    bool enabled = false;
    bool pingPong = false;
    float speed = 90.0f;
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float amplitude = 1.0f;
    bool ease = false;
    float startOffset = 0.0f;   // seconds the entity stays at rest before the action starts
};

// A capsule character controller (item 4): the instance becomes a walkable player character. It is an alternative to
// BodyComponent, not an addition — a character is kinematic and does not fall under Jolt's rigid body rules.
struct CharacterComponent {
    bool enabled = false;
    float radius = 0.3f;
    float height = 1.8f;
    float maxSlopeDeg = 50.0f;
    float stepHeight = 0.4f;
    float jumpSpeed = 5.0f;
    float speed = 4.0f;        // metres per second at full stick
};

struct SystemsComponents {
    BodyComponent body;
    CharacterComponent character;
    AgentComponent agent;
    AudioComponent audio;
    AnimComponent anim;
    ScriptComponent script;
    EmitterComponent emitter;
    ActionComponent action;

    bool any() const {
        return body.enabled || character.enabled || agent.enabled || audio.enabled || anim.enabled || script.enabled ||
               emitter.enabled || action.enabled;
    }
};

} // namespace ukiyo
