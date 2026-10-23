#include "engine/localisation.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace ukiyo {

namespace {
const char* kLocaleDir = "assets/locales";

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

bool LocaleCatalog::has(const std::string& key) const { return strings.count(key) != 0; }

std::string LocaleCatalog::tr(const std::string& key) const {
    auto it = strings.find(key);
    return it == strings.end() ? key : it->second;
}

bool loadLocale(const std::string& projectRoot, const std::string& code, LocaleCatalog* out, std::string* error) {
    if (code.empty()) {
        if (error) *error = "no language given";
        return false;
    }
    const std::string path = (fs::path(projectRoot) / kLocaleDir / (code + ".json")).string();
    const std::string text = readFile(path);
    if (text.empty()) {
        if (error) *error = "no " + path;
        return false;
    }
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (error) *error = path + " is not JSON";
        return false;
    }
    LocaleCatalog catalog;
    catalog.code = code;
    // A catalogue may be flat or wrapped in {"strings": {...}}; both are what a translator's tool writes.
    const nlohmann::json& strings = doc.contains("strings") && doc["strings"].is_object() ? doc["strings"] : doc;
    for (auto it = strings.begin(); it != strings.end(); ++it) {
        if (it.value().is_string()) catalog.strings[it.key()] = it.value().get<std::string>();
    }
    if (out) *out = std::move(catalog);
    return true;
}

std::string localeJson(const LocaleCatalog& catalog) {
    nlohmann::json doc = nlohmann::json::object();
    doc["format"] = "ukiyo-locale";
    doc["version"] = 1;
    doc["locale"] = catalog.code;
    nlohmann::json strings = nlohmann::json::object();
    for (const auto& [key, text] : catalog.strings) strings[key] = text;
    doc["strings"] = strings;
    return doc.dump(2) + "\n";
}

std::vector<std::string> availableLocales(const std::string& projectRoot) {
    std::vector<std::string> codes;
    std::error_code ec;
    const fs::path dir = fs::path(projectRoot) / kLocaleDir;
    if (!fs::is_directory(dir, ec)) return codes;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".json") codes.push_back(entry.path().stem().string());
    }
    std::sort(codes.begin(), codes.end());
    return codes;
}

std::string baseLocale(const std::string& projectRoot) {
    // project.json's "locale", then the first catalogue, then English.
    std::error_code ec;
    for (const char* file : {"Project/project.json", "project.json", "Build/project.json"}) {
        const fs::path p = fs::path(projectRoot) / file;
        if (!fs::exists(p, ec)) continue;
        const nlohmann::json doc = nlohmann::json::parse(readFile(p), nullptr, false);
        if (!doc.is_discarded() && doc.is_object()) {
            const auto at = doc.find("locale");
            if (at != doc.end() && at->is_string() && !at->get<std::string>().empty()) return at->get<std::string>();
        }
    }
    const std::vector<std::string> codes = availableLocales(projectRoot);
    return codes.empty() ? "en" : codes.front();
}

LocaleCatalog newLocale(const LocaleCatalog& base, const std::string& code) {
    LocaleCatalog out;
    out.code = code;
    out.strings = base.strings;   // the translator replaces the values; the keys are the mapping
    return out;
}

} // namespace ukiyo
