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

#include "engine/lights.h"
#include "engine/localisation.h"
#include "engine/project_settings.h"
#include "systems/save_game.h"
#include "input/input.h"
#include "input/input_users.h"
#include "systems/anim_state.h"
#include "systems/audio.h"
#include "systems/navigation.h"
#include "systems/physics.h"
#include "systems/script_runtime.h"
#include "systems/vfx.h"

namespace ukiyo {

// Where a frame's time went. One frame's worth, in milliseconds, measured by the world itself — the editor's profiler
// reads it, and a script can ask for it (`ukiyo.frameTiming`).
//
// This is CPU time. It says what the game's own update costs — physics, navigation, scripts, animation, particles — and
// deliberately not what the GPU took, which the RHI does not expose (docs/PROFILER.md).
struct FrameTiming {
    float input = 0.0f;
    float scripts = 0.0f;
    float physics = 0.0f;
    float characters = 0.0f;
    float navigation = 0.0f;
    float animation = 0.0f;
    float particles = 0.0f;
    float entities = 0.0f;
    float total = 0.0f;
    // The share of `total` each stage took, 0..1. An editor's bar chart reads these; a test reads them to prove a
    // breakdown is a breakdown and not five copies of the total.
    float share(const char* stage) const;
};

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
    // The full rotation, x = pitch, y = yaw, z = roll, in the engine's ZXY order. It used to be a yaw alone, which
    // was enough for a character on flat ground and nothing else: a plank tilted in the editor arrived at run time
    // standing up straight, and a box that physics tumbled was drawn the way it was authored.
    glm::vec3 rotationDeg{0.0f};
    PhysicsWorld::Handle body = PhysicsWorld::kNone;   // driven by physics when set
    int agent = -1;                                    // driven by the navmesh crowd when >= 0
    CharacterHandle character = 0;                     // driven by the capsule controller when set
    glm::vec3 characterVelocity{0.0f};                 // planar metres per second, set by the game each frame
    bool characterJump = false;                        // one-shot: a jump was requested this frame
    bool grounded = true;                              // the character's ground state after the last move
    std::unique_ptr<AnimStateMachine> anim;
    std::string clip;
    EntityAction action;                               // spin or ping-pong, when on                                  // last clip the state machine asked for
};

// One bound function: the name a script sees under ukiyo, and the C function behind it.
struct ScriptApiBinding {
    const char* name;
    lua_CFunction fn;
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
    void attachDynamicBox(const std::string& name, const glm::vec3& halfExtents, float mass,
                          const glm::vec3& rotationDeg = glm::vec3(0.0f));
    // A navmesh agent drives the entity once a navmesh is built.
    void attachAgent(const std::string& name, float maxSpeed = 3.5f);
    // A capsule character drives the entity (item 4). The capsule stands on the entity's current position.
    bool attachCharacter(const std::string& name, const CharacterSettings& settings = CharacterSettings());
    // The game's input for this frame: a planar velocity in metres per second and an optional jump.
    void setCharacterVelocity(const std::string& name, const glm::vec3& planarVelocity, bool jump = false);
    bool characterState(const std::string& name, CharacterState* out) const;
    void setEntityPosition(const std::string& name, const glm::vec3& p);
    void setParam(const std::string& name, const std::string& param, float value);
    // Starts a spin or ping-pong on an entity. `rest` and `restYaw` are where it starts from.
    void setAction(const std::string& name, const EntityAction& a, const glm::vec3& rest, float restYaw);

    // The ukiyo.* functions this world binds, in registration order. The stub file the editor generates is checked
    // against it, so a function cannot be bound without being documented for the language server.
    static const std::vector<ScriptApiBinding>& apiBindings();

    // Adds the script for one entity ("" for a scene-wide script) and runs its onStart. Every instance that has a
    // script gets one, in the order the scene lists them.
    bool loadScriptFor(const std::string& entity, const std::string& path, std::string* error);
    bool loadScript(const std::string& path, std::string* error);
    // The host of the scene-level script (the one with no entity), created empty when there is none. A world with its own
    // logic and no object to hang it on, and tests that drive one script by hand, use this.
    ScriptHost& sceneScript();
    // Folders require() may read from: the project's script folders, usually.
    void setScriptSearchPaths(std::vector<std::string> roots) { scripts.setSearchPaths(std::move(roots)); }
    // Loads a sound by file and caches it. Returns 0 when the file cannot be read.
    AudioEngine::SoundId soundFor(const std::string& path);

    // The scene's lights while the world runs. The caller (player or editor) keeps pushing this copy to the
    // renderer each frame, so ukiyo.setLightEnabled in a script is what the next frame shades with.
    void setSceneLights(std::vector<SceneLight> lights) { sceneLights_ = std::move(lights); }
    const std::vector<SceneLight>& sceneLights() const { return sceneLights_; }
    SceneLight* findLight(const std::string& name);

    // Game progress: a flat SaveGame store the script fills (ukiyo.saveSet) and persists per slot
    // (ukiyo.saveWrite -> <saveDir>/slot<n>.uksv). The directory is set by whoever owns the project — the player
    // points it at the project's saves folder, tests point it at a temp directory.
    void setSaveDir(std::string dir) { saveDir_ = std::move(dir); }
    const std::string& saveDir() const { return saveDir_; }
    SaveGame& saveStore() { return saveStore_; }

