// The first game, as a test. Lantern Run is shipped in this repository as a project directory — a build manifest, one
// scene, one script and four generated GLB files — and everything about it can be checked without a device: does the
// manifest parse, does the scene load, does every path it names exist, does the script it hangs on "Night" name the
// lanterns the scene has, and are the GLB files the asset tool wrote actually glTF 2.0 with the primitives the loader
// reads. A game that cannot load is found here rather than by a player.
//
//   game_test [project directory]        (default: games/lantern-run, relative to the working directory)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/build_manifest.h"
#include "engine/scene_runtime.h"

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    std::ifstream f(path, std::ios::binary);
    if (!f) return bytes;
    f.seekg(0, std::ios::end);
    bytes.resize(static_cast<size_t>(std::max<std::streamoff>(0, f.tellg())));
    f.seekg(0, std::ios::beg);
    if (!bytes.empty()) f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

// The parts of a GLB this test needs: the two chunks, and the JSON they carry.
struct Glb {
    bool ok = false;
    json doc;
    size_t binBytes = 0;
    std::string why;
};

Glb parseGlb(const std::string& path) {
    Glb g;
    const std::vector<uint8_t> bytes = readFile(path);
    if (bytes.size() < 20) {
        g.why = "file is too small to be a GLB";
        return g;
    }
    if (u32(bytes, 0) != 0x46546C67u) {
        g.why = "not a glTF binary (bad magic)";
        return g;
    }
    if (u32(bytes, 4) != 2) {
        g.why = "glTF version is not 2";
        return g;
    }
    if (u32(bytes, 8) != bytes.size()) {
        g.why = "the header's length does not match the file size";
        return g;
    }
    size_t at = 12;
    bool haveJson = false;
    while (at + 8 <= bytes.size()) {
        const uint32_t length = u32(bytes, at);
        const uint32_t type = u32(bytes, at + 4);
        at += 8;
        if (at + length > bytes.size()) {
            g.why = "a chunk runs past the end of the file";
            return g;
        }
        if (type == 0x4E4F534Au) {   // JSON
            g.doc = json::parse(std::string(bytes.begin() + static_cast<long>(at), bytes.begin() + static_cast<long>(at + length)),
                                nullptr, false);
            haveJson = !g.doc.is_discarded();
            if (!haveJson) {
                g.why = "the JSON chunk does not parse";
                return g;
            }
        } else if (type == 0x004E4942u) {   // BIN
            g.binBytes = length;
        }
        at += length;
    }
    if (!haveJson) {
        g.why = "no JSON chunk";
        return g;
    }
    g.ok = true;
    return g;
}

} // namespace

