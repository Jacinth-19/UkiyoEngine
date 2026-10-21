#include "engine/project_settings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace engine {

namespace {

constexpr float kMinStep = 1.0f / 120.0f;
constexpr float kMaxStep = 1.0f / 20.0f;

std::string trimmed(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

template <typename T>
T clampTo(T v, T lo, T hi) {
    return std::min(std::max(v, lo), hi);
}

} // namespace

const std::array<const char*, 8>& builtinLayerNames() {
    static const std::array<const char*, 8> names{
        "Default", "TransparentFX", "Ignore Raycast", "Water", "UI", "Layer 5", "Layer 6", "Layer 7"};
    return names;
}

const std::vector<std::string>& builtinTags() {
    static const std::vector<std::string> tags{
        "Untagged", "Respawn", "Finish", "EditorOnly", "MainCamera", "Player", "GameController"};
    return tags;
}

void normalize(ProjectSettings& s) {
    // Tags: trimmed, non-empty, unique, and not one of the built-in tags.
    std::set<std::string> builtin(builtinTags().begin(), builtinTags().end());
    std::vector<std::string> tags;
    std::set<std::string> seen;
    for (const std::string& raw : s.tags) {
        const std::string t = trimmed(raw);
        if (t.empty() || builtin.count(t) || seen.count(t)) continue;
        seen.insert(t);
        tags.push_back(t);
    }
    s.tags = tags;

    // Layers: built-ins are fixed, user slots are trimmed and unique.
    std::set<std::string> layerSeen;
    for (size_t i = 0; i < s.layers.size(); ++i) {
        if (i < 8) {
            s.layers[i] = builtinLayerNames()[i];
            layerSeen.insert(s.layers[i]);
            continue;
        }
        std::string name = trimmed(s.layers[i]);
        if (name.empty() || layerSeen.count(name)) name.clear();
        if (!name.empty()) layerSeen.insert(name);
        s.layers[i] = name;
    }

    if (!std::isfinite(s.fixedTimestep)) s.fixedTimestep = 1.0f / 60.0f;
    s.fixedTimestep = clampTo(s.fixedTimestep, kMinStep, kMaxStep);
    if (!std::isfinite(s.timeScale)) s.timeScale = 1.0f;
    s.timeScale = clampTo(s.timeScale, 0.0f, 4.0f);

    for (float& c : {std::ref(s.gravity.x), std::ref(s.gravity.y), std::ref(s.gravity.z)}) {
        if (!std::isfinite(c)) c = 0.0f;
        c = clampTo(c, -100.0f, 100.0f);
    }
    s.velocitySolverSteps = clampTo(s.velocitySolverSteps, 1, 32);
    s.positionSolverSteps = clampTo(s.positionSolverSteps, 1, 32);

    // Input: an action needs a name and a binding (a key or a control path). Duplicate names keep the first entry.
    std::vector<InputAction> actions;
    std::set<std::string> actionNames;
    for (const InputAction& a : s.inputActions) {
        const std::string name = trimmed(a.name);
        const std::string key = trimmed(a.key);
        if (name.empty() || key.empty() || actionNames.count(name)) continue;
        actionNames.insert(name);
        actions.push_back({name, key});
    }
    s.inputActions = actions;

    if (trimmed(s.productName).empty()) s.productName = "Ukiyo Game";
    else s.productName = trimmed(s.productName);
    std::vector<std::string> scenes;
    std::set<std::string> sceneSeen;
    for (const std::string& raw : s.buildScenes) {
        const std::string p = trimmed(raw);
        if (p.empty() || sceneSeen.count(p)) continue;
        sceneSeen.insert(p);
        scenes.push_back(p);
    }
    s.buildScenes = scenes;

    s.buildStartScene = trimmed(s.buildStartScene);
    if (!s.buildStartScene.empty() && !sceneSeen.count(s.buildStartScene)) s.buildStartScene.clear();
    s.buildFont = trimmed(s.buildFont);
    // The same limits the manifest enforces, applied here so a typo in a spin box cannot write an unloadable build.
    if (s.buildWidth < 320) s.buildWidth = 320;
    if (s.buildWidth > 7680) s.buildWidth = 7680;
    if (s.buildHeight < 240) s.buildHeight = 240;
    if (s.buildHeight > 4320) s.buildHeight = 4320;
}

std::string projectSettingsPath(const std::string& projectRoot) {
    return projectRoot + "/ProjectSettings/ProjectSettings.json";
}

bool loadProjectSettings(const std::string& path, ProjectSettings& out, std::string& error) {
    out = ProjectSettings();
    if (!std::filesystem::exists(path)) {
        normalize(out);
        return true;
    }
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        error = std::string("malformed project settings: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "project settings must be a JSON object";
        return false;
    }

    if (j.contains("tags") && j["tags"].is_array())
        for (const auto& t : j["tags"]) if (t.is_string()) out.tags.push_back(t.get<std::string>());
    if (j.contains("layers") && j["layers"].is_array()) {
        size_t i = 0;
        for (const auto& l : j["layers"]) {
            if (i >= out.layers.size()) break;
            if (l.is_string()) out.layers[i] = l.get<std::string>();
            ++i;
        }
    }
    if (j.contains("time") && j["time"].is_object()) {
        const auto& t = j["time"];
        out.fixedTimestep = t.value("fixedTimestep", out.fixedTimestep);
        out.timeScale = t.value("timeScale", out.timeScale);
    }
    if (j.contains("physics") && j["physics"].is_object()) {
        const auto& p = j["physics"];
        if (p.contains("gravity") && p["gravity"].is_array() && p["gravity"].size() == 3) {
            out.gravity = {p["gravity"][0].get<float>(), p["gravity"][1].get<float>(), p["gravity"][2].get<float>()};
        }
        out.velocitySolverSteps = p.value("velocitySolverSteps", out.velocitySolverSteps);
        out.positionSolverSteps = p.value("positionSolverSteps", out.positionSolverSteps);
    }
    if (j.contains("input") && j["input"].is_array()) {
        for (const auto& a : j["input"]) {
            if (!a.is_object()) continue;
            out.inputActions.push_back({a.value("name", std::string()), a.value("key", std::string())});
        }
    }
    if (j.contains("build") && j["build"].is_object()) {
        const auto& b = j["build"];
        out.productName = b.value("productName", out.productName);
        if (b.contains("scenes") && b["scenes"].is_array())
            for (const auto& s : b["scenes"]) if (s.is_string()) out.buildScenes.push_back(s.get<std::string>());
        out.buildStartScene = b.value("startScene", out.buildStartScene);
        out.buildFont = b.value("font", out.buildFont);
        out.buildWidth = b.value("width", out.buildWidth);
        out.buildHeight = b.value("height", out.buildHeight);
        out.buildFullscreen = b.value("fullscreen", out.buildFullscreen);
    }
    normalize(out);
    return true;
}

bool saveProjectSettings(const std::string& path, const ProjectSettings& in, std::string& error) {
    ProjectSettings s = in;
    normalize(s);

    nlohmann::json j;
    j["format"] = "ukiyo-project-settings";
    j["version"] = 1;
    j["tags"] = s.tags;
    nlohmann::json layers = nlohmann::json::array();
    for (const std::string& l : s.layers) layers.push_back(l);
    j["layers"] = layers;
    j["time"] = {{"fixedTimestep", s.fixedTimestep}, {"timeScale", s.timeScale}};
    j["physics"] = {{"gravity", {s.gravity.x, s.gravity.y, s.gravity.z}},
                    {"velocitySolverSteps", s.velocitySolverSteps},
                    {"positionSolverSteps", s.positionSolverSteps}};
    nlohmann::json input = nlohmann::json::array();
    for (const InputAction& a : s.inputActions) input.push_back({{"name", a.name}, {"key", a.key}});
    j["input"] = input;
    j["build"] = {{"productName", s.productName},
                  {"scenes", s.buildScenes},
                  {"startScene", s.buildStartScene},
                  {"font", s.buildFont},
                  {"width", s.buildWidth},
                  {"height", s.buildHeight},
                  {"fullscreen", s.buildFullscreen}};

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out << j.dump(2) << '\n';
    return static_cast<bool>(out);
}

} // namespace engine
