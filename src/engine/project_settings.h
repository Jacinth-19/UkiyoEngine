#pragma once
// Project-wide settings: input actions, tags and layers, time, physics, and the build list.
// Stored as ProjectSettings/ProjectSettings.json in the project root. A missing file means the
// defaults. Values are clamped and cleaned up by normalize() on every load and save.
#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace engine {

struct InputAction {
    std::string name;   // e.g. "Jump"
    // What the action is bound to: a control path naming its device ("<Keyboard>/space", "<Gamepad>/buttonSouth",
    // "<Mouse>/leftButton", "<Touchscreen>/primaryTouch/press"), or a bare portable key ("Space", "W", "Left") for a
    // project file written before paths. The editor writes paths; a bare key is the keyboard's path and is still
    // read. Touch positions on a binding are screen fractions (0..1, origin top-left), not Unity's pixels.
    std::string key;
};

struct ProjectSettings {
    // Tags are added by the user. Built-in tags are always available and are not stored.
    std::vector<std::string> tags;
    // 32 layer slots. Slots 0-7 have built-in names (Default, TransparentFX, ...) and cannot be
    // renamed. Slots 8-31 are user layers. An empty name means the slot is unused.
    std::array<std::string, 32> layers;

    // Time. The fixed step drives physics. The time scale multiplies the game's dt, so 0 pauses.
    float fixedTimestep = 1.0f / 60.0f;
    float timeScale = 1.0f;

    // Physics. Gravity is in m/s^2. The solver counts are Jolt's velocity and position steps.
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    int velocitySolverSteps = 10;
    int positionSolverSteps = 2;

    // Input actions. A script asks for an action, and the action resolves to a key.
    std::vector<InputAction> inputActions;

    // Build. Scenes are project-relative .ukscene paths, in build order. These fields are exactly what the player's
    // manifest needs, so the Build page can write Build/build.json without asking anything twice.
    std::string productName = "Ukiyo Game";
    std::vector<std::string> buildScenes;
    std::string buildStartScene;      // project-relative; empty means the first scene
    std::string buildFont;            // project-relative .ttf for the HUD; empty means no text
    int buildWidth = 1280;
    int buildHeight = 720;
    bool buildFullscreen = false;
};

// The built-in names for the first eight layer slots and the tags that always exist.
const std::array<const char*, 8>& builtinLayerNames();
const std::vector<std::string>& builtinTags();

// Clamps numeric values, trims and drops empty or duplicate names, and fills the built-in layers.
void normalize(ProjectSettings& s);

// The file that stores the settings for a project root.
std::string projectSettingsPath(const std::string& projectRoot);

// A missing file loads as the defaults and succeeds. A malformed file fails with `error`.
bool loadProjectSettings(const std::string& path, ProjectSettings& out, std::string& error);
bool saveProjectSettings(const std::string& path, const ProjectSettings& in, std::string& error);

} // namespace engine
