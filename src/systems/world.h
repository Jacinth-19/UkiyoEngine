// The systems container. A World owns physics, navigation, audio, particles, and scripts, and it
// keeps the named entities that scripts address. tick() runs the systems in a fixed order.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/project_settings.h"
#include "systems/anim_state.h"
#include "systems/audio.h"
#include "systems/navigation.h"
#include "systems/physics.h"
#include "systems/script.h"
#include "systems/vfx.h"

namespace ukiyo {

// Runtime spin or ping-pong, set from ActionComponent when play starts. See ActionComponent.
struct EntityAction {
    bool on = false;
    bool pingPong = false;
    float speed = 90.0f;
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float amplitude = 1.0f;
    bool ease = false;
    glm::vec3 rest{0.0f};          // position the ping-pong swings around
    float restYaw = 0.0f;          // yaw the spin turns from
    float startOffset = 0.0f;      // seconds at rest before the action starts
    float t = 0.0f;                // seconds since the action started; negative while the start offset runs
};

// Share of the way to the far end at phase `phase` (in cycles), 0 at rest and 1 at the far end.
// Linear is a triangle wave. Ease is a cosine, so it slows at both ends.
float pingPongWeight(float phase, bool ease);

struct Entity {
    std::string name;
    glm::vec3 position{0.0f};
    float yawDeg = 0.0f;
    PhysicsWorld::Handle body = PhysicsWorld::kNone;   // driven by physics when set
    int agent = -1;                                    // driven by the navmesh crowd when >= 0
    std::unique_ptr<AnimStateMachine> anim;
    std::string clip;
    EntityAction action;                               // spin or ping-pong, when on                                  // last clip the state machine asked for
};

class World {
public:
    explicit World(bool audioNoDevice = true);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Entities are created on first use.
    Entity& entity(const std::string& name);
    Entity* findEntity(const std::string& name);
    // Physics drives the entity from now on. Its position is copied from the body each tick.
    void attachDynamicSphere(const std::string& name, float radius, float mass);
    void attachDynamicBox(const std::string& name, const glm::vec3& halfExtents, float mass);
    // A navmesh agent drives the entity once a navmesh is built.
    void attachAgent(const std::string& name, float maxSpeed = 3.5f);
    void setEntityPosition(const std::string& name, const glm::vec3& p);
    void setParam(const std::string& name, const std::string& param, float value);
    // Starts a spin or ping-pong on an entity. `rest` and `restYaw` are where it starts from.
    void setAction(const std::string& name, const EntityAction& a, const glm::vec3& rest, float restYaw);

    bool loadScript(const std::string& path, std::string* error);
    // Loads a sound by file and caches it. Returns 0 when the file cannot be read.
    AudioEngine::SoundId soundFor(const std::string& path);

    // One frame: script, physics, navigation, entity sync, animation, particles, listener.
    void tick(float dt);
    double time() const { return time_; }

    // Project time scale. tick() multiplies dt by it, so 0 freezes scripts, physics, and animation.
    void setTimeScale(float scale) { timeScale_ = scale < 0.0f ? 0.0f : scale; }
    float timeScale() const { return timeScale_; }

    // Input. Actions map to a key (portable key text, e.g. "Space"). Scripts ask with actionDown().
    void setInputActions(const std::vector<engine::InputAction>& actions) { actions_ = actions; }
    // The editor reports key presses and releases for the game view.
    void setKeyHeld(const std::string& key, bool down);
    bool actionDown(const std::string& action) const;

    PhysicsWorld physics;
    NavMesh nav;
    AudioEngine audio;
    VfxSystem vfx;
    ScriptHost script;

    // Called when an entity's state machine changes clip. Defaults to storing the clip on the entity.
    std::function<void(const std::string& entity, const std::string& clip, float fade, float startFraction)> onClipChange;
    // A blend-tree state changed its weights (fade < 0) or was entered (fade >= 0).
    std::function<void(const std::string& entity, const std::vector<std::string>& clips,
                       const std::vector<float>& weights, const std::vector<MotionOptions>& options, float fade)> onBlendChange;
    // Scene switching and loading holds. A script asks for a switch with requestScene(). The editor performs
    // it once no hold is open, so a script can wait (a fade, a load in progress) before the swap. Holds belong
    // to this World. A switch replaces the World, so it drops them. A second request before the switch
    // replaces the first.
    void requestScene(const std::string& path) { scenePath_ = path; sceneRequested_ = true; }
    bool sceneRequested() const { return sceneRequested_; }
    // True when a switch is waiting and no hold is open.
    bool sceneSwitchReady() const { return sceneRequested_ && loadHolds_.empty(); }
    // Returns the requested path and clears the request.
    std::string takeSceneRequest();
    // Opens a hold and returns its token. Holds stop a requested switch until released.
    int holdLoad();
    // Releases a hold. Returns false for an unknown or already released token.
    bool releaseLoad(int token);
    size_t loadHoldCount() const { return loadHolds_.size(); }
    // Asks the editor to load a model ahead of time, so a later switch to it is quicker. Taken with takePreloads().
    void requestPreload(const std::string& path) { preloads_.push_back(path); }
    std::vector<std::string> takePreloads();
    // Runs the script's onSceneLoaded(). The editor calls this once the scene has started.
    void notifySceneLoaded() { script.callSceneLoaded(); }

    // Messages from scripts (ukiyo.log).
    std::vector<std::string> logs;

private:
    void syncEntities();
    // Runs onContact(event) for each contact the last physics step raised. Body changes made there are deferred.
    void dispatchContacts();
    void updateAnimation(float dt);
    void updateListener();
    void updateActions(float dt);

    float timeScale_ = 1.0f;
    bool sceneRequested_ = false;
    std::string scenePath_;
    std::set<int> loadHolds_;
    int nextHold_ = 1;
    std::vector<std::string> preloads_;
    std::vector<engine::InputAction> actions_;
    std::vector<std::string> heldKeys_;
    std::map<std::string, Entity> entities_;   // std::map keeps the update order deterministic
    std::map<std::string, AudioEngine::SoundId> sounds_;
    double time_ = 0.0;
};

} // namespace ukiyo
