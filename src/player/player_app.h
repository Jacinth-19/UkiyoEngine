// The player's runtime, without a window.
//
// Everything a shipped game does that is not drawing lives here: reading the manifest, loading the start scene, building
// the World (physics, navigation, audio, scripts, particles), advancing it, applying entity positions back to the scene
// transforms, switching scenes when a script asks, and describing what the HUD should say. Keeping it free of GLFW and
// the RHI means the player's logic is covered by a test that runs on CI without a GPU, while main.cpp stays a thin
// presentation layer.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/build_manifest.h"
#include "engine/scene_runtime.h"
#include "engine/transform.h"
#include "player/hud_layout.h"
#include "systems/save_game.h"
#include "systems/world.h"

namespace ukiyo {

struct PlayerInstance {
    std::string name;
    std::string modelPath;      // absolute; empty for an empty object
    Transform local;            // authored local transform
    int parent = -1;
    SystemsComponents components;
    glm::mat4 world{1.0f};      // resolved world matrix, updated every tick
    glm::vec3 position{0.0f};   // entity position after physics/navigation
};

class PlayerApp {
public:
    // `audio` opens the sound device; a headless run (CI, a screenshot) passes false so it never touches one.
    explicit PlayerApp(bool audio = true);
    ~PlayerApp();

    // Reads the manifest, then loads the first scene and starts the world. No GPU is touched: models are the renderer's
    // job, and `instances()` tells it what to load and where to put it.
    bool start(const std::string& buildPath, const std::string& projectRoot, std::string* error);
    void stop();

    void requestQuit() { quit_ = true; }
    bool running() const { return started_ && !quit_; }

    // Key state in Qt's portable key text ("W", "Space", "Left"), which is what the input asset and scripts speak.
    void setKey(const std::string& key, bool down);

    // One frame. dt is seconds and is clamped by the caller.
    void tick(float dt);

    // Saves the running game: every instance's position and yaw, the scene path and the play time, in the engine's
    // checksummed UKSV format. A save taken in one scene only restores that scene's instances; loading a save whose
    // scene differs is refused rather than applied to the wrong geometry.
    bool saveGame(const std::string& path, std::string* error) const;
    bool loadGame(const std::string& path, std::string* error);

    const BuildManifest& manifest() const { return manifest_; }
    const RuntimeScene& scene() const { return scene_; }
    const std::vector<PlayerInstance>& instances() const { return instances_; }
    World* world() { return world_.get(); }
    const World* world() const { return world_.get(); }

    // Bumps when the scene is replaced (start, or a script's loadScene). The renderer reloads models on a change.
    uint32_t sceneGeneration() const { return generation_; }
    double playTime() const { return playTime_; }
    int scriptsRunning() const;

    // Puts a one-line message under the title until the next one (a save result, a failed load). Empty clears it.
    void notify(const std::string& message);
    const std::string& notification() const { return notification_; }

    // Smoothed frame rate for the HUD, and the last frame time in milliseconds.
    void reportFrame(float dt);
    float fps() const { return fps_; }
    float frameMs() const { return frameMs_; }

    // One line describing the build, for the window title and the HUD.
    std::string title() const;
    // HUD strings, so the layout can be tested without a window. The layout itself is drawPlayerHud (hud_layout.h).
    using HudText = PlayerHudText;
    HudText hudText(uint32_t drawCalls, uint32_t triangles) const;

private:
    bool loadScene(const std::string& relativePath, std::string* error);
    void buildWorld();
    void driveCharacter(float dt);
    void syncInstancesFromWorld();

    BuildManifest manifest_;
    std::string projectRoot_;
    RuntimeScene scene_;
    std::vector<PlayerInstance> instances_;
    std::unique_ptr<World> world_;
    std::vector<glm::vec3> lastEntityPos_;
    std::vector<float> speed_;
    uint32_t generation_ = 0;
    double playTime_ = 0.0;
    float fps_ = 0.0f;
    float frameMs_ = 0.0f;
    bool audio_ = true;
    bool started_ = false;
    bool quit_ = false;
    std::vector<std::string> warnings_;
    std::string notification_;
    int characterIndex_ = -1;          // the instance the player controls, or -1
    std::set<std::string> keys_;       // keys held this frame, in Qt's portable text
};

// Maps a GLFW key (its name as printed by glfwGetKeyName, plus the special keys) to Qt's portable key text, which is
// what the input asset and scripts use. Returns an empty string for keys the engine has no name for.
std::string glfwKeyNameToQt(int glfwKey, const char* printableName);

} // namespace ukiyo
