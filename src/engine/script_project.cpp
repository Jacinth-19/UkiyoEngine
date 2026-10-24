#include "engine/script_project.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ukiyo::script_project {

namespace fs = std::filesystem;

namespace {

const char* kLuaRc = R"({
  "$schema": "https://raw.githubusercontent.com/LuaLS/vscode-lua/master/setting/schema.json",
  "runtime.version": "Lua 5.4",
  "runtime.builtin": {
    "io": "disable",
    "os": "disable",
    "package": "disable",
    "debug": "disable",
    "coroutine": "enable",
    "utf8": "enable"
  },
  "workspace.library": ["Scripts/.ukiyo/lua"],
  "workspace.checkThirdParty": false,
  "diagnostics.globals": ["self"],
  "diagnostics.unusedLocal": "Hint",
  "format.enable": true
}
)";

const char* kEditorConfig = R"(# Ukiyo projects keep one style across the editor, the formatter and the language server.
root = true

[*]
charset = utf-8
end_of_line = lf
insert_final_newline = true
trim_trailing_whitespace = true
indent_style = space
indent_size = 4

[*.lua]
indent_style = space
indent_size = 4
max_line_length = 120
)";

const char* kStyLua = R"(column_width = 120
line_endings = "Unix"
indent_type = "Spaces"
indent_width = 4
quote_style = "AutoPreferDouble"
call_parentheses = "Always"
)";

// selene: Lua 5.4 rules, and no complaint about the engine's globals (ukiyo, self) or about a script that is all hooks.
const char* kSelene = R"(std = "lua54"

[lints]
global_usage = "allow"
unscoped_variables = "allow"
multiple_statements = "allow"
)";



const char* kVscodeSettings = R"({
  "[lua]": {
    "editor.defaultFormatter": "JohnnyMorganz.stylua",
    "editor.formatOnSave": true,
    "editor.tabSize": 4
  },
  "Lua.runtime.version": "Lua 5.4",
  "Lua.workspace.library": ["Scripts/.ukiyo/lua"],
  "Lua.diagnostics.globals": ["self"],
  "files.trimTrailingWhitespace": true,
  "files.insertFinalNewline": true,
  "files.associations": {
    "*.ukshader": "glsl",
    "*.glsl": "glsl"
  }
}
)";

bool writeFile(const fs::path& path, const std::string& text, bool onlyWhenMissing, bool* wrote, std::string* error) {
    std::error_code ec;
    if (onlyWhenMissing && fs::exists(path, ec)) return true;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "could not write " + path.string();
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        if (error) *error = "could not write " + path.string();
        return false;
    }
    if (wrote) *wrote = true;
    return true;
}

} // namespace

const std::vector<ToolRecommendation>& toolRecommendations() {
    // One row per tool, and `.vscode/extensions.json` is built from the rows that have a VS Code id. The executables are
    // what the editor looks for on PATH to say "found here"; a tool that ships inside an extension has none.
    static const std::vector<ToolRecommendation> tools{
        {"LuaLS - Lua Language Server", "Language server", "sumneko.lua", "lua-language-server",
         "https://luals.github.io/",
         "Completion, hover and diagnostics for the whole ukiyo API, read from the stub this project ships."},
        {"StyLua", "Formatter", "JohnnyMorganz.stylua", "stylua", "https://github.com/JohnnyMorganz/StyLua",
         "One formatting style across the project, from stylua.toml: 4 spaces, 120 columns, double quotes."},
        {"Selene", "Linter", "Kampfkarren.selene-vscode", "selene", "https://kampfkarren.github.io/selene/",
         "Finds unused locals and unknown globals before play, with ukiyo and self declared as known."},
        {"EditorConfig for VS Code", "Editor support", "EditorConfig.EditorConfig", "",
         "https://editorconfig.org/",
         "Keeps indentation and line endings steady in every editor the project is opened in."},
        {"Lua Debug", "Debugger", "actboy168.lua-debug", "", "https://github.com/actboy168/lua-debug",
         "Breakpoints and stepping for Lua 5.4, for the day a script is attached to the running player."},
        {"Shader languages support (GLSL)", "File format", "slevesque.shader", "",
         "https://marketplace.visualstudio.com/items?itemName=slevesque.shader",
         "Highlighting for shaders/*.frag, *.vert, *.glsl and the project's *.ukshader files."},
    };
    return tools;
}

namespace {

// `.vscode/extensions.json`, from the recommendations that are VS Code extensions.
std::string vscodeExtensions() {
    std::string out = "{\n  \"recommendations\": [\n";
    bool first = true;
    for (const ToolRecommendation& tool : toolRecommendations()) {
        if (std::string(tool.vscodeId).empty()) continue;
        if (!first) out += ",\n";
        out += "    \"" + std::string(tool.vscodeId) + "\"";
        first = false;
    }
    out += "\n  ]\n}\n";
    return out;
}

} // namespace

