// Packaging, as a command.
//
// The editor used to say "run ukiyo_player --project ." and stop. `ukiyo_pack` is the program that turns a project into
// something a player runs; this is the one place that knows how to call it, so the editor's Build page and the tests
// build the same argument list and a change to the tool is made once.
#pragma once

#include <string>
#include <vector>

namespace ukiyo::pack {

enum class Target { Linux, Windows, Android };

const char* targetName(Target t);   // "linux", "windows", "android": the --platform value

struct PackRequest {
    std::string project;                 // project root, relative to the working directory
    std::string player;                  // the player binary to ship
    std::string shaders;                 // the compiled shader directory to copy
    std::string outDir;                  // the package folder to create
    std::string zip;                     // the archive to write; empty for none
    Target target = Target::Linux;
    std::string root = ".";              // the repository root, for the paths a package keeps
    bool verify = true;                  // rehash the package after writing it
};

// The arguments for `ukiyo_pack`, without the program name. Order matters only in that each flag precedes its value.
std::vector<std::string> packArgs(const PackRequest& request);

// Sets "version" in a Build/build.json, leaving everything else in the file alone. Returns false with a reason when the
// file is not a build manifest. This is what a version field on the Build page edits.
bool setManifestVersion(const std::string& buildJsonPath, const std::string& version, std::string* error);
std::string manifestVersion(const std::string& buildJsonPath);

// Where a packaged game ends up for a project and a target: `<out>/<project name>-<target>`.
std::string defaultOutDir(const std::string& projectRoot, Target target);

} // namespace ukiyo::pack
