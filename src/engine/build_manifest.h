// Build manifest: what the editor's Build page writes and the player reads.
//
// Build/build.json has existed since the Build page landed, but nothing consumed it: the page wrote a list of scenes and
// said packaging an executable was not included. The player consumes it now. The manifest is deliberately small and
// stable: product name, the ordered scene list with a start scene, the window, and which font the HUD uses. Anything a
// game needs beyond that belongs in its own project settings, not here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ukiyo {

struct BuildWindow {
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool vsync = true;
};

struct BuildManifest {
    static constexpr int kVersion = 1;

    std::string name = "Ukiyo game";      // window title and HUD heading
    std::string startScene;               // project-relative path; empty means the first scene
    std::vector<std::string> scenes;      // project-relative .ukscene paths, in load order
    BuildWindow window;
    std::string font;                     // project-relative .ttf; empty means "no HUD text"
    float hudScale = 1.0f;                // multiplies the HUD's pixel sizes for high-DPI windows
    std::string version = "1.0.0";
};

// Parses a manifest from JSON bytes. Returns false with a reason in `error` when the document is not a build manifest
// or a required field is missing. Unknown fields are ignored so a newer editor can write more without breaking an older
// player.
bool parseBuildManifest(const std::string& json, BuildManifest* out, std::string* error);

// Reads and parses Build/build.json. `error` receives the path and the reason when it fails.
bool loadBuildManifest(const std::string& path, BuildManifest* out, std::string* error);

// The scene the player should open first: startScene when set, else the first entry. Empty when there are none.
std::string firstScene(const BuildManifest& m);

// Shows a manifest as JSON, which is what the Build page writes. Used by tests and by the editor once it writes the
// full manifest rather than only the scene list.
std::string buildManifestJson(const BuildManifest& m);

} // namespace ukiyo
