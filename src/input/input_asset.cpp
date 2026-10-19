#include "input/input_asset.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace ukiyo::input {

namespace {

using nlohmann::json;

std::string getString(const json& j, const char* key, const std::string& fallback = "") {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

bool getBool(const json& j, const char* key, bool fallback = false) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

ActionType typeFromString(const std::string& s) {
    if (s == "Value") return ActionType::Value;
    if (s == "PassThrough") return ActionType::PassThrough;
    return ActionType::Button;   // "Button" and anything unknown
}

const char* typeToString(ActionType t) {
    switch (t) {
    case ActionType::Value: return "Value";
    case ActionType::PassThrough: return "PassThrough";
    case ActionType::Button: break;
    }
    return "Button";
}

DeviceKind deviceKindFromName(const std::string& path, bool* known) {
    *known = true;
    if (path == "<Keyboard>") return DeviceKind::Keyboard;
    if (path == "<Mouse>") return DeviceKind::Mouse;
    *known = path == "<Gamepad>" || path == "<Joystick>";
    return DeviceKind::Gamepad;
}

} // namespace

std::string makeGuid(const std::string& seed) {
    // FNV-1a over the seed, expanded to the 32 hex digits Unity uses. Not cryptographic: its only job is to be stable
    // for the same object between runs, which is what a diff in version control needs.
    uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull;
    for (unsigned char c : seed) {
        h1 = (h1 ^ c) * 1099511628211ull;
        h2 = (h2 ^ (c + 0x9e)) * 1469598103934665603ull;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", uint32_t(h1 >> 32), uint32_t(h1), uint32_t(h2 >> 32),
                  uint32_t(h2));
    return buf;
}

AssetLoadResult loadAssetJson(State& state, const std::string& text) {
    AssetLoadResult result;
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        result.error = std::string("not JSON: ") + e.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "the asset's root is not an object";
        return result;
    }
    if (!root.contains("maps") || !root["maps"].is_array()) {
        result.error = "no \"maps\" array: this is not a Unity .inputactions asset";
        return result;
    }

    state.maps().clear();
    state.controlSchemes.clear();
    state.assetName = getString(root, "name", "Ukiyo Controls");

    size_t mapIndex = 0;
    for (const json& jm : root["maps"]) {
        if (!jm.is_object()) continue;
        ActionMap& map = state.addMap(getString(jm, "name", "Map " + std::to_string(mapIndex)));
        map.id = getString(jm, "id", makeGuid("map/" + map.name));
        map.enabled = false;   // an asset describes maps; enabling one is the game's decision

        for (const json& ja : jm.value("actions", json::array())) {
            if (!ja.is_object()) continue;
            const std::string name = getString(ja, "name");
            if (name.empty()) {
                result.warnings.push_back("map \"" + map.name + "\": an action with no name was skipped");
                continue;
            }
            Action& a = map.add(name, typeFromString(getString(ja, "type", "Button")));
            a.id = getString(ja, "id", makeGuid("action/" + map.name + "/" + name));
            a.expectedControlType = getString(ja, "expectedControlType");
            a.processors = getString(ja, "processors");
            a.interactions = getString(ja, "interactions");
            // Unity's Initial State Check makes a Value action report its value on the first update even when no
            // control changed. The engine always evaluates every action each frame, which has the same effect, so the
            // flag is read and ignored — noted here rather than in a warning, since it is not a loss of behaviour.
        }

        for (const json& jb : jm.value("bindings", json::array())) {
            if (!jb.is_object()) continue;
            const std::string actionName = getString(jb, "action");
            Action* action = map.find(actionName);
            if (!action) {
                result.warnings.push_back("map \"" + map.name + "\": binding to unknown action \"" + actionName +
                                          "\" was skipped");
                continue;
            }
            Binding b;
            b.name = getString(jb, "name");
            b.id = getString(jb, "id", makeGuid("binding/" + map.name + "/" + actionName + "/" +
                                                std::to_string(action->bindings.size())));
            b.path = getString(jb, "path");
            b.action = actionName;
            b.groups = getString(jb, "groups");
            b.processors = getString(jb, "processors");
            b.interactions = getString(jb, "interactions");
            b.isComposite = getBool(jb, "isComposite");
            b.isPartOfComposite = getBool(jb, "isPartOfComposite");

            if (b.path.empty() && !b.isComposite) {
                result.warnings.push_back("map \"" + map.name + "\": binding \"" + b.name + "\" has no path; skipped");
                continue;
            }
            // Report the parts the engine cannot honour, but keep the binding so the asset round-trips intact.
            std::vector<std::string> unknown;
            parseProcessors(b.processors, &unknown);
            for (const std::string& u : unknown)
                result.warnings.push_back("map \"" + map.name + "\": unknown processor \"" + u + "\" on \"" +
                                          actionName + "\" is ignored");
            unknown.clear();
            parseInteractions(b.interactions, &unknown);
            for (const std::string& u : unknown)
                result.warnings.push_back("map \"" + map.name + "\": unknown interaction \"" + u + "\" on \"" +
                                          actionName + "\" is ignored");
            if (!b.isComposite && !b.isPartOfComposite) {
                DeviceKind kind;
                if (!deviceKindFromPath(b.path, &kind))
                    result.warnings.push_back("map \"" + map.name + "\": binding path \"" + b.path +
                                              "\" is not a <Device>/<control> path; it will never resolve");
            }
            action->bindings.push_back(std::move(b));
        }
        ++mapIndex;
    }