const std::vector<ApiDoc>& apiDocs() {
    static const std::vector<ApiDoc> docs{
        {"log", "ukiyo.log(message)",
         "Writes a line to the editor console and the player's log."},
        {"time", "ukiyo.time()",
         "Seconds since the world started, already scaled by the project's time scale."},
        {"actionDown", "ukiyo.actionDown(action)",
         "True while the named Gameplay action is held for the local player."},
        {"hud", "ukiyo.hud(text)",
         "Sets the player's status line under the title; an empty string leaves the player's own message alone."},
        {"tr", "ukiyo.tr(key, fallback?)",
         "The text for a key in the language the project loaded (assets/locales/<code>.json). A missing key returns the "
         "fallback, or the key itself, so an untranslated line still says where it came from."},
        {"locale", "ukiyo.locale()",
         "The language code the game loaded, such as \"en\" or \"ja\"."},
        {"getPosition", "ukiyo.getPosition(name)",
         "Returns x, y, z of an entity, in metres."},
        {"setPosition", "ukiyo.setPosition(name, x, y, z)",
         "Moves an entity; a body or an agent is moved with it."},
        {"moveTo", "ukiyo.moveTo(name, x, y, z)",
         "Sends an agent to a point on the navmesh. Returns false when the entity has no agent."},
        {"impulse", "ukiyo.impulse(name, x, y, z)",
         "Adds an impulse to a dynamic body, in newton-seconds."},
        {"setParam", "ukiyo.setParam(name, param, value)",
         "Sets an animation parameter on an entity that has a state machine."},
        {"playSound", "ukiyo.playSound(path, entity?, bus?)",
         "Plays a sound file. With an entity the sound is positional; without one it is 2D. The optional bus is one of "
         "music, sfx (default), ambient — an unknown name is an error. Returns false when the file cannot be read."},
        {"playMusic", "ukiyo.playMusic(path, loop?, fadeSeconds?)",
         "Streams music on the music bus, outside the voice limit. A truthy loop repeats; fadeSeconds fades the volume "
         "in. Returns false when the file cannot be read."},
        {"stopMusic", "ukiyo.stopMusic(fadeSeconds?)",
         "Fades the music out over that many seconds — 0 stops at once — then ends it. Returns false when nothing was "
         "playing."},
        {"setBusVolume", "ukiyo.setBusVolume(bus, volume)",
         "Mixes one bus — music, sfx, ambient, or master — at 0..4. Returns false for an unknown bus name."},
        {"setBusReverb", "ukiyo.setBusReverb(bus, wet)",
         "How much of one bus (music, sfx, ambient) runs through the reverb insert: 0 is dry, 1 fully wet. Returns "
         "false for an unknown bus name."},
        {"frameTiming", "ukiyo.frameTiming()",
         "The last tick's cost in milliseconds by stage: input, scripts, physics, characters, navigation, animation, "
         "particles, entities, total. All zero until a profiled tick has run."},
        {"setLightEnabled", "ukiyo.setLightEnabled(name, enabled)",
         "Turns one of the scene's named lights on or off; the next frame shades with the change. Errors when no "
         "light in the scene carries that name."},
        {"lightEnabled", "ukiyo.lightEnabled(name)",
         "Whether the named scene light is on. Errors on an unknown name, as setLightEnabled does."},
        {"setLightIntensity", "ukiyo.setLightIntensity(name, intensity)",
         "Sets the named light's intensity — what a flicker or a warm-up animates. Errors on an unknown name."},
        {"saveSet", "ukiyo.saveSet(key, value)",
         "Stores a boolean, number or string in the run's save data. An integral number is stored as an integer."},
        {"saveGet", "ukiyo.saveGet(key)",
         "The value saveSet stored, typed: nil for a missing key or a value of another type."},
        {"saveHas", "ukiyo.saveHas(key)", "True when the run's save data holds this key."},
        {"saveWrite", "ukiyo.saveWrite(slot)",
         "Writes the save data to a slot file (0..63) in the project's saves folder. Returns false if the file "
         "could not be written."},
        {"saveRead", "ukiyo.saveRead(slot)",
         "Replaces the save data with slot n. A missing or corrupt file returns false and leaves the data as it "
         "was."},
        {"saveSlotExists", "ukiyo.saveSlotExists(slot)", "True when this slot has a file — a Continue row's test."},
        {"saveDelete", "ukiyo.saveDelete(slot)",
         "Removes a slot's file. Returns true when a file was removed, false when there was none."},
        {"hudRect", "ukiyo.hudRect(x, y, w, h, r?, g?, b?, a?)",
         "Draws a solid rectangle in this frame's HUD: pixels, origin top-left, colours 0..1 (default opaque "
         "white). Cleared every tick — call it from onUpdate."},
        {"spawnEffect", "ukiyo.spawnEffect(x, y, z, count?)",
         "Draws a one-shot particle burst at a point. Returns the effect's handle."},
        {"raycast", "ukiyo.raycast(ox, oy, oz, dx, dy, dz, maxDistance)",
         "Casts against the physics world. Returns false, or true and the hit point as x, y, z."},
        {"setContactFilter", "ukiyo.setContactFilter(a, b, enabled)",
         "Stops (false) or allows (true) new contacts between two entities that have bodies."},
        {"Entity", "ukiyo.Entity(name)",
         "An entity handle: .position (Vector3, writable), .rotation (Vector3, writable degrees), "
         ".yaw (number, writable, the y of rotation), .name (read only)."},
        {"Vector3", "ukiyo.Vector3(x, y, z)",
         "A value: x, y, z fields, addition, subtraction, scaling, and a readable tostring."},
        {"Quaternion", "ukiyo.Quaternion.lookRotation(x, y, z)",
         "The rotation that looks along a direction. Multiplies a Vector3 or another Quaternion."},
        {"joinPlayer", "ukiyo.joinPlayer(controlScheme?)",
         "Adds a local player and returns its index, or nil when no device is free."},
        {"playerCount", "ukiyo.playerCount()", "How many local players are joined."},
        {"leavePlayer", "ukiyo.leavePlayer(index)", "Removes a local player. Returns false when it was not joined."},
        {"playerActionDown", "ukiyo.playerActionDown(player, action)",
         "True while that player holds the action. This is what a split-screen game asks."},
        {"playerActionValue", "ukiyo.playerActionValue(player, action)",
         "An action's 1D value (a stick axis, a trigger) for one player."},
        {"playerActionVector2", "ukiyo.playerActionVector2(player, action)",
         "An action's 2D value (a stick) as x, y for one player."},
        {"loadScene", "ukiyo.loadScene(path)",
         "Asks for a scene switch. The switch happens between frames, once every load hold is released."},
        {"holdLoad", "ukiyo.holdLoad()", "Opens a load hold and returns its token; a switch waits for it."},
        {"releaseLoad", "ukiyo.releaseLoad(token)", "Releases a hold. Returns false for an unknown token."},
        {"preloadModel", "ukiyo.preloadModel(path)", "Asks the editor to load a model ahead of a switch."},
    };
    return docs;
}

