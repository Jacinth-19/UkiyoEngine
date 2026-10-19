#include "engine/project_template.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/grass.h"
#include "engine/terrain.h"
#include "engine/water.h"

namespace engine {
namespace {

namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;

constexpr const char* kEngineVersion = "0.1";
constexpr const char* kMainScene = "Scenes/Main.ukscene";
constexpr const char* kManifestFile = "ukiyo.project.json";

bool writeText(const fs::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "could not write " + path.string();
        return false;
    }
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) {
        if (error) *error = "could not finish writing " + path.string();
        return false;
    }
    return true;
}

bool writeJson(const fs::path& path, const ojson& json, std::string* error) {
    return writeText(path, json.dump(2) + "\n", error);
}

std::string isoNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32] = {};
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Rounds to four decimals so generated files stay readable (0.3, not 0.30000001192092896).
double tidy(double value) { return std::round(value * 10000.0) / 10000.0; }

ojson numbers(std::initializer_list<double> values) {
    ojson array = ojson::array();
    for (double value : values) array.push_back(tidy(value));
    return array;
}

ojson vec3Json(const glm::vec3& v) { return numbers({v.x, v.y, v.z}); }

ojson cameraJson(double targetY, double distance, double yaw, double pitch) {
    ojson camera;
    camera["target"] = numbers({0.0, targetY, 0.0});
    camera["distance"] = distance;
    camera["yaw"] = yaw;
    camera["pitch"] = pitch;
    camera["fovY"] = 0.52;
    camera["nearZ"] = 0.1;
    camera["farZ"] = 400.0;
    return camera;
}

// The render settings a new scene starts with: studio light, anime tonemapping, and the helpers on.
ojson sceneSettings(bool grid) {
    ojson s;
    s["shadows"] = true;
    s["sky"] = true;
    s["grid"] = grid;
    s["outlines"] = true;
    s["bloom"] = true;
    s["tonemap"] = 3;
    s["look"] = 0;
    s["exposure"] = 0.84;
    s["sunIntensity"] = 1.25;
    s["sunDir"] = numbers({0.35, 0.75, 0.55});
    s["saturation"] = 1.18;
    s["contrast"] = 1.12;
    s["vignette"] = 0.12;
    return s;
}

ojson layerJson(const ukiyo::TerrainLayer& layer) {
    ojson o;
    o["name"] = layer.name;
    o["colour"] = vec3Json(layer.colour);
    o["roughness"] = tidy(layer.roughness);
    o["heightMin"] = tidy(layer.heightMin);
    o["heightMax"] = tidy(layer.heightMax);
    o["heightFade"] = tidy(layer.heightFade);
    o["slopeMin"] = tidy(layer.slopeMin);
    o["slopeMax"] = tidy(layer.slopeMax);
    o["slopeFade"] = tidy(layer.slopeFade);
    o["tile"] = tidy(layer.tileMetres);
    return o;
}

ojson foliageJson(const std::string& name, ukiyo::FoliageShape shape, float density, float heightMin,
                  float heightMax, float slopeMax, float scaleMin, float scaleMax, uint32_t seed,
                  const glm::vec3& tint) {
    ojson o;
    o["name"] = name;
    o["shape"] = shape == ukiyo::FoliageShape::Cone ? "cone" : "tuft";
    o["density"] = tidy(density);
    o["heightMin"] = tidy(heightMin);
    o["heightMax"] = tidy(heightMax);
    o["slopeMax"] = tidy(slopeMax);
    o["scaleMin"] = tidy(scaleMin);
    o["scaleMax"] = tidy(scaleMax);
    o["seed"] = seed;
    o["fadeStart"] = 70.0;
    o["fadeEnd"] = 110.0;
    o["tint"] = vec3Json(tint);
    return o;
}