    for (const json& js : root.value("controlSchemes", json::array())) {
        if (!js.is_object()) continue;
        ControlScheme scheme;
        scheme.name = getString(js, "name");
        scheme.bindingGroup = getString(js, "bindingGroup");
        for (const json& jd : js.value("devices", json::array())) {
            const std::string path = getString(jd, "devicePath");
            bool known = false;
            const DeviceKind kind = deviceKindFromName(path, &known);
            if (!known) {
                result.warnings.push_back("control scheme \"" + scheme.name + "\": device \"" + path +
                                          "\" is not supported (no touch or XR devices)");
                continue;
            }
            scheme.devices.push_back(kind);
        }
        state.controlSchemes.push_back(std::move(scheme));
    }

    result.ok = true;
    return result;
}

AssetLoadResult loadAssetFile(State& state, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        AssetLoadResult r;
        r.error = "cannot open " + path;
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadAssetJson(state, ss.str());
}

std::string saveAssetJson(const State& state) {
    json root;
    root["name"] = state.assetName;
    json maps = json::array();
    for (const ActionMap& map : state.maps()) {
        json jm;
        jm["name"] = map.name;
        jm["id"] = map.id.empty() ? makeGuid("map/" + map.name) : map.id;
        json actions = json::array();
        for (const Action& a : map.actions) {
            json ja;
            ja["name"] = a.name;
            ja["type"] = typeToString(a.type);
            ja["id"] = a.id.empty() ? makeGuid("action/" + map.name + "/" + a.name) : a.id;
            ja["expectedControlType"] = a.expectedControlType;
            ja["processors"] = a.processors;
            ja["interactions"] = a.interactions;
            ja["initialStateCheck"] = a.type != ActionType::Button;
            actions.push_back(std::move(ja));
        }
        json bindings = json::array();
        for (const Action& a : map.actions) {
            for (const Binding& b : a.bindings) {
                json jb;
                jb["name"] = b.name;
                jb["id"] = b.id.empty()
                               ? makeGuid("binding/" + map.name + "/" + a.name + "/" + b.path)
                               : b.id;
                jb["path"] = b.path;
                jb["interactions"] = b.interactions;
                jb["processors"] = b.processors;
                jb["groups"] = b.groups;
                jb["action"] = a.name;
                jb["isComposite"] = b.isComposite;
                jb["isPartOfComposite"] = b.isPartOfComposite;
                bindings.push_back(std::move(jb));
            }
        }
        jm["actions"] = std::move(actions);
        jm["bindings"] = std::move(bindings);
        maps.push_back(std::move(jm));
    }
    root["maps"] = std::move(maps);
    json schemes = json::array();
    for (const ControlScheme& s : state.controlSchemes) {
        json js;
        js["name"] = s.name;
        js["bindingGroup"] = s.bindingGroup;
        json devices = json::array();
        for (DeviceKind k : s.devices) {
            json jd;
            jd["devicePath"] = std::string("<") + deviceName(k) + ">";
            jd["isOptional"] = false;
            jd["isOR"] = false;
            devices.push_back(std::move(jd));
        }
        js["devices"] = std::move(devices);
        schemes.push_back(std::move(js));
    }
    root["controlSchemes"] = std::move(schemes);
    return root.dump(4) + "\n";
}

bool saveAssetFile(const State& state, const std::string& path, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    out << saveAssetJson(state);
    if (!out) {
        if (error) *error = "write failed for " + path;
        return false;
    }
    return true;
}

} // namespace ukiyo::input