namespace {

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// One API entry as Lua: the doc line, a ---@param for each argument (with '?' for the optional ones, which is how the
// language server knows a call may leave them out), and the function itself. The "?" is documentation spelling; Lua
// has no optional parameters, so it is stripped from the declaration.
void emitFunction(std::ostringstream& out, const ApiDoc& d) {
    std::string signature = d.signature;
    const size_t open = signature.find('(');
    const size_t close = signature.rfind(')');
    const std::string head = open == std::string::npos ? signature : signature.substr(0, open);
    std::string args;
    if (open != std::string::npos && close != std::string::npos && close > open)
        args = signature.substr(open + 1, close - open - 1);

    out << "--- " << d.doc << "\n";
    std::vector<std::string> names;
    std::string current;
    std::istringstream parts(args);
    while (std::getline(parts, current, ',')) {
        current = trimmed(current);
        if (current.empty()) continue;
        const bool optional = current.back() == '?';
        if (optional) current.pop_back();
        out << "---@param " << current << (optional ? "? " : " ") << "any\n";
        names.push_back(current);
    }
    out << "function " << head << "(";
    for (size_t i = 0; i < names.size(); ++i) out << (i ? ", " : "") << names[i];
    out << ") end\n\n";
}

} // namespace

std::string apiStub() {
    std::ostringstream out;
    out << "---@meta\n"
           "-- Ukiyo Engine scripting API, generated by the engine. Do not edit: the engine rewrites this file.\n"
           "-- The Lua language server reads it so ukiyo.* completes and hovers; no script runs it.\n\n"
           "--- The engine's functions. Every one of them is available to every script.\n"
           "---@class Ukiyo\n"
           "ukiyo = {}\n\n";
    for (const ApiDoc& d : apiDocs()) {
        if (std::string(d.name) == "Vector3" || std::string(d.name) == "Quaternion") continue;
        emitFunction(out, d);
    }
    out << "--- A 3-component vector. Add, subtract, scale, compare by value.\n"
           "---@class Ukiyo.Vector3\n"
           "---@field x number\n"
           "---@field y number\n"
           "---@field z number\n"
           "--- Creates a vector.\n"
           "---@param x number\n"
           "---@param y number\n"
           "---@param z number\n"
           "---@return Ukiyo.Vector3\n"
           "function ukiyo.Vector3(x, y, z) end\n\n"
           "--- A rotation. Multiplying one by a Vector3 rotates the vector.\n"
           "---@class Ukiyo.Quaternion\n"
           "---@field x number\n"
           "---@field y number\n"
           "---@field z number\n"
           "---@field w number\n"
           "--- The rotation that looks along a direction.\n"
           "---@param x number\n"
           "---@param y number\n"
           "---@param z number\n"
           "---@return Ukiyo.Quaternion\n"
           "function ukiyo.Quaternion.lookRotation(x, y, z) end\n\n"
           "--- A rotation around an axis, in degrees.\n"
           "---@param degrees number\n"
           "---@param x number\n"
           "---@param y number\n"
           "---@param z number\n"
           "---@return Ukiyo.Quaternion\n"
           "function ukiyo.Quaternion.angleAxis(degrees, x, y, z) end\n\n"
           "--- An entity in the running scene.\n"
           "---@class Ukiyo.Entity\n"
           "---@field position Ukiyo.Vector3  writable\n"
           "---@field rotation Ukiyo.Vector3  writable, degrees, x = pitch, y = yaw, z = roll\n"
           "---@field yaw number              writable, degrees, the y of rotation\n"
           "---@field name string             read only\n"
           "local Entity = {}\n\n"
           "--- An entity handle for a name in the running scene.\n"
           "---@param name string\n"
           "---@return Ukiyo.Entity\n"
           "function ukiyo.Entity(name) end\n\n"
           "-- Lifecycle hooks. A script defines the ones it needs; the engine calls them in this order.\n"
           "--- Called once, after the script is loaded.\n"
           "function onStart() end\n\n"
           "--- Called every frame, before physics.\n"
           "---@param dt number seconds since the last frame\n"
           "function onUpdate(dt) end\n\n"
           "--- Called once the scene has started, after every script's onStart.\n"
           "function onSceneLoaded() end\n\n"
           "--- Called when the scene is torn down. Release what the script holds here.\n"
           "function onDestroy() end\n\n"
           "--- Called for each physics contact.\n"
           "---@param contact { kind: \"added\"|\"removed\", a: string, b: string, point: Ukiyo.Vector3, normal: Ukiyo.Vector3 }\n"
           "function onContact(contact) end\n\n"
           "-- The entity this script belongs to, as a name: ukiyo.getPosition(self).\n"
           "---@type string\n"
           "self = \"\"\n";
    return out.str();
}