// Grass (item 19). On by default in a new project, growing on layer 0 (the tile's grass layer), so a freshly made
// scene shows the system working without anyone opening the terrain inspector.
ojson grassJson() {
    ukiyo::GrassSettings g;
    g.enabled = true;
    g.density = 0.55f;
    g.height = 0.46f;
    g.width = 0.052f;
    g.blades = 3;
    g.segments = 4;
    g.scaleMin = 0.8f;
    g.scaleMax = 1.4f;
    g.bandNear = 26.0f;
    g.bandMid = 55.0f;
    g.cullDistance = 110.0f;
    g.includeLayers = 1u << 0;
    g.includeThreshold = 0.30f;
    g.windStrength = 0.24f;
    g.windFrequency = 0.42f;
    g.windScale = 0.10f;
    g.flutter = 0.05f;
    g.baseColour = glm::vec3(0.055f, 0.115f, 0.030f);
    g.tipColour = glm::vec3(0.34f, 0.52f, 0.15f);

    ojson o;
    o["enabled"] = true;
    o["density"] = tidy(g.density);
    o["heightMin"] = tidy(g.heightMin);
    o["heightMax"] = tidy(g.heightMax);
    o["slopeMax"] = tidy(g.slopeMax);
    o["height"] = tidy(g.height);
    o["width"] = tidy(g.width);
    o["blades"] = int(g.blades);
    o["segments"] = int(g.segments);
    o["scaleMin"] = tidy(g.scaleMin);
    o["scaleMax"] = tidy(g.scaleMax);
    o["seed"] = g.seed;
    o["groundAlign"] = tidy(g.groundAlign);
    o["bandNear"] = tidy(g.bandNear);
    o["bandMid"] = tidy(g.bandMid);
    o["cullDistance"] = tidy(g.cullDistance);
    ojson inc = ojson::array();
    for (uint32_t k = 0; k < ukiyo::kTerrainMaxLayers; ++k)
        if (g.includeLayers & (1u << k)) inc.push_back(int(k));
    o["includeLayers"] = inc;
    o["includeThreshold"] = tidy(g.includeThreshold);
    o["windDir"] = numbers({g.windDir.x, g.windDir.y});
    o["windStrength"] = tidy(g.windStrength);
    o["windFrequency"] = tidy(g.windFrequency);
    o["windScale"] = tidy(g.windScale);
    o["flutter"] = tidy(g.flutter);
    o["baseColour"] = vec3Json(g.baseColour);
    o["tipColour"] = vec3Json(g.tipColour);
    o["translucency"] = tidy(g.translucency);
    o["tipDarkening"] = tidy(g.tipDarkening);
    return o;
}

ojson terrainJson(const std::string& heightmapRelative) {
    ukiyo::TerrainSettings terrain;
    terrain.heightmap = heightmapRelative;
    terrain.size = 257;
    terrain.cellSize = 1.0f;
    terrain.heightScale = 14.0f;
    terrain.layers = ukiyo::defaultTerrainLayers();

    ojson o;
    o["heightmap"] = heightmapRelative;
    o["size"] = int(terrain.size);
    o["cellSize"] = double(terrain.cellSize);
    o["heightScale"] = double(terrain.heightScale);
    ojson layers = ojson::array();
    for (const ukiyo::TerrainLayer& layer : terrain.layers) layers.push_back(layerJson(layer));
    o["layers"] = layers;
    ojson foliage = ojson::array();
    foliage.push_back(foliageJson("grass", ukiyo::FoliageShape::Tuft, 0.42f, 0.05f, 0.62f, 34.0f, 0.7f,
                                  1.25f, 7, glm::vec3(0.28f, 0.46f, 0.20f)));
    foliage.push_back(foliageJson("pine", ukiyo::FoliageShape::Cone, 0.035f, 0.24f, 0.92f, 26.0f, 1.4f,
                                  2.6f, 21, glm::vec3(0.10f, 0.24f, 0.13f)));
    o["foliage"] = foliage;
    o["grass"] = grassJson();
    return o;
}