int main(int argc, char** argv) {
    const std::string project = argc > 1 ? argv[1] : "games/lantern-run";
    std::printf("game_test: %s\n", project.c_str());

    // --- The build manifest -------------------------------------------------------------
    ukiyo::BuildManifest manifest;
    std::string error;
    const std::string manifestPath = project + "/Build/build.json";
    check(ukiyo::loadBuildManifest(manifestPath, &manifest, &error), "the build manifest loads", error);
    check(manifest.name == "Lantern Run", "the build is called Lantern Run", manifest.name);
    check(!manifest.version.empty(), "it carries a version", manifest.version);
    check(!manifest.scenes.empty() && ukiyo::firstScene(manifest) == manifest.startScene,
          "the start scene is the one the manifest names", ukiyo::firstScene(manifest));
    check(!manifest.font.empty() && fs::exists(project + "/" + manifest.font),
          "the HUD font the manifest names exists", manifest.font);

    // --- The scene ----------------------------------------------------------------------
    ukiyo::RuntimeScene scene;
    const std::string scenePath = project + "/" + ukiyo::firstScene(manifest);
    check(ukiyo::loadRuntimeScene(scenePath, project, &scene, &error), "the start scene loads", error);
    check(scene.warnings.empty(), "the scene loads without warnings",
          scene.warnings.empty() ? std::string() : scene.warnings.front());
    check(scene.instances.size() == 16, "the court has sixteen instances",
          std::to_string(scene.instances.size()));

    int hero = -1, lanterns = 0, stoneLanterns = 0, scripts = 0;
    bool everyPathExists = true;
    std::string missing;
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const ukiyo::RuntimeInstance& in = scene.instances[i];
        if (in.name == "Hero") hero = static_cast<int>(i);
        if (in.components.script.enabled) ++scripts;
        if (in.name.rfind("Lantern ", 0) == 0) ++lanterns;
        if (in.name.rfind("Stone Lantern", 0) == 0) ++stoneLanterns;
        if (!in.empty && !in.path.empty() && !fs::exists(in.path)) {
            everyPathExists = false;
            if (missing.empty()) missing = in.name + " -> " + in.path;
        }
    }
    check(everyPathExists, "every model path the scene names exists on disk", missing);
    check(hero >= 0 && scene.instances[static_cast<size_t>(hero)].components.character.enabled,
          "the hero is a character the player can drive");
    check(lanterns == 5, "there are five paper lanterns to gather", std::to_string(lanterns));
    check(stoneLanterns == 6, "six stone lanterns light the court", std::to_string(stoneLanterns));
    check(scripts == 1, "one script runs the game", std::to_string(scripts));

    // The script has to agree with the scene about what exists: a typo in a lantern's name is a lantern that can never
    // be gathered, and the scene would still load perfectly.
    std::string script;
    for (const ukiyo::RuntimeInstance& in : scene.instances) {
        if (!in.components.script.enabled) continue;
        const std::string path = project + "/" + in.components.script.path;
        const std::vector<uint8_t> bytes = readFile(path);
        script.assign(bytes.begin(), bytes.end());
        check(!script.empty(), "the game script is on disk", path);
    }
    bool namesEveryLantern = !script.empty();
    for (const ukiyo::RuntimeInstance& in : scene.instances) {
        if (in.name.rfind("Lantern ", 0) != 0) continue;
        if (script.find("\"" + in.name + "\"") == std::string::npos) namesEveryLantern = false;
    }
    check(namesEveryLantern, "the script names every lantern the scene places");
    check(script.find("ukiyo.hud(") != std::string::npos,
          "the script writes the HUD line the player shows");
    check(script.find("function onUpdate") != std::string::npos,
          "the script has an update hook to gather lanterns in");

    // --- The lighting and the look --------------------------------------------------------
    check(scene.lights.size() == 4 && scene.lights.front().type == ukiyo::LightType::Directional,
          "one moon and three lantern lights", std::to_string(scene.lights.size()));
    check(scene.settings.shadows && scene.settings.sky && scene.settings.bloom,
          "the court is lit, has a sky and glows");
    check(scene.settings.fogDensity > 0.0f, "the night has fog in it",
          std::to_string(scene.settings.fogDensity));

    // --- The generated assets -------------------------------------------------------------
    struct Asset {
        const char* file;
        size_t minTriangles;
    };
    const Asset assets[] = {{"platform.glb", 300}, {"torii.glb", 100}, {"stone_lantern.glb", 60}, {"paper_lantern.glb", 200}};
    for (const Asset& a : assets) {
        const std::string path = project + "/assets/" + a.file;
        const Glb glb = parseGlb(path);
        check(glb.ok, (std::string(a.file) + " is a glTF 2.0 binary").c_str(), glb.why);
        if (!glb.ok) continue;
        size_t triangles = 0;
        bool primitivesOk = true;
        int materials = 0;
        if (glb.doc.contains("materials") && glb.doc["materials"].is_array())
            materials = static_cast<int>(glb.doc["materials"].size());
        for (const json& mesh : glb.doc.value("meshes", json::array())) {
            for (const json& prim : mesh.value("primitives", json::array())) {
                if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION") ||
                    !prim["attributes"].contains("NORMAL") || !prim.contains("indices"))
                    primitivesOk = false;
                if (!prim.contains("indices")) continue;
                const size_t accessor = prim["indices"].get<size_t>();
                if (accessor >= glb.doc["accessors"].size()) continue;
                triangles += glb.doc["accessors"][accessor].value("count", size_t(0)) / 3;
            }
        }
        check(primitivesOk, (std::string(a.file) + " has positions, normals and indices").c_str());
        check(triangles >= a.minTriangles, (std::string(a.file) + " has the geometry it should").c_str(),
              std::to_string(triangles) + " triangles");
        check(glb.binBytes > 0, (std::string(a.file) + " carries its vertex data").c_str(),
              std::to_string(glb.binBytes) + " bytes");
        check(materials >= 1, (std::string(a.file) + " names its materials").c_str(), std::to_string(materials));
    }

    std::printf("game_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
