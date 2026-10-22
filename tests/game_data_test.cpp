// The half of Lantern Run's check that has to pass on a phone.
//
// game_test loads the same project on a desktop and adds the parts a desktop build can afford: glTF parsing and
// generated-asset arithmetic. This one is built from only the translation units the Android job compiles for arm64-v8a
// and x86_64 — the manifest reader, the scene reader, the transform and light maths, nothing with a GPU or a window in
// it. Running it on Linux and Windows is the behaviour check; building it for the two phone ABIs is the other half of
// the claim, because a game whose scene reader does not compile for a phone is a game that cannot exist there, whatever
// the renderer could do with it.
//
//   game_data_test [project directory]        (default: games/lantern-run)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "engine/build_manifest.h"
#include "engine/scene_runtime.h"

namespace {

namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    const std::string project = argc > 1 ? argv[1] : "games/lantern-run";
    std::printf("game_data_test: %s\n", project.c_str());

    ukiyo::BuildManifest manifest;
    std::string error;
    check(ukiyo::loadBuildManifest(project + "/Build/build.json", &manifest, &error), "the build manifest loads", error);
    check(manifest.name == "Lantern Run", "the build is called Lantern Run", manifest.name);
    check(!ukiyo::firstScene(manifest).empty(), "it names a start scene", ukiyo::firstScene(manifest));

    ukiyo::RuntimeScene scene;
    check(ukiyo::loadRuntimeScene(project + "/" + ukiyo::firstScene(manifest), project, &scene, &error),
          "the start scene loads", error);
    check(scene.warnings.empty(), "it loads without warnings",
          scene.warnings.empty() ? std::string() : scene.warnings.front());
    check(scene.instances.size() == 16, "sixteen instances", std::to_string(scene.instances.size()));
    check(scene.lights.size() == 4, "four lights", std::to_string(scene.lights.size()));

    int hero = -1, lanterns = 0, scripts = 0;
    bool pathsExist = true;
    std::string missing, scriptPath;
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const ukiyo::RuntimeInstance& in = scene.instances[i];
        if (in.name == "Hero") hero = static_cast<int>(i);
        if (in.name.rfind("Lantern ", 0) == 0) ++lanterns;
        if (in.components.script.enabled) {
            ++scripts;
            scriptPath = project + "/" + in.components.script.path;
        }
        if (!in.empty && !in.path.empty() && !fs::exists(in.path)) {
            pathsExist = false;
            if (missing.empty()) missing = in.name + " -> " + in.path;
        }
    }
    check(hero >= 0 && scene.instances[static_cast<size_t>(hero)].components.character.enabled,
          "the hero is the character the phone would drive");
    check(lanterns == 5, "five lanterns to gather", std::to_string(lanterns));
    check(scripts == 1, "one script", std::to_string(scripts));
    check(pathsExist, "every model the scene names is on disk", missing);
    check(!scriptPath.empty() && fs::exists(scriptPath), "the script is where the scene says it is", scriptPath);

    // The parts of the game a phone needs to agree with the desktop about: the look, and the fact that the script has
    // an update hook at all (a syntax error is a silent game, and this is the cheapest place to notice one).
    check(scene.settings.shadows && scene.settings.sky, "shadows and a sky are asked for");
    const std::string script = readText(scriptPath);
    check(script.find("function onUpdate") != std::string::npos && script.find("ukiyo.hud(") != std::string::npos,
          "the script updates and writes the HUD");

    std::printf("game_data_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