ojson waterJson() {
    ukiyo::WaterVolume water;
    water.centre = glm::vec2(128.0f, 128.0f);
    water.half = glm::vec2(150.0f, 150.0f);
    water.level = 3.2f;
    water.shallow = glm::vec3(0.12f, 0.46f, 0.50f);
    water.deep = glm::vec3(0.01f, 0.10f, 0.18f);
    water.absorption = 0.32f;
    water.foamWidth = 0.5f;
    water.refraction = 0.04f;
    water.density = 1000.0f;

    ojson o;
    o["centre"] = numbers({water.centre.x, water.centre.y});
    o["half"] = numbers({water.half.x, water.half.y});
    o["level"] = tidy(water.level);
    ojson waves = ojson::array();
    for (const ukiyo::WaterWave& wave : water.waves) {
        ojson w;
        w["dir"] = numbers({wave.dir.x, wave.dir.y});
        w["wavelength"] = tidy(wave.wavelength);
        w["amplitude"] = tidy(wave.amplitude);
        w["speed"] = tidy(wave.speed);
        w["steepness"] = tidy(wave.steepness);
        waves.push_back(w);
    }
    o["waves"] = waves;
    o["shallow"] = vec3Json(water.shallow);
    o["deep"] = vec3Json(water.deep);
    o["absorption"] = tidy(water.absorption);
    o["foamWidth"] = tidy(water.foamWidth);
    o["refraction"] = tidy(water.refraction);
    o["density"] = tidy(water.density);
    return o;
}

// A heightmap of rolling hills with a lake basin scooped out of the north-east, and softer ground
// around the rim so the tile edge does not end in a cliff.
bool generateTerrainHeightmap(const fs::path& path, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not make " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    ukiyo::Heightmap map;
    map.size = 257;
    map.cellSize = 1.0f;
    map.heightScale = 14.0f;
    map.samples.resize(size_t(map.size) * map.size);

    for (uint32_t z = 0; z < map.size; ++z) {
        for (uint32_t x = 0; x < map.size; ++x) {
            const float u = float(x) / float(map.size - 1);
            const float v = float(z) / float(map.size - 1);
            float h = 0.52f;
            h += 0.16f * std::sin((u * 2.0f + v * 0.6f) * 6.2831853f);
            h += 0.10f * std::sin((v * 3.1f - u * 1.4f) * 6.2831853f + 1.7f);
            h += 0.05f * std::sin((u + v) * 11.0f + 0.3f);
            h += 0.03f * std::sin((u * 7.0f - v * 5.0f) + 2.4f);
            // The lake: a smooth basin around (0.63, 0.40).
            const float dx = u - 0.63f, dz = v - 0.40f;
            h -= 0.34f * std::exp(-(dx * dx + dz * dz) * 55.0f);
            // A rim that settles toward the water level, so the tile edge is a shoreline, not a wall.
            const float edge = std::min({u, 1.0f - u, v, 1.0f - v});
            h = h * (0.55f + 0.45f * std::min(1.0f, edge * 9.0f)) + 0.06f;
            map.samples[size_t(z) * map.size + x] = std::clamp(h, 0.0f, 1.0f);
        }
    }
    return ukiyo::saveHeightmapU16(path.string(), map, error);
}

