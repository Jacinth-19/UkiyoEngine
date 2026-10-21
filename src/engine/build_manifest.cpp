#include "engine/build_manifest.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ukiyo {

namespace {
using nlohmann::json;

std::string readAll(const std::string& path, bool* ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

} // namespace

bool parseBuildManifest(const std::string& text, BuildManifest* out, std::string* error) {
    if (!out) return false;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        return fail(std::string("not JSON: ") + e.what());
    }
    if (!j.is_object()) return fail("the manifest is not an object");
    const std::string format = j.value("format", std::string());
    if (format != "ukiyo-build") return fail("format is not ukiyo-build");
    const int version = j.value("version", 0);
    if (version <= 0 || version > BuildManifest::kVersion) {
        return fail("unsupported manifest version " + std::to_string(version));
    }

    BuildManifest m;
    // `name` is the field; `productName` is what the Build page wrote before the player existed, and a project that has
    // been open since then still has one in Build/build.json.
    m.name = j.value("name", j.value("productName", m.name));
    m.startScene = j.value("startScene", std::string());
    m.version = j.value("versionName", m.version);
    m.font = j.value("font", std::string());
    m.hudScale = j.value("hudScale", 1.0f);
    if (m.hudScale < 0.25f) m.hudScale = 0.25f;
    if (m.hudScale > 8.0f) m.hudScale = 8.0f;

    if (j.contains("window") && j["window"].is_object()) {
        const json& w = j["window"];
        m.window.width = w.value("width", m.window.width);
        m.window.height = w.value("height", m.window.height);
        m.window.fullscreen = w.value("fullscreen", m.window.fullscreen);
        m.window.vsync = w.value("vsync", m.window.vsync);
    }
    m.window.width = std::max(320, std::min(m.window.width, 7680));
    m.window.height = std::max(240, std::min(m.window.height, 4320));

    if (j.contains("scenes")) {
        if (!j["scenes"].is_array()) return fail("scenes is not an array");
        for (const json& s : j["scenes"]) {
            if (s.is_string()) {
                m.scenes.push_back(s.get<std::string>());
            } else if (s.is_object()) {
                // The editor writes objects ({path, name}); accept both shapes so older and newer files load.
                const std::string p = s.value("path", std::string());
                if (!p.empty()) m.scenes.push_back(p);
            }
        }
    }
    if (m.scenes.empty()) return fail("the manifest lists no scenes");
    if (!m.startScene.empty()) {
        bool found = false;
        for (const std::string& s : m.scenes) found = found || (s == m.startScene);
        if (!found) return fail("startScene is not in the scene list: " + m.startScene);
    }
    *out = m;
    return true;
}

bool loadBuildManifest(const std::string& path, BuildManifest* out, std::string* error) {
    bool ok = false;
    const std::string text = readAll(path, &ok);
    if (!ok) {
        if (error) *error = "could not open " + path;
        return false;
    }
    std::string why;
    if (!parseBuildManifest(text, out, &why)) {
        if (error) *error = path + ": " + why;
        return false;
    }
    return true;
}

std::string firstScene(const BuildManifest& m) {
    if (!m.startScene.empty()) return m.startScene;
    return m.scenes.empty() ? std::string() : m.scenes.front();
}

std::string buildManifestJson(const BuildManifest& m) {
    json j;
    j["format"] = "ukiyo-build";
    j["version"] = BuildManifest::kVersion;
    j["name"] = m.name;
    j["versionName"] = m.version;
    j["startScene"] = m.startScene;
    j["font"] = m.font;
    j["hudScale"] = m.hudScale;
    j["window"] = {{"width", m.window.width}, {"height", m.window.height}, {"fullscreen", m.window.fullscreen},
                   {"vsync", m.window.vsync}};
    json scenes = json::array();
    for (const std::string& s : m.scenes) scenes.push_back(s);
    j["scenes"] = scenes;
    return j.dump(2);
}

} // namespace ukiyo
