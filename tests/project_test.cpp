// Headless checks for the Ukiyo Hub's project templates: names, the manifest, the scaffold each
// template writes, the generated terrain heightmap, the progress steps the splash shows, and the
// ways a new project is refused. Needs no window, GPU, or Qt.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/project_settings.h"
#include "engine/project_template.h"
#include "engine/terrain.h"

#ifndef UKIYO_ROOT
#define UKIYO_ROOT "."
#endif

namespace fs = std::filesystem;
using nlohmann::json;

static int passed = 0;
static int failed = 0;

static void check(bool ok, const std::string& name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    std::fflush(stdout);
    ok ? ++passed : ++failed;
}

static std::string num(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.3f", value);
    return buffer;
}

static fs::path tempRoot() {
    static int counter = 0;
    // Beside the engine rather than in the system temporary directory: the templates link the engine's own assets, and
    // a project on another volume cannot be described by a relative path. Windows CI checks the engine out on D: and
    // keeps TEMP on C:, so a project in TEMP made every "the path is relative" check fail for a reason that has
    // nothing to do with the code under test. build/ is where a build already writes, and it is ignored by git.
    // "test-projects", not "project_test": on Linux the executable itself is build/project_test, so a directory of
    // that name cannot be created beside it.
    const fs::path base = fs::path(UKIYO_ROOT) / "build" / "test-projects";
    std::error_code ec;
    fs::create_directories(base, ec);
    return base / ("case" + std::to_string(++counter));
}

static json readJson(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return json();
    try {
        return json::parse(in);
    } catch (const std::exception&) {
        return json();
    }
}

static bool writeFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return false;
    out << text;
    return bool(out);
}

// ---- Names ----------------------------------------------------------------------------------

static void testNames() {
    check(engine::validProjectName("My Game"), "names: a normal name is accepted");
    check(engine::validProjectName("ukiyo-demo"), "names: dashes and letters are accepted");
    check(engine::validProjectName("v1.2 Prototype"), "names: dots, spaces and digits are accepted");
    check(engine::validProjectName("  padded  "), "names: surrounding spaces are trimmed");

    std::string why;
    check(!engine::validProjectName("", &why) && !why.empty(), "names: an empty name is refused", why);
    why.clear();
    check(!engine::validProjectName("   ", &why), "names: a whitespace name is refused");
    why.clear();
    check(!engine::validProjectName(".hidden", &why) && !why.empty(),
          "names: a leading dot is refused", why);
    why.clear();
    check(!engine::validProjectName("bad/name", &why), "names: a slash is refused");
    why.clear();
    check(!engine::validProjectName("bad:name", &why), "names: a colon is refused");
    why.clear();
    check(!engine::validProjectName("CON", &why) && !why.empty(),
          "names: a Windows reserved name is refused", why);
    why.clear();
    check(!engine::validProjectName(std::string(65, 'a'), &why),
          "names: a name over 64 characters is refused");
}

// ---- Templates ------------------------------------------------------------------------------

static void testTemplates() {
    const std::vector<engine::ProjectTemplate>& templates = engine::projectTemplates();
    check(templates.size() == 3, "templates: three are built in",
          std::to_string(templates.size()));

    bool unique = true;
    bool described = true;
    for (size_t i = 0; i < templates.size(); ++i) {
        for (size_t j = i + 1; j < templates.size(); ++j) {
            if (templates[i].id == templates[j].id) unique = false;
        }
        if (templates[i].name.empty() || templates[i].tagline.empty() ||
            templates[i].description.empty() || templates[i].bullets.empty() ||
            templates[i].accent.size() != 7 || templates[i].accent.front() != '#') {
            described = false;
        }
    }
    check(unique, "templates: ids are unique");
    check(described, "templates: every template has a name, tagline, description and accent");

    check(engine::findProjectTemplate("terrain") != nullptr, "templates: lookups find the terrain template");
    check(engine::findProjectTemplate("missing") == nullptr, "templates: an unknown id is not found");
}

// ---- The empty template ---------------------------------------------------------------------

