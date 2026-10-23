#include "pack/pack_cli.h"

#include <fstream>
#include <sstream>

#include <filesystem>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace ukiyo::pack {

const char* targetName(Target t) {
    switch (t) {
        case Target::Windows: return "windows";
        case Target::Android: return "android";
        case Target::Linux: break;
    }
    return "linux";
}

std::vector<std::string> packArgs(const PackRequest& request) {
    std::vector<std::string> args;
    auto flag = [&args](const char* name, const std::string& value) {
        if (value.empty()) return;
        args.push_back(name);
        args.push_back(value);
    };
    flag("--project", request.project);
    flag("--player", request.player);
    flag("--platform", targetName(request.target));
    flag("--out", request.outDir);
    flag("--zip", request.zip);
    flag("--shaders", request.shaders);
    flag("--root", request.root);
    if (!request.verify) args.push_back("--no-verify");
    return args;
}

bool setManifestVersion(const std::string& buildJsonPath, const std::string& version, std::string* error) {
    std::ifstream in(buildJsonPath, std::ios::binary);
    if (!in) {
        if (error) *error = "no " + buildJsonPath;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();
    nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (error) *error = buildJsonPath + " is not JSON";
        return false;
    }
    const auto format = doc.find("format");
    if (format == doc.end() || !format->is_string() || format->get<std::string>() != "ukiyo-build") {
        if (error) *error = buildJsonPath + " is not a build manifest";
        return false;
    }
    doc["version"] = version;
    std::ofstream out(buildJsonPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + buildJsonPath;
        return false;
    }
    out << doc.dump(2) << "\n";
    return true;
}

std::string manifestVersion(const std::string& buildJsonPath) {
    std::ifstream in(buildJsonPath, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    const nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return {};
    const auto version = doc.find("version");
    // "version": 1 is the manifest *format* version, not the game's; only a string is a game version.
    return version != doc.end() && version->is_string() ? version->get<std::string>() : std::string();
}

std::string defaultOutDir(const std::string& projectRoot, Target target) {
    std::string name = fs::path(projectRoot).filename().string();
    if (name.empty()) name = "game";
    return "dist/" + name + "-" + targetName(target);
}

} // namespace ukiyo::pack