ojson sceneFor(const std::string& templateId, const std::string& engineRoot, const fs::path& root,
               bool* usedEngineAsset, std::string* note) {
    ojson scene;
    scene["format"] = "ukiyo-scene";
    scene["version"] = 2;

    if (templateId == "anime") {
        const char* candidates[] = {"assets/Seed-san.vrm", "assets/AliciaSolid_vrm-0.51.vrm"};
        for (const char* candidate : candidates) {
            const fs::path asset = fs::path(engineRoot) / candidate;
            if (engineRoot.empty() || !fs::exists(asset)) continue;
            std::error_code ec;
            fs::path relative = fs::relative(asset, root, ec);
            if (ec) relative = asset;
            const std::string path = relative.generic_string();
            scene["model"] = path;
            ojson model;
            model["path"] = path;
            model["position"] = numbers({0.0, 0.0, 0.0});
            model["yawDeg"] = 0.0;
            model["name"] = "Seed-san";
            scene["models"] = ojson::array({model});
            scene["active"] = 0;
            scene["settings"] = sceneSettings(false);
            scene["gameCamera"] = cameraJson(0.95, 2.6, 0.35, 0.08);
            if (usedEngineAsset) *usedEngineAsset = true;
            return scene;
        }
        if (note) *note = "the character asset was not found beside the engine, so the scene starts empty";
    }

    if (templateId == "terrain") {
        ojson model;
        model["empty"] = true;
        model["position"] = numbers({0.0, 0.0, 0.0});
        model["yawDeg"] = 0.0;
        model["name"] = "Terrain";
        model["terrain"] = terrainJson("assets/Terrain/hills.u16");
        scene["models"] = ojson::array({model});
        scene["active"] = 0;
        scene["settings"] = sceneSettings(false);
        ojson camera = cameraJson(4.0, 78.0, 0.62, 0.30);
        camera["target"] = numbers({128.0, 4.0, 128.0});
        camera["farZ"] = 900.0;
        scene["gameCamera"] = camera;
        scene["water"] = waterJson();
        return scene;
    }

    ojson model;
    model["empty"] = true;
    model["position"] = numbers({0.0, 0.0, 0.0});
    model["yawDeg"] = 0.0;
    model["name"] = "Empty Object";
    scene["models"] = ojson::array({model});
    scene["active"] = 0;
    scene["settings"] = sceneSettings(true);
    scene["gameCamera"] = cameraJson(0.95, 3.4, 0.25, 0.12);
    return scene;
}

// The same layout engine::saveProjectSettings writes: input[] entries and a build object.
ojson projectSettingsFor(const std::string& name) {
    ojson settings;
    settings["format"] = "ukiyo-project-settings";
    settings["version"] = 1;
    settings["tags"] = ojson::array({"Player", "Interactable"});
    settings["time"] = {{"fixedTimestep", 1.0 / 60.0}, {"timeScale", 1.0}};
    settings["physics"] = {{"gravity", numbers({0.0, -9.81, 0.0})},
                           {"velocitySolverSteps", 10},
                           {"positionSolverSteps", 2}};
    ojson actions = ojson::array();
    for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
             {"Jump", "Space"}, {"Interact", "E"}, {"Sprint", "LeftShift"}, {"MoveForward", "W"},
             {"MoveBack", "S"}, {"MoveLeft", "A"}, {"MoveRight", "D"}}) {
        actions.push_back({{"name", entry.first}, {"key", entry.second}});
    }
    settings["input"] = actions;
    settings["build"] = {{"productName", name}, {"scenes", ojson::array({kMainScene})}};
    return settings;
}

const char* kStarterScript =
    "-- Runs in play mode. ukiyo.log writes to the editor's Console.\n"
    "function onStart()\n"
    "    ukiyo.log(\"Hello from this Ukiyo project\")\n"
    "end\n"
    "\n"
    "function onUpdate(dt)\n"
    "end\n";

const char* kAssetsReadme =
    "# Assets\n"
    "\n"
    "Files this project owns live here. Paths in a scene are relative to the project folder.\n"
    "\n"
    "- `assets/Terrain/hills.u16` (Terrain Lab) is a raw 16-bit heightmap, 257 x 257 samples.\n"
    "- Models, materials and shaders placed here travel with the project.\n"
    "\n"
    "Content that ships with the engine (the starter characters, for example) is not copied in.\n"
    "The manifest records where the engine lives, and the scene links to it with a relative path.\n";

} // namespace