    // Script HUD quads for the frame that is being built: ukiyo.hudRect during this tick, drawn after
    // drawPlayerHud, cleared at the start of the next tick. Pixels, origin top-left, colours 0..1.
    struct HudQuad {
        float x = 0, y = 0, w = 0, h = 0;
        float r = 1, g = 1, b = 1, a = 1;
    };
    const std::vector<HudQuad>& hudQuads() const { return hudQuads_; }
    void addHudQuad(const HudQuad& q) { hudQuads_.push_back(q); }
    // The project root the world runs inside. A media path from a script ("Sound/beep.wav") resolves against it
    // first and falls back to the path as written, so the same script works in the editor, in a packaged game whose
    // layout is the checkout's, and on Android where the staged tree is the working directory.
    void setProjectRoot(std::string root) { projectRoot_ = std::move(root); }
    std::string resolveMediaPath(const std::string& path) const;

    // One frame: script, physics, navigation, entity sync, animation, particles, listener.
    void tick(float dt);

    // The last tick's breakdown. Zero until the first tick.
    const FrameTiming& lastTiming() const { return timing_; }
    // Turns the clock on. Off by default: a shipped game should not pay for std::chrono four times a frame, and a
    // profiler that is always on is a profiler nobody trusts.
    void setProfiling(bool on) { profiling_ = on; }
    bool profiling() const { return profiling_; }
    double time() const { return time_; }

    // Project time scale. tick() multiplies dt by it, so 0 freezes scripts, physics, and animation.
    void setTimeScale(float scale) { timeScale_ = scale < 0.0f ? 0.0f : scale; }
    float timeScale() const { return timeScale_; }

    // Input. The project's actions (a name and a portable key, e.g. "Jump" and "Space") are compiled into a real
    // action map in the engine's input system, so a script asking actionDown() gets the same phases, bindings and
    // rebinding a keyboard or gamepad would. The map is named "Gameplay" and is enabled while the world exists.
    void setInputActions(const std::vector<engine::InputAction>& actions);
    // The editor reports key presses and releases for the game view, in Qt's portable key text.
    void setKeyHeld(const std::string& key, bool down);
    // A finger on glass, in screen fractions (0..1, origin top-left): finger 0 is the primary touch and 1 the
    // secondary. Writes <Touchscreen> controls in the same State, so an action bound to
    // "<Touchscreen>/primaryTouch/press" fires from a real touch event on Android and from a test on any machine.
    void setTouch(int finger, bool down, const glm::vec2& position, const glm::vec2& delta);
    bool actionDown(const std::string& action) const;
    // A game's own line in the player's HUD, set by a script (`ukiyo.hud`). The player reads it after every tick and
    // shows it under the title, which is how a game says "3 of 5 lanterns" without owning the HUD renderer. Empty
    // means "nothing to say": the player then keeps its own message (a save result, a warning) on screen.
    void setHudText(const std::string& text) { hudText_ = text; }
    // The game's language (src/engine/localisation.h). A world with no catalogue returns empty for every key, which is
    // what makes ukiyo.tr fall back to its second argument or the key.
    void setLocale(LocaleCatalog catalog) { locale_ = std::move(catalog); }
    std::string translate(const std::string& key) const {
        auto it = locale_.strings.find(key);
        return it == locale_.strings.end() ? std::string() : it->second;
    }
    const std::string& localeCode() const { return locale_.code; }
    const std::string& hudText() const { return hudText_; }
    // The live input state, for a game that wants more than actionDown(): values, vectors, phases and rebinding.
    ukiyo::input::State& input() { return input_; }
    const ukiyo::input::State& input() const { return input_; }

    // Multi-player (Unity's PlayerInput / InputUser, § input_users.h). Created on first use, with this world's input
    // state as the master, so the devices and the "Gameplay" map are shared and only the pairing is per player. The
    // world updates the master itself in tick(), so the manager is told not to update it again.
    ukiyo::input::InputSystem& inputPlayers();
    bool hasInputPlayers() const { return players_ != nullptr; }

    PhysicsWorld physics;
    NavMesh nav;
    AudioEngine audio;
    VfxSystem vfx;
    ScriptRuntime scripts;

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
    // Runs every script's onSceneLoaded(). The editor calls this once the scene has started.
    void notifySceneLoaded() { scripts.callSceneLoaded(); }

    // Messages from scripts (ukiyo.log).
    std::vector<std::string> logs;

private:
    void syncEntities();
    void updateCharacters(float dt);
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
    ukiyo::input::State input_;
    std::unique_ptr<ukiyo::input::InputSystem> players_;
    std::map<std::string, Entity> entities_;   // std::map keeps the update order deterministic
    std::map<std::string, AudioEngine::SoundId> sounds_;
    std::string projectRoot_;
    std::vector<SceneLight> sceneLights_;
    SaveGame saveStore_;
    std::string saveDir_;
    std::vector<HudQuad> hudQuads_;
    std::string hudText_;
    LocaleCatalog locale_;
    FrameTiming timing_;
    bool profiling_ = false;
    double time_ = 0.0;
};

} // namespace ukiyo