static void testEmptyProject() {
    const fs::path root = tempRoot();
    std::vector<float> fractions;
    std::vector<std::string> messages;
    engine::CreateProjectOptions options;
    options.root = root.string();
    options.name = "Project Test";
    options.templateId = "empty";
    options.engineRoot = UKIYO_ROOT;

    std::string error;
    const bool created = engine::createProject(options, [&](float fraction, const std::string& message) {
        fractions.push_back(fraction);
        messages.push_back(message);
    }, &error);
    check(created, "empty: the project is created", error);
    if (!created) return;

    check(fs::exists(root / "ukiyo.project.json"), "empty: the manifest is written");
    check(fs::exists(root / "ProjectSettings" / "ProjectSettings.json"),
          "empty: ProjectSettings is written");
    check(fs::exists(root / "Scenes" / "Main.ukscene"), "empty: the main scene is written");
    check(fs::exists(root / "Scripts" / "hello.lua"), "empty: the starter script is written");
    check(fs::exists(root / "assets" / "README.md"), "empty: the assets note is written");

    engine::ProjectManifest manifest;
    check(engine::loadProjectManifest(root.string(), &manifest, &error) &&
              manifest.name == "Project Test" && manifest.templateId == "empty" &&
              manifest.mainScene == "Scenes/Main.ukscene",
          "empty: the manifest reads back", error);
    check(manifest.engineRoot == fs::path(UKIYO_ROOT).lexically_normal().string(),
          "empty: the manifest records the engine root", manifest.engineRoot);
    check(engine::isProject(root.string()), "empty: the folder is recognised as a project");

    engine::ProjectSettings settings;
    std::string settingsError;
    check(engine::loadProjectSettings(engine::projectSettingsPath(root.string()), settings, settingsError),
          "empty: ProjectSettings parses with the engine's loader", settingsError);
    check(settings.productName == "Project Test", "empty: the product name is the project name",
          settings.productName);
    check(settings.buildScenes.size() == 1 && settings.buildScenes.front() == "Scenes/Main.ukscene",
          "empty: the main scene is the first build scene");
    check(settings.inputActions.size() == 7, "empty: the default input actions are written",
          std::to_string(settings.inputActions.size()));

    const json scene = readJson(root / "Scenes" / "Main.ukscene");
    check(scene.value("format", std::string()) == "ukiyo-scene" && scene.value("version", 0) == 2,
          "empty: the scene is a version 2 Ukiyo scene");
    check(scene.contains("models") && scene["models"].is_array() && scene["models"].size() == 1,
          "empty: the scene has one model");
    check(scene["models"][0].value("empty", false),
          "empty: the model is the empty object");
    check(scene.contains("settings") && scene["settings"].value("grid", false),
          "empty: the scene shows the grid");

    check(!fractions.empty() && std::abs(fractions.back() - 1.0f) < 1e-6f,
          "empty: the last progress step is 1.0", fractions.empty() ? "none" : num(fractions.back()));
    bool increasing = true;
    bool described = true;
    for (size_t i = 0; i < fractions.size(); ++i) {
        if (fractions[i] < 0.0f || fractions[i] > 1.0f) increasing = false;
        if (i > 0 && fractions[i] < fractions[i - 1]) increasing = false;
        if (messages.size() == fractions.size() && messages[i].empty()) described = false;
    }
    check(increasing, "empty: progress never goes backwards", std::to_string(fractions.size()) + " steps");
    check(described, "empty: every progress step has a line for the splash");
    check(!messages.empty() && messages.front().find("Preparing") != std::string::npos,
          "empty: the first line names the project", messages.empty() ? "none" : messages.front());

    // The same folder cannot be reused while it holds a project.
    error.clear();
    check(!engine::createProject(options, {}, &error) && !error.empty(),
          "empty: a folder that already holds a project is refused", error);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- The anime template ---------------------------------------------------------------------

static void testAnimeProject() {
    const fs::path root = tempRoot();
    engine::CreateProjectOptions options;
    options.root = root.string();
    options.name = "Portrait";
    options.templateId = "anime";
    options.engineRoot = UKIYO_ROOT;

    std::string error;
    const bool created = engine::createProject(options, {}, &error);
    check(created, "anime: the project is created", error);
    if (!created) return;

    // The template links the engine's own asset, so the checkout has to have it and the engine root has to point at
    // it. Checked on its own because every failure below is a consequence of this one, and an empty model path three
    // checks later says nothing about why.
    check(fs::exists(fs::path(options.engineRoot) / "assets" / "Seed-san.vrm"),
          "anime: the engine asset resolves from the engine root", options.engineRoot);

    const json scene = readJson(root / "Scenes" / "Main.ukscene");
    const std::string path = scene["models"][0].value("path", std::string());
    // Only when it is wrong: the scene as written, so a platform difference is visible instead of inferred.
    if (path.empty())
        check(false, "anime: the scene records a model path", scene.contains("models") ? scene.dump().substr(0, 600)
                                                                                       : std::string("no models key"));
    check(!path.empty() && path.find('\\') == std::string::npos,
          "anime: the model is stored as a relative path with forward slashes", path);
    check(fs::exists(fs::path(root) / path),
          "anime: the relative path resolves to the engine asset",
          (fs::path(root) / path).string());
    check(path.find("Seed-san.vrm") != std::string::npos,
          "anime: the scene names Seed-san", path);
    check(scene["models"][0].value("name", std::string()) == "Seed-san",
          "anime: the instance is named Seed-san");
    check(scene.value("model", std::string()) == path,
          "anime: the older single-model key matches the first instance");
    check(scene.contains("gameCamera") &&
              scene["gameCamera"].value("distance", 0.0) > 1.0,
          "anime: the camera is framed on the character");

    std::error_code ec;
    fs::remove_all(root, ec);
}

static void testAnimeWithoutEngine() {
    const fs::path root = tempRoot();
    engine::CreateProjectOptions options;
    options.root = root.string();
    options.name = "No Engine";
    options.templateId = "anime";
    options.engineRoot.clear();

    std::vector<std::string> messages;
    std::string error;
    const bool created = engine::createProject(
        options, [&](float, const std::string& message) { messages.push_back(message); }, &error);
    check(created, "anime: a project still builds without an engine root", error);
    if (!created) return;

    const json scene = readJson(root / "Scenes" / "Main.ukscene");
    check(scene["models"][0].value("empty", false),
          "anime: the scene falls back to the empty object without the asset");
    bool explained = false;
    for (const std::string& message : messages) {
        if (message.find("not found") != std::string::npos ||
            message.find("starts empty") != std::string::npos) {
            explained = true;
        }
    }
    check(explained, "anime: the fallback is reported to the splash");

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- The terrain template -------------------------------------------------------------------

static void testTerrainProject() {
    const fs::path root = tempRoot();
    engine::CreateProjectOptions options;
    options.root = root.string();
    options.name = "Hills";
    options.templateId = "terrain";
    options.engineRoot = UKIYO_ROOT;

    std::vector<std::string> messages;
    std::string error;
    const bool created = engine::createProject(
        options, [&](float, const std::string& message) { messages.push_back(message); }, &error);
    check(created, "terrain: the project is created", error);
    if (!created) return;

    const fs::path heightmap = root / "assets" / "Terrain" / "hills.u16";
    check(fs::exists(heightmap), "terrain: the heightmap file is written");
    const auto size = fs::exists(heightmap) ? fs::file_size(heightmap) : 0;
    check(size == uintmax_t(257) * 257 * 2, "terrain: the heightmap is 257 x 257 16-bit samples",
          std::to_string(size) + " bytes");

    ukiyo::Heightmap map;
    map.cellSize = 1.0f;
    map.heightScale = 14.0f;
    std::string loadError;
    check(ukiyo::loadHeightmapU16(heightmap.string(), 257, &map, &loadError) && map.valid(),
          "terrain: the engine's loader reads the generated heightmap", loadError);
    if (map.valid()) {
        const auto [low, high] = std::minmax_element(map.samples.begin(), map.samples.end());
        check(*high - *low > 0.15f, "terrain: the hills have real relief",
              num(double(*high - *low)));
        const float middle = map.samples[size_t(128) * map.size + 128];
        const float basin = map.samples[size_t(100) * map.size + 160];
        check(basin < middle, "terrain: the lake basin sits below the surrounding hills",
              num(basin) + " vs " + num(middle));
    }

    const json scene = readJson(root / "Scenes" / "Main.ukscene");
    check(scene.contains("water"), "terrain: the scene has a water plane");
    check(scene["models"][0].value("empty", false), "terrain: the tile is an empty object");
    const json& terrain = scene["models"][0]["terrain"];
    check(terrain.value("heightmap", std::string()) == "assets/Terrain/hills.u16",
          "terrain: the scene points at the heightmap relatively",
          terrain.value("heightmap", std::string()));
    check(terrain.value("size", 0) == 257 && terrain.value("heightScale", 0.0) > 1.0,
          "terrain: the tile is 257 samples at a real height scale");
    check(terrain.contains("layers") && terrain["layers"].size() >= 3,
          "terrain: the default layers are stored");
    check(terrain.contains("foliage") && terrain["foliage"].size() == 2,
          "terrain: grass and pines are scattered");
    check(terrain.contains("grass") && terrain["grass"].value("enabled", false) &&
              terrain["grass"].value("density", 0.0) > 0.0 && terrain["grass"].contains("includeLayers"),
          "terrain: the new project grows grass");
    check(terrain["grass"]["includeLayers"].size() == 2,
          "terrain: the meadow covers the moss and the heather, not just the valley floor");
    // The vendored ground textures and the detail overlay are linked from the engine folder, relative to the project.
    if (!terrain["layers"][0].contains("diffuseMap"))
        check(false, "terrain: the first layer records its maps",
              terrain["layers"][0].dump().substr(0, 400) + " | engineRoot=" + std::string(UKIYO_ROOT));
    check(terrain["layers"][0].contains("diffuseMap") && terrain["layers"][0].contains("normalMap") &&
              terrain["layers"][0].contains("aoMap"),
          "terrain: layer 0 links the albedo, normal and occlusion of the vendored ground textures");
    check(terrain.contains("detail") && terrain["detail"].contains("map") &&
              terrain["detail"].value("metres", 0.0) > 0.0,
          "terrain: the detail overlay is linked too");

    bool sculpted = false;
    for (const std::string& message : messages) {
        if (message.find("Sculpting the terrain") != std::string::npos) sculpted = true;
    }
    check(sculpted, "terrain: the splash says the terrain is being sculpted");

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- Refusals and folders -------------------------------------------------------------------

static void testRefusals() {
    const fs::path root = tempRoot();
    engine::CreateProjectOptions options;
    options.root = root.string();
    options.name = "Bad/Name";
    options.templateId = "empty";

    std::string error;
    check(!engine::createProject(options, {}, &error) && !error.empty(),
          "refusals: an invalid name is refused", error);
    check(!fs::exists(root), "refusals: nothing is created for an invalid name");

    options.name = "Fine";
    options.templateId = "does-not-exist";
    error.clear();
    check(!engine::createProject(options, {}, &error), "refusals: an unknown template is refused");
    check(!fs::exists(root), "refusals: nothing is created for an unknown template");

    // A folder that already has files in it is left alone.
    std::error_code ec;
    fs::create_directories(root, ec);
    writeFile(root / "keep.txt", "do not touch");
    options.templateId = "empty";
    error.clear();
    check(!engine::createProject(options, {}, &error),
          "refusals: a folder with files is refused", error);
    check(fs::exists(root / "keep.txt"), "refusals: the existing file is untouched");
    fs::remove_all(root, ec);

    // An existing empty folder is used as the project.
    fs::create_directories(root, ec);
    check(engine::createProject(options, {}, &error), "folders: an existing empty folder is used", error);
    check(fs::exists(root / "Scenes" / "Main.ukscene"),
          "folders: the project lands in the existing folder");
    fs::remove_all(root, ec);
}

// ---- Manifests ------------------------------------------------------------------------------

static void testManifest() {
    const fs::path root = tempRoot();
    std::error_code ec;
    fs::create_directories(root, ec);

    engine::ProjectManifest manifest;
    manifest.name = "Manifest Test";
    manifest.templateId = "empty";
    manifest.mainScene = "Scenes/Level01.ukscene";
    manifest.engineRoot = "/opt/ukiyo";
    manifest.createdUtc = "2026-10-19T02:20:00Z";

    std::string error;
    check(engine::saveProjectManifest(root.string(), manifest, &error),
          "manifest: it saves", error);
    engine::ProjectManifest back;
    check(engine::loadProjectManifest(root.string(), &back, &error) &&
              back.name == manifest.name && back.templateId == manifest.templateId &&
              back.mainScene == manifest.mainScene && back.engineRoot == manifest.engineRoot &&
              back.createdUtc == manifest.createdUtc,
          "manifest: every field round trips", error);

    check(!engine::isProject(root.string()),
          "manifest: a manifest whose scene is missing is not a project");

    writeFile(root / "ukiyo.project.json", "{ not json");
    engine::ProjectManifest broken;
    error.clear();
    check(!engine::loadProjectManifest(root.string(), &broken, &error) && !error.empty(),
          "manifest: malformed JSON is refused with a reason", error);

    writeFile(root / "ukiyo.project.json", "{\"format\": \"something-else\"}");
    error.clear();
    check(!engine::loadProjectManifest(root.string(), &broken, &error),
          "manifest: a foreign manifest is refused");

    fs::remove_all(root, ec);
}

int main() {
    testNames();
    testTemplates();
    testEmptyProject();
    testAnimeProject();
    testAnimeWithoutEngine();
    testTerrainProject();
    testRefusals();
    testManifest();
    std::printf("project: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