const std::vector<ProjectTemplate>& projectTemplates() {
    static const std::vector<ProjectTemplate> templates{
        {"empty",
         "Empty Scene",
         "One object, soft studio light, and nothing in the way.",
         "A clean stage for whatever you have in mind. The scene has a single empty object to "
         "replace or build around, anime tonemapping, sky, grid and outlines on, and a starter Lua "
         "script wired up and ready for play mode.",
         {"Empty object ready to replace", "Anime tonemapping, sky and outlines on",
          "Starter Lua script and project settings"},
         "#4a9ef0"},
        {"anime",
         "Anime Character",
         "Seed-san under anime lighting, framed for a portrait.",
         "A character showcase in the style the engine is built for. Seed-san loads from the engine's "
         "asset folder, the camera is framed at portrait distance, and the scene is set up with the "
         "Custom Anime tonemap, outlines and bloom. Add a terrain tile or a water plane when the "
         "portrait needs a stage.",
         {"Seed-san linked from the engine assets", "Portrait camera and anime tonemap",
          "Outlines, bloom and cel-friendly light"},
         "#e8748f"},
        {"terrain",
         "Terrain Lab",
         "Rolling hills, grass tufts, pines and a still lake, ready to sculpt.",
         "A 256 x 256 metre terrain tile with a generated 257 x 257 heightmap: rolling hills, a lake "
         "basin, grass tufts and pines scattered by the foliage system, and a calm water plane at the "
         "lake's level. Paint layers, sculpt heights, or drop a character on it.",
         {"Generated 257 x 257 heightmap with a lake", "Grass and pine foliage",
          "Water plane and a camera framed on the hills"},
         "#5fd0a0"},
    };
    return templates;
}

const ProjectTemplate* findProjectTemplate(const std::string& id) {
    for (const ProjectTemplate& projectTemplate : projectTemplates()) {
        if (projectTemplate.id == id) return &projectTemplate;
    }
    return nullptr;
}

std::string projectManifestPath(const std::string& root) {
    return (fs::path(root) / kManifestFile).string();
}

bool loadProjectManifest(const std::string& root, ProjectManifest* out, std::string* error) {
    if (!out) {
        if (error) *error = "no manifest to fill";
        return false;
    }
    const fs::path path = fs::path(root) / kManifestFile;
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "no " + std::string(kManifestFile) + " in " + root;
        return false;
    }
    ojson json;
    try {
        json = ojson::parse(in);
    } catch (const std::exception& e) {
        if (error) *error = std::string("malformed project manifest: ") + e.what();
        return false;
    }
    if (json.value("format", std::string()) != "ukiyo-project") {
        if (error) *error = std::string(kManifestFile) + " is not a Ukiyo project manifest";
        return false;
    }
    ProjectManifest manifest;
    manifest.name = json.value("name", std::string());
    manifest.templateId = json.value("template", std::string());
    manifest.mainScene = json.value("mainScene", std::string(kMainScene));
    if (json.contains("engine") && json["engine"].is_object()) {
        manifest.engineRoot = json["engine"].value("root", std::string());
        manifest.engineVersion = json["engine"].value("version", std::string(kEngineVersion));
    }
    manifest.createdUtc = json.value("created", std::string());
    if (trimmed(manifest.name).empty()) manifest.name = fs::path(root).filename().string();
    *out = manifest;
    return true;
}

bool saveProjectManifest(const std::string& root, const ProjectManifest& manifest, std::string* error) {
    ojson json;
    json["format"] = "ukiyo-project";
    json["version"] = 1;
    json["name"] = manifest.name;
    json["template"] = manifest.templateId;
    json["mainScene"] = manifest.mainScene;
    json["engine"] = {{"version", manifest.engineVersion.empty() ? kEngineVersion : manifest.engineVersion},
                      {"root", manifest.engineRoot}};
    json["created"] = manifest.createdUtc.empty() ? isoNow() : manifest.createdUtc;
    return writeJson(fs::path(root) / kManifestFile, json, error);
}

bool isProject(const std::string& root) {
    ProjectManifest manifest;
    std::string error;
    if (!loadProjectManifest(root, &manifest, &error)) return false;
    return fs::exists(fs::path(root) / manifest.mainScene);
}

bool validProjectName(const std::string& name, std::string* why) {
    const std::string clean = trimmed(name);
    auto refuse = [why](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (clean.empty()) return refuse("Give the project a name.");
    if (clean.size() > 64) return refuse("Keep the name under 64 characters.");
    if (clean.front() == '.') return refuse("A name cannot start with a dot.");
    for (char c : clean) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool allowed = std::isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '.';
        if (!allowed) return refuse("Use letters, digits, spaces, '-', '_' and '.' only.");
    }
    static const std::set<std::string> reserved{"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3",
                                                "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1",
                                                "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string upper = clean;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (reserved.count(upper)) return refuse("\"" + clean + "\" is a reserved name on Windows.");
    return true;
}