const std::vector<std::string>& supportFiles() {
    static const std::vector<std::string> files{
        "Scripts/.ukiyo/lua/ukiyo.lua", ".luarc.json", ".editorconfig",
        "stylua.toml",                  "selene.toml", ".vscode/extensions.json",
        ".vscode/settings.json",
    };
    return files;
}

bool writeSupportFiles(const std::string& projectRoot, std::vector<std::string>* written, std::string* error) {
    const fs::path root(projectRoot);
    struct Item {
        const char* rel;
        const std::string text;
        bool onlyWhenMissing;
    };
    std::vector<Item> items{
        {"Scripts/.ukiyo/lua/ukiyo.lua", apiStub(), false},
        {".luarc.json", kLuaRc, false},
        {".editorconfig", kEditorConfig, true},
        {"stylua.toml", kStyLua, true},
        {"selene.toml", kSelene, true},
        {".vscode/extensions.json", vscodeExtensions(), true},
        {".vscode/settings.json", kVscodeSettings, true},
    };
    for (const Item& item : items) {
        bool wrote = false;
        if (!writeFile(root / item.rel, item.text, item.onlyWhenMissing, &wrote, error)) return false;
        if (wrote && written) written->push_back(item.rel);
    }
    return true;
}

std::string installCommand(const ToolRecommendation& tool) {
    if (std::string(tool.vscodeId).empty()) return {};
    return std::string("code --install-extension ") + tool.vscodeId;
}

std::vector<std::string> completionWords() {
    std::vector<std::string> words;
    for (const ApiDoc& doc : apiDocs()) words.push_back(std::string("ukiyo.") + doc.name);
    for (const char* hook : {"onStart", "onUpdate", "onSceneLoaded", "onDestroy", "onContact"}) words.push_back(hook);
    words.push_back("self");
    words.push_back("require");
    return words;
}

} // namespace ukiyo::script_project