bool createProject(const CreateProjectOptions& options, const ProjectProgress& progress, std::string* error) {
    auto report = [&progress](float fraction, const std::string& message) {
        if (progress) progress(fraction, message);
    };
    auto fail = [error](const std::string& reason) {
        if (error) *error = reason;
        return false;
    };

    if (error) error->clear();
    const std::string name = trimmed(options.name);
    std::string why;
    if (!validProjectName(name, &why)) return fail(why);
    if (!findProjectTemplate(options.templateId)) {
        return fail("unknown project template \"" + options.templateId + "\"");
    }

    std::error_code ec;
    fs::path root = fs::path(options.root);
    if (root.is_relative()) root = fs::absolute(root, ec);
    root = root.lexically_normal();
    if (!root.filename().empty() && root.filename() == ".") root = root.parent_path();
    if (root.filename().empty() && root.has_parent_path()) root = root.parent_path();
    if (root.empty()) return fail("the project needs a folder");

    bool createdRoot = false;
    if (fs::exists(root, ec)) {
        if (!fs::is_directory(root, ec)) return fail(root.string() + " is not a folder");
        if (!fs::is_empty(root, ec)) return fail(root.string() + " already has files in it");
    } else {
        fs::create_directories(root, ec);
        if (ec) return fail("could not make " + root.string() + ": " + ec.message());
        createdRoot = true;
    }

    auto cleanup = [&] {
        std::error_code ignored;
        if (createdRoot) {
            fs::remove_all(root, ignored);
        } else {
            for (const char* entry : {kManifestFile, "ProjectSettings", "Scenes", "assets", "Scripts"}) {
                fs::remove_all(root / entry, ignored);
            }
        }
    };

    std::string engineRoot;
    if (!options.engineRoot.empty()) {
        engineRoot = fs::path(options.engineRoot).lexically_normal().string();
    }

    report(0.03f, "Preparing " + name);
    report(0.12f, "Writing the project manifest");
    ProjectManifest manifest;
    manifest.name = name;
    manifest.templateId = options.templateId;
    manifest.mainScene = kMainScene;
    manifest.engineRoot = engineRoot;
    manifest.engineVersion = kEngineVersion;
    manifest.createdUtc = isoNow();
    if (!saveProjectManifest(root.string(), manifest, error)) {
        cleanup();
        return false;
    }

    report(0.24f, "Creating ProjectSettings");
    if (!writeJson(root / "ProjectSettings" / "ProjectSettings.json", projectSettingsFor(name), error)) {
        cleanup();
        return false;
    }

    bool usedEngineAsset = false;
    std::string note;
    if (options.templateId == "terrain") {
        report(0.38f, "Sculpting the terrain: 257 x 257 samples");
        if (!generateTerrainHeightmap(root / "assets" / "Terrain" / "hills.u16", error)) {
            cleanup();
            return false;
        }
        report(0.62f, "Scattering grass and pines");
    } else if (options.templateId == "anime") {
        report(0.40f, "Linking the character from the engine assets");
    } else {
        report(0.40f, "Building the main scene");
    }

    if (!writeText(root / "assets" / "README.md", kAssetsReadme, error)) {
        cleanup();
        return false;
    }
    if (!writeText(root / "Scripts" / "hello.lua", kStarterScript, error)) {
        cleanup();
        return false;
    }

    report(0.74f, "Building the main scene");
    const ojson scene = sceneFor(options.templateId, engineRoot, root, &usedEngineAsset, &note);
    if (!writeJson(root / kMainScene, scene, error)) {
        cleanup();
        return false;
    }
    if (usedEngineAsset) {
        report(0.84f, "Checking the linked engine assets");
    }

    report(0.92f, "Checking the project");
    ProjectManifest check;
    std::string manifestError;
    if (!loadProjectManifest(root.string(), &check, &manifestError) || !isProject(root.string())) {
        cleanup();
        return fail("the new project did not verify: " + manifestError);
    }

    report(1.0f, note.empty() ? "Ready" : "Ready (" + note + ")");
    return true;
}

} // namespace engine
