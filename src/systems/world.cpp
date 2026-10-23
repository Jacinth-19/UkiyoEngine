#include "systems/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

#include <glm/gtc/quaternion.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace ukiyo {

namespace uinput = input;
namespace {

World* worldOf(lua_State* L) { return static_cast<World*>(lua_touserdata(L, lua_upvalueindex(1))); }

glm::vec3 readVec3(lua_State* L, int first) {
    return glm::vec3(float(luaL_checknumber(L, first)), float(luaL_checknumber(L, first + 1)),
                     float(luaL_checknumber(L, first + 2)));
}

// ukiyo.log(message)
int l_log(lua_State* L) {
    World* w = worldOf(L);
    w->logs.push_back(luaL_checkstring(L, 1));
    return 0;
}

// ukiyo.loadScene(path). Asks for a switch to another .ukscene, performed once no hold is open.
int l_loadScene(lua_State* L) {
    worldOf(L)->requestScene(luaL_checkstring(L, 1));
    return 0;
}

// ukiyo.holdLoad() -> token. Stops a requested scene switch until ukiyo.releaseLoad(token).
int l_holdLoad(lua_State* L) {
    lua_pushinteger(L, worldOf(L)->holdLoad());
    return 1;
}

// ukiyo.releaseLoad(token) -> bool
int l_releaseLoad(lua_State* L) {
    const lua_Integer token = luaL_checkinteger(L, 1);
    lua_pushboolean(L, worldOf(L)->releaseLoad(static_cast<int>(token)) ? 1 : 0);
    return 1;
}

// ukiyo.preloadModel(path). Asks the editor to load a model before it is needed.
int l_preloadModel(lua_State* L) {
    worldOf(L)->requestPreload(luaL_checkstring(L, 1));
    return 0;
}

// ukiyo.time() -> seconds
int l_time(lua_State* L) {
    lua_pushnumber(L, worldOf(L)->time());
    return 1;
}

// ukiyo.actionDown(action) -> true while the key bound to the action is held
int l_actionDown(lua_State* L) {
    const char* action = luaL_checkstring(L, 1);
    lua_pushboolean(L, worldOf(L)->actionDown(action ? action : "") ? 1 : 0);
    return 1;
}

// ukiyo.hud(text) -> the line the player shows under the title. A game's own status line, not a log.
int l_hud(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    worldOf(L)->setHudText(text ? text : "");
    return 0;
}

// ukiyo.tr(key[, fallback]) -> the text for a key in the game's language. Locales are project files
// (assets/locales/<code>.json, src/engine/localisation.h) loaded by the player; a world with none returns the fallback,
// so a script written before a project has translations still reads correctly.
int l_tr(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    World* w = worldOf(L);
    std::string text = w->translate(key ? key : "");
    if (text.empty() && lua_gettop(L) >= 2) text = luaL_checkstring(L, 2);
    if (text.empty()) text = key ? key : "";
    lua_pushlstring(L, text.c_str(), text.size());
    return 1;
}

// ukiyo.locale() -> the language code the game loaded ("en" when it has none)
int l_locale(lua_State* L) {
    const std::string code = worldOf(L)->localeCode();
    lua_pushlstring(L, code.c_str(), code.size());
    return 1;
}

// ukiyo.joinPlayer([scheme]) -> index, or nil when no free device matches
int l_joinPlayer(lua_State* L) {
    World* w = worldOf(L);
    ukiyo::input::JoinOptions options;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) options.controlScheme = luaL_checkstring(L, 1);
    ukiyo::input::PlayerInput* p = w->inputPlayers().join(options);
    if (!p) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, p->index());
    return 1;
}

// ukiyo.playerCount() -> int
int l_playerCount(lua_State* L) {
    World* w = worldOf(L);
    lua_pushinteger(L, w->hasInputPlayers() ? w->inputPlayers().playerCount() : 0);
    return 1;
}

// ukiyo.leavePlayer(index) -> bool
int l_leavePlayer(lua_State* L) {
    World* w = worldOf(L);
    if (!w->hasInputPlayers()) { lua_pushboolean(L, 0); return 1; }
    const int index = int(luaL_checkinteger(L, 1));
    const bool had = w->inputPlayers().player(index) != nullptr;
    w->inputPlayers().leaveIndex(index);
    lua_pushboolean(L, had ? 1 : 0);
    return 1;
}

// The action a player is currently driving, or null. Shared by the three readers below.
const ukiyo::input::Action* playerAction(World* w, int index, const char* name) {
    if (!w->hasInputPlayers()) return nullptr;
    ukiyo::input::PlayerInput* p = w->inputPlayers().player(index);
    return p ? p->action("", name) : nullptr;
}

// ukiyo.playerActionDown(index, action) -> bool
int l_playerActionDown(lua_State* L) {
    const ukiyo::input::Action* a = playerAction(worldOf(L), int(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2));
    lua_pushboolean(L, a && a->pressed() ? 1 : 0);
    return 1;
}

// ukiyo.playerActionValue(index, action) -> number
int l_playerActionValue(lua_State* L) {
    const ukiyo::input::Action* a = playerAction(worldOf(L), int(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2));
    lua_pushnumber(L, a ? a->readFloat() : 0.0);
    return 1;
}

// ukiyo.playerActionVector2(index, action) -> x, y
int l_playerActionVector2(lua_State* L) {
    const ukiyo::input::Action* a = playerAction(worldOf(L), int(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2));
    const glm::vec2 v = a ? a->readVector2() : glm::vec2(0.0f);
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    return 2;
}

// ukiyo.getPosition(name) -> x, y, z
int l_getPosition(lua_State* L) {
    World* w = worldOf(L);
    Entity* e = w->findEntity(luaL_checkstring(L, 1));
    const glm::vec3 p = e ? e->position : glm::vec3(0.0f);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    lua_pushnumber(L, p.z);
    return 3;
}

// ukiyo.setPosition(name, x, y, z)
int l_setPosition(lua_State* L) {
    worldOf(L)->setEntityPosition(luaL_checkstring(L, 1), readVec3(L, 2));
    return 0;
}

// ukiyo.moveTo(name, x, y, z) -> bool. Needs a navmesh and an agent on the entity.
int l_moveTo(lua_State* L) {
    World* w = worldOf(L);
    Entity* e = w->findEntity(luaL_checkstring(L, 1));
    const bool ok = e && e->agent >= 0 && w->nav.moveAgent(e->agent, readVec3(L, 2));
    lua_pushboolean(L, ok);
    return 1;
}

// ukiyo.impulse(name, x, y, z)
int l_impulse(lua_State* L) {
    World* w = worldOf(L);
    Entity* e = w->findEntity(luaL_checkstring(L, 1));
    if (e && e->body != PhysicsWorld::kNone) w->physics.addImpulse(e->body, readVec3(L, 2));
    return 0;
}

// ukiyo.setParam(name, param, value)
int l_setParam(lua_State* L) {
    worldOf(L)->setParam(luaL_checkstring(L, 1), luaL_checkstring(L, 2), float(luaL_checknumber(L, 3)));
    return 0;
}

// The mixer bus a script names, or false for an unknown word.
bool busFromName(const char* name, AudioEngine::Bus* out) {
    if (std::strcmp(name, "music") == 0) { *out = AudioEngine::Bus::Music; return true; }
    if (std::strcmp(name, "sfx") == 0) { *out = AudioEngine::Bus::Sfx; return true; }
    if (std::strcmp(name, "ambient") == 0) { *out = AudioEngine::Bus::Ambient; return true; }
    return false;
}

// ukiyo.playSound(path, name[, bus]) -> bool. Plays at the entity's position when name is given; on the named
// mixer bus ("music", "sfx", "ambient") when bus is given, on Sfx otherwise.
int l_playSound(lua_State* L) {
    World* w = worldOf(L);
    const AudioEngine::SoundId id = w->soundFor(luaL_checkstring(L, 1));
    AudioEngine::Bus bus = AudioEngine::Bus::Sfx;
    if (lua_isstring(L, 3) && !busFromName(lua_tostring(L, 3), &bus))
        return luaL_error(L, "playSound: unknown bus \"%s\" (music, sfx, ambient)", lua_tostring(L, 3));
    bool ok = false;
    if (id) {
        if (lua_isstring(L, 2)) {
            Entity* e = w->findEntity(lua_tostring(L, 2));
            if (e) {
                w->audio.setPosition(id, e->position);
                w->audio.setSpatial(id, true);
            }
        } else {
            w->audio.setSpatial(id, false);
        }
        ok = w->audio.playOn(id, bus);
    }
    lua_pushboolean(L, ok);
    return 1;
}

// ukiyo.frameTiming() -> table. The last tick's breakdown in milliseconds: input, scripts, physics, characters,
// navigation, animation, particles, entities, total. Zeros until a profiled tick has run (World::setProfiling).
// This is the number a script-side debug HUD prints; the editor's overlay reads the same struct directly.
int l_frameTiming(lua_State* L) {
    const FrameTiming& t = worldOf(L)->lastTiming();
    lua_createtable(L, 0, 9);
    const struct { const char* name; float ms; } stages[] = {
        {"input", t.input},         {"scripts", t.scripts},   {"physics", t.physics},
        {"characters", t.characters}, {"navigation", t.navigation}, {"animation", t.animation},
        {"particles", t.particles}, {"entities", t.entities}, {"total", t.total},
    };
    for (const auto& st : stages) {
        lua_pushnumber(L, st.ms);
        lua_setfield(L, -2, st.name);
    }
    return 1;
}

// ukiyo.playMusic(path, loop[, fadeInSeconds]) -> bool. Streaming, on the Music bus, never stolen by the voice
// limit. `loop` is a boolean; a non-zero number works, as lua_toboolean accepts any value.
int l_playMusic(lua_State* L) {
    World* w = worldOf(L);
    const bool loop = lua_toboolean(L, 2) != 0;
    const float fade = float(luaL_optnumber(L, 3, 0.0));
    lua_pushboolean(L, w->audio.playMusic(w->resolveMediaPath(luaL_checkstring(L, 1)), loop, fade));
    return 1;
}

// The scene's lights, addressed by the name the scene gave them. An unknown name is a script error naming what was
// asked for, the same contract as an unknown mix bus: a typo in a puzzle script should fail in CI, not silently
// leave a lantern dark.
SceneLight* lightOr(lua_State* L, World* w, int arg) {
    const char* name = luaL_checkstring(L, arg);
    SceneLight* l = w->findLight(name);
    if (!l) luaL_error(L, "no scene light named \"%s\"", name);
    return l;
}

// ukiyo.setLightEnabled(name, enabled) -> true. The renderer sees it on the next frame.
int l_setLightEnabled(lua_State* L) {
    World* w = worldOf(L);
    SceneLight* l = lightOr(L, w, 1);
    l->enabled = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, 1);
    return 1;
}

// ukiyo.lightEnabled(name) -> boolean.
int l_lightEnabled(lua_State* L) {
    SceneLight* l = lightOr(L, worldOf(L), 1);
    lua_pushboolean(L, l->enabled ? 1 : 0);
    return 1;
}

// ukiyo.setLightIntensity(name, intensity) -> true. What a warm-up or a flicker animates; the scene's own range
// (0..64 after the scene reader's clamp) is not re-clamped here, so a script can dim a light to nothing.
int l_setLightIntensity(lua_State* L) {
    SceneLight* l = lightOr(L, worldOf(L), 1);
    l->intensity = float(luaL_checknumber(L, 2));
    lua_pushboolean(L, 1);
    return 1;
}

// Save slots. The store lives in the world; the files live in the directory whoever owns the project set. A boolean,
// an integral number, a fractional number and a string each take their own SaveGame type — a Lua number with no
// fractional part is an Int, which is what a counter wants.
int l_saveSet(lua_State* L) {
    World* w = worldOf(L);
    const char* key = luaL_checkstring(L, 1);
    switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
        w->saveStore().setBool(key, lua_toboolean(L, 2) != 0);
        break;
    case LUA_TNUMBER: {
        const double v = lua_tonumber(L, 2);
        if (v == std::floor(v) && v >= -9.2233720368547758e18 && v <= 9.2233720368547758e18)
            w->saveStore().setInt(key, static_cast<int64_t>(v));
        else
            w->saveStore().setFloat(key, v);
        break;
    }
    case LUA_TSTRING:
        w->saveStore().setString(key, lua_tostring(L, 2));
        break;
    default:
        return luaL_error(L, "saveSet: value must be a boolean, number or string, got %s", luaL_typename(L, 2));
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ukiyo.saveGet(key) -> value or nil. Nil covers missing keys and a different type than the one written, which is
// the store's own getters' contract.
int l_saveGet(lua_State* L) {
    World* w = worldOf(L);
    const char* key = luaL_checkstring(L, 1);
    SaveGame& s = w->saveStore();
    SaveGame::Type t;
    if (!s.typeOf(key, &t)) {
        lua_pushnil(L);
        return 1;
    }
    switch (t) {
    case SaveGame::Type::Int: {
        int64_t v = 0;
        s.getInt(key, &v);
        lua_pushinteger(L, static_cast<lua_Integer>(v));
        break;
    }
    case SaveGame::Type::Float: {
        double v = 0.0;
        s.getFloat(key, &v);
        lua_pushnumber(L, v);
        break;
    }
    case SaveGame::Type::Bool: {
        bool v = false;
        s.getBool(key, &v);
        lua_pushboolean(L, v ? 1 : 0);
        break;
    }
    case SaveGame::Type::String: {
        std::string v;
        s.getString(key, &v);
        lua_pushlstring(L, v.data(), v.size());
        break;
    }
    case SaveGame::Type::Vec3:
        // A Vec3 is C++-side only; scripts store positions as three numbers or a string.
        lua_pushnil(L);
        break;
    }
    return 1;
}

// ukiyo.saveHas(key) -> boolean.
int l_saveHas(lua_State* L) {
    lua_pushboolean(L, worldOf(L)->saveStore().has(luaL_checkstring(L, 1)) ? 1 : 0);
    return 1;
}

// The path of a slot file. Slots are 0..63 — enough for any menu, few enough that a script cannot walk the disk.
std::string slotPath(const World* w, lua_State* L, int arg) {
    const lua_Integer slot = luaL_checkinteger(L, arg);
    if (slot < 0 || slot > 63) luaL_error(L, "save slot %lld is out of range (0..63)", static_cast<long long>(slot));
    if (w->saveDir().empty()) luaL_error(L, "this world has no save directory");
    return w->saveDir() + "/slot" + std::to_string(static_cast<long long>(slot)) + ".uksv";
}

// ukiyo.saveWrite(slot) -> boolean. Creates the directory on first use.
int l_saveWrite(lua_State* L) {
    World* w = worldOf(L);
    const std::string path = slotPath(w, L, 1);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    lua_pushboolean(L, w->saveStore().saveToFile(path) ? 1 : 0);
    return 1;
}

// ukiyo.saveRead(slot) -> boolean. A failed read leaves the store exactly as it was (SaveGame's own contract), so a
// corrupt file cannot half-load a run.
int l_saveRead(lua_State* L) {
    World* w = worldOf(L);
    lua_pushboolean(L, w->saveStore().loadFromFile(slotPath(w, L, 1)) ? 1 : 0);
    return 1;
}

// ukiyo.saveSlotExists(slot) -> boolean. For a title screen's Continue row: opens the file, never loads it.
int l_saveSlotExists(lua_State* L) {
    World* w = worldOf(L);
    if (w->saveDir().empty()) luaL_error(L, "this world has no save directory");
    const std::string path = slotPath(w, L, 1);
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// ukiyo.saveDelete(slot) -> boolean: true when a file was removed, false when there was none.
int l_saveDelete(lua_State* L) {
    World* w = worldOf(L);
    const std::string path = slotPath(w, L, 1);
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    std::filesystem::remove(path, ec);
    lua_pushboolean(L, existed && !ec ? 1 : 0);
    return 1;
}

// ukiyo.hudRect(x, y, w, h, r?, g?, b?, a?) — one solid quad in this frame's HUD, pixels, origin top-left, colours
// 0..1. The touch controls' ring, a prompt plate, a joystick under the thumb: drawn after the player's own HUD.
int l_hudRect(lua_State* L) {
    World::HudQuad q;
    q.x = float(luaL_checknumber(L, 1));
    q.y = float(luaL_checknumber(L, 2));
    q.w = float(luaL_checknumber(L, 3));
    q.h = float(luaL_checknumber(L, 4));
    q.r = float(luaL_optnumber(L, 5, 1.0));
    q.g = float(luaL_optnumber(L, 6, 1.0));
    q.b = float(luaL_optnumber(L, 7, 1.0));
    q.a = float(luaL_optnumber(L, 8, 1.0));
    worldOf(L)->addHudQuad(q);
    return 0;
}

// ukiyo.stopMusic([fadeOutSeconds])
int l_stopMusic(lua_State* L) {
    worldOf(L)->audio.stopMusic(float(luaL_optnumber(L, 1, 0.0)));
    return 0;
}

// ukiyo.setBusVolume(bus, volume). `master` is the engine's own volume; the other three are the mixer's buses.
int l_setBusVolume(lua_State* L) {
    World* w = worldOf(L);
    const char* bus = luaL_checkstring(L, 1);
    const float v = float(luaL_checknumber(L, 2));
    AudioEngine::Bus b;
    if (std::strcmp(bus, "master") == 0) w->audio.setMasterVolume(v);
    else if (busFromName(bus, &b)) w->audio.setBusVolume(b, v);
    else return luaL_error(L, "setBusVolume: unknown bus \"%s\" (master, music, sfx, ambient)", bus);
    return 0;
}

// ukiyo.setBusReverb(bus, wet). Wet is how much of the bus runs through the reverb insert, 0..1.
int l_setBusReverb(lua_State* L) {
    World* w = worldOf(L);
    const char* bus = luaL_checkstring(L, 1);
    const float wet = float(luaL_checknumber(L, 2));
    AudioEngine::Bus b;
    if (!busFromName(bus, &b))
        return luaL_error(L, "setBusReverb: unknown bus \"%s\" (music, sfx, ambient)", bus);
    w->audio.setBusReverb(b, wet);
    return 0;
}

// ukiyo.spawnEffect(x, y, z, count) -> handle. Emits a one-shot burst of particles.
int l_spawnEffect(lua_State* L) {
    World* w = worldOf(L);
    EmitterSettings s;
    s.loop = false;
    const glm::vec3 p = readVec3(L, 1);
    const VfxSystem::Handle h = w->vfx.spawn(s, p);
    w->vfx.burst(h, int(luaL_optinteger(L, 4, 24)));
    lua_pushinteger(L, lua_Integer(h));
    return 1;
}

// ukiyo.raycast(ox, oy, oz, dx, dy, dz, maxDistance) -> hit, x, y, z
int l_raycast(lua_State* L) {
    World* w = worldOf(L);
    glm::vec3 hit(0.0f);
    const bool ok = w->physics.raycast(readVec3(L, 1), readVec3(L, 4), float(luaL_checknumber(L, 7)), &hit);
    lua_pushboolean(L, ok);
    lua_pushnumber(L, hit.x);
    lua_pushnumber(L, hit.y);
    lua_pushnumber(L, hit.z);
    return 4;
}

// --- ukiyo.Vector3 and ukiyo.Quaternion: value types for scripts ----------------------------
// Both are userdata with read-only fields (x, y, z, and w for quaternions). Quaternion * Vector3 rotates the
// vector, Quaternion * Quaternion composes rotations, and Vector3 adds, subtracts, and scales by a number.
// Rotations match the engine: a quaternion turns +Z into the direction it looks at.
constexpr const char* kVec3Meta = "ukiyo.Vector3";
constexpr const char* kQuatMeta = "ukiyo.Quaternion";

void pushVec3(lua_State* L, const glm::vec3& v) {
    *static_cast<glm::vec3*>(lua_newuserdatauv(L, sizeof(glm::vec3), 0)) = v;
    luaL_setmetatable(L, kVec3Meta);
}

void pushQuat(lua_State* L, const glm::quat& q) {
    *static_cast<glm::quat*>(lua_newuserdatauv(L, sizeof(glm::quat), 0)) = q;
    luaL_setmetatable(L, kQuatMeta);
}

glm::vec3* testVec3(lua_State* L, int i) { return static_cast<glm::vec3*>(luaL_testudata(L, i, kVec3Meta)); }
glm::quat* testQuat(lua_State* L, int i) { return static_cast<glm::quat*>(luaL_testudata(L, i, kQuatMeta)); }

// ukiyo.Vector3(x, y, z)
int vec3New(lua_State* L) {
    pushVec3(L, readVec3(L, 1));
    return 1;
}

int vec3Index(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    const glm::vec3* v = testVec3(L, 1);
    const char field = key[0] && !key[1] ? key[0] : '\0';
    if (!v || (field != 'x' && field != 'y' && field != 'z')) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, field == 'x' ? v->x : field == 'y' ? v->y : v->z);
    return 1;
}

int quatIndex(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    const glm::quat* q = testQuat(L, 1);
    const char field = key[0] && !key[1] ? key[0] : '\0';
    if (!q || (field != 'x' && field != 'y' && field != 'z' && field != 'w')) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, field == 'x' ? q->x : field == 'y' ? q->y : field == 'z' ? q->z : q->w);
    return 1;
}

// Shared by __add and __sub. `sign` is +1 or -1.
int vec3Combine(lua_State* L, float sign) {
    const glm::vec3* a = testVec3(L, 1);
    const glm::vec3* b = testVec3(L, 2);
    if (!a || !b) return luaL_error(L, "ukiyo: Vector3 arithmetic needs two Vector3 values");
    pushVec3(L, *a + sign * *b);
    return 1;
}

int vec3Add(lua_State* L) { return vec3Combine(L, 1.0f); }
int vec3Sub(lua_State* L) { return vec3Combine(L, -1.0f); }

// __mul for both types: Quaternion * Vector3, Quaternion * Quaternion, and Vector3 scaled by a number.
int mulMeta(lua_State* L) {
    const glm::quat* q1 = testQuat(L, 1);
    const glm::quat* q2 = testQuat(L, 2);
    const glm::vec3* v1 = testVec3(L, 1);
    const glm::vec3* v2 = testVec3(L, 2);
    if (q1 && v2) pushVec3(L, *q1 * *v2);
    else if (q1 && q2) pushQuat(L, *q1 * *q2);
    else if (v1 && lua_isnumber(L, 2)) pushVec3(L, *v1 * float(lua_tonumber(L, 2)));
    else if (v2 && lua_isnumber(L, 1)) pushVec3(L, *v2 * float(lua_tonumber(L, 1)));
    else return luaL_error(L, "ukiyo: unsupported operands for *");
    return 1;
}

int vec3ToString(lua_State* L) {
    const glm::vec3* v = testVec3(L, 1);
    if (!v) return luaL_error(L, "ukiyo: not a Vector3");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%g, %g, %g)", double(v->x), double(v->y), double(v->z));
    lua_pushstring(L, buf);
    return 1;
}

int quatToString(lua_State* L) {
    const glm::quat* q = testQuat(L, 1);
    if (!q) return luaL_error(L, "ukiyo: not a Quaternion");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%g, %g, %g, %g)", double(q->x), double(q->y), double(q->z), double(q->w));
    lua_pushstring(L, buf);
    return 1;
}

// Reads a Vector3 argument, or raises a Lua argument error.
glm::vec3 argVec3(lua_State* L, int i) {
    const glm::vec3* v = testVec3(L, i);
    if (!v) luaL_argerror(L, i, "Vector3 expected");
    return *v;
}

// ukiyo.Quaternion.lookRotation(forward, up) -> Quaternion. up defaults to (0, 1, 0). A zero forward gives identity.
int quatLookRotation(lua_State* L) {
    const glm::vec3 forward = argVec3(L, 1);
    const glm::vec3 up = lua_isnoneornil(L, 2) ? glm::vec3(0.0f, 1.0f, 0.0f) : argVec3(L, 2);
    const float fl = glm::length(forward);
    const glm::vec3 right = glm::cross(up, forward);
    if (fl < 1e-6f || glm::length(right) < 1e-6f) {
        pushQuat(L, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        return 1;
    }
    const glm::vec3 z = forward / fl;
    const glm::vec3 x = glm::normalize(right);
    const glm::vec3 y = glm::cross(z, x);
    pushQuat(L, glm::quat_cast(glm::mat3(x, y, z)));
    return 1;
}

// ukiyo.Quaternion.angleAxis(degrees, axis) -> Quaternion. angleAxis(yaw, Vector3(0, 1, 0)) is a yaw turn.
int quatAngleAxis(lua_State* L) {
    const float degrees = float(luaL_checknumber(L, 1));
    glm::vec3 axis = argVec3(L, 2);
    const float len = glm::length(axis);
    axis = len > 1e-6f ? axis / len : glm::vec3(0.0f, 1.0f, 0.0f);
    pushQuat(L, glm::angleAxis(glm::radians(degrees), axis));
    return 1;
}

// ukiyo.Entity(name): a handle to a named entity. Fields: name (read-only), position (Vector3, writable),
// yaw (degrees, writable), body (physics handle, read-only). Methods: impulse, setParam, moveTo.
constexpr const char* kEntityMeta = "ukiyo.Entity";

struct EntityRef {
    World* world;
    std::string name;
};

EntityRef* checkEntity(lua_State* L, int i) {
    auto* r = static_cast<EntityRef*>(luaL_testudata(L, i, kEntityMeta));
    if (!r) luaL_argerror(L, i, "Entity expected");
    return r;
}

void pushEntity(lua_State* L, World* w, const std::string& name) {
    void* mem = lua_newuserdatauv(L, sizeof(EntityRef), 0);
    new (mem) EntityRef{w, name};
    luaL_setmetatable(L, kEntityMeta);
}

// ukiyo.Entity(name) -> Entity. Creates the entity on first use, like the other name-based calls.
int l_entityNew(lua_State* L) {
    World* w = worldOf(L);
    const std::string name = luaL_checkstring(L, 1);
    w->entity(name);
    pushEntity(L, w, name);
    return 1;
}

int entityGc(lua_State* L) {
    static_cast<EntityRef*>(lua_touserdata(L, 1))->~EntityRef();
    return 0;
}

int entityEq(lua_State* L) {
    const EntityRef* a = checkEntity(L, 1);
    const EntityRef* b = checkEntity(L, 2);
    lua_pushboolean(L, a->world == b->world && a->name == b->name);
    return 1;
}

int entityToString(lua_State* L) {
    const EntityRef* r = checkEntity(L, 1);
    lua_pushstring(L, ("Entity(" + r->name + ")").c_str());
    return 1;
}

int entityIndex(lua_State* L) {
    EntityRef* r = checkEntity(L, 1);
    const std::string key = luaL_checkstring(L, 2);
    Entity* e = r->world->findEntity(r->name);
    if (key == "name") lua_pushstring(L, r->name.c_str());
    else if (key == "position") pushVec3(L, e ? e->position : glm::vec3(0.0f));
    else if (key == "yaw") lua_pushnumber(L, e ? e->rotationDeg.y : 0.0);
    else if (key == "rotation") pushVec3(L, e ? e->rotationDeg : glm::vec3(0.0f));
    else if (key == "body") lua_pushinteger(L, e ? static_cast<lua_Integer>(e->body) : 0);
    else if (key == "impulse") lua_pushcfunction(L, [](lua_State* L2) -> int {
        EntityRef* r2 = checkEntity(L2, 1);
        Entity* e2 = r2->world->findEntity(r2->name);
        if (e2 && e2->body != PhysicsWorld::kNone) r2->world->physics.addImpulse(e2->body, readVec3(L2, 2));
        return 0;
    });
    else if (key == "setParam") lua_pushcfunction(L, [](lua_State* L2) -> int {
        EntityRef* r2 = checkEntity(L2, 1);
        r2->world->setParam(r2->name, luaL_checkstring(L2, 2), float(luaL_checknumber(L2, 3)));
        return 0;
    });
    else if (key == "moveTo") lua_pushcfunction(L, [](lua_State* L2) -> int {
        EntityRef* r2 = checkEntity(L2, 1);
        Entity* e2 = r2->world->findEntity(r2->name);
        const bool ok = e2 && e2->agent >= 0 && r2->world->nav.moveAgent(e2->agent, readVec3(L2, 2));
        lua_pushboolean(L2, ok);
        return 1;
    });
    else return luaL_error(L, "ukiyo.Entity has no field '%s'", key.c_str());
    return 1;
}

int entityNewIndex(lua_State* L) {
    EntityRef* r = checkEntity(L, 1);
    const std::string key = luaL_checkstring(L, 2);
    if (key == "position") {
        const glm::vec3* v = testVec3(L, 3);
        if (!v) return luaL_error(L, "ukiyo.Entity.position needs a Vector3");
        r->world->setEntityPosition(r->name, *v);
    } else if (key == "yaw") {
        Entity& e = r->world->entity(r->name);
        e.rotationDeg.y = static_cast<float>(luaL_checknumber(L, 3));
    } else if (key == "rotation") {
        const glm::vec3* v = testVec3(L, 3);
        if (!v) return luaL_error(L, "ukiyo.Entity.rotation needs a Vector3");
        Entity& e = r->world->entity(r->name);
        e.rotationDeg = *v;
        // A body follows the entity, or the prop would be drawn turned and still collide straight.
        if (e.body != PhysicsWorld::kNone && r->world->physics.isDynamic(e.body))
            r->world->physics.setRotation(e.body, *v);
    } else if (key == "name" || key == "body") return luaL_error(L, "ukiyo.Entity.%s is read-only", key.c_str());
    else return luaL_error(L, "ukiyo.Entity has no writable field '%s'", key.c_str());
    return 0;
}

// ukiyo.setContactFilter(a, b, enabled): enabled = false stops new contacts between two entities. Both need physics
// bodies. A pair that already touches separates only when it moves apart.
int l_setContactFilter(lua_State* L) {
    World* w = worldOf(L);
    Entity* a = w->findEntity(luaL_checkstring(L, 1));
    Entity* b = w->findEntity(luaL_checkstring(L, 2));
    if (!a || !b || a->body == PhysicsWorld::kNone || b->body == PhysicsWorld::kNone)
        return luaL_error(L, "ukiyo.setContactFilter needs two entities with physics bodies");
    const bool enabled = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    w->physics.setPairEnabled(a->body, b->body, enabled);
    return 0;
}

void setField(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

// Called after ScriptHost has created the ukiyo table (its first registerFunction call).
void installMath(lua_State* L) {
    luaL_newmetatable(L, kEntityMeta);
    setField(L, "__index", entityIndex);
    setField(L, "__newindex", entityNewIndex);
    setField(L, "__eq", entityEq);
    setField(L, "__tostring", entityToString);
    setField(L, "__gc", entityGc);
    lua_pop(L, 1);

    luaL_newmetatable(L, kVec3Meta);
    setField(L, "__index", vec3Index);
    setField(L, "__add", vec3Add);
    setField(L, "__sub", vec3Sub);
    setField(L, "__mul", mulMeta);
    setField(L, "__tostring", vec3ToString);
    lua_pop(L, 1);

    luaL_newmetatable(L, kQuatMeta);
    setField(L, "__index", quatIndex);
    setField(L, "__mul", mulMeta);
    setField(L, "__tostring", quatToString);
    lua_pop(L, 1);

    lua_getglobal(L, "ukiyo");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    setField(L, "Vector3", vec3New);
    lua_newtable(L);
    setField(L, "lookRotation", quatLookRotation);
    setField(L, "angleAxis", quatAngleAxis);
    lua_setfield(L, -2, "Quaternion");
    lua_pop(L, 1);
}

} // namespace

World::World(bool audioNoDevice) : audio(audioNoDevice) {
    // The devices exist before any action map, so a project with no actions yet still accepts key events (the editor
    // forwards every key it sees) and a gamepad can be hot-plugged into the same state later.
    input_.addDevice(uinput::DeviceKind::Keyboard, 0);
    input_.addDevice(uinput::DeviceKind::Mouse, 0);
    // Every script the runtime creates is given the same API. The list is here, once, so a script added mid-scene and
    // one from the level's first frame see the same table; installMath adds Vector3, Quaternion and the metatables.
    scripts.onHostCreate([this](ScriptHost& host) {
        for (const ScriptApiBinding& b : apiBindings()) host.registerFunction(b.name, b.fn, this);
        installMath(host.state());
    });
}

const std::vector<ScriptApiBinding>& World::apiBindings() {
    static const std::vector<ScriptApiBinding> list{
        {"log", l_log},
        {"time", l_time},
        {"actionDown", l_actionDown},
        {"hud", l_hud},
        {"tr", l_tr},
        {"locale", l_locale},
        {"joinPlayer", l_joinPlayer},
        {"playerCount", l_playerCount},
        {"leavePlayer", l_leavePlayer},
        {"playerActionDown", l_playerActionDown},
        {"playerActionValue", l_playerActionValue},
        {"playerActionVector2", l_playerActionVector2},
        {"getPosition", l_getPosition},
        {"setPosition", l_setPosition},
        {"moveTo", l_moveTo},
        {"impulse", l_impulse},
        {"setParam", l_setParam},
        {"playSound", l_playSound},
        {"playMusic", l_playMusic},
        {"stopMusic", l_stopMusic},
        {"setBusVolume", l_setBusVolume},
        {"setBusReverb", l_setBusReverb},
        {"spawnEffect", l_spawnEffect},
        {"raycast", l_raycast},
        {"loadScene", l_loadScene},
        {"holdLoad", l_holdLoad},
        {"releaseLoad", l_releaseLoad},
        {"preloadModel", l_preloadModel},
        {"setContactFilter", l_setContactFilter},
        {"frameTiming", l_frameTiming},
        {"setLightEnabled", l_setLightEnabled},
        {"lightEnabled", l_lightEnabled},
        {"setLightIntensity", l_setLightIntensity},
        {"saveSet", l_saveSet},
        {"saveGet", l_saveGet},
        {"saveHas", l_saveHas},
        {"saveWrite", l_saveWrite},
        {"saveRead", l_saveRead},
        {"saveSlotExists", l_saveSlotExists},
        {"saveDelete", l_saveDelete},
        {"hudRect", l_hudRect},
        {"Entity", l_entityNew},
    };
    return list;
}

World::~World() = default;

Entity& World::entity(const std::string& name) {
    auto it = entities_.find(name);
    if (it != entities_.end()) return it->second;
    Entity& e = entities_[name];
    e.name = name;
    return e;
}

Entity* World::findEntity(const std::string& name) {
    auto it = entities_.find(name);
    return it == entities_.end() ? nullptr : &it->second;
}

void World::attachDynamicSphere(const std::string& name, float radius, float mass) {
    Entity& e = entity(name);
    if (e.body != PhysicsWorld::kNone) physics.removeBody(e.body);
    e.body = physics.addDynamicSphere(e.position, radius, mass);
}

void World::attachDynamicBox(const std::string& name, const glm::vec3& halfExtents, float mass,
                             const glm::vec3& rotationDeg) {
    Entity& e = entity(name);
    if (e.body != PhysicsWorld::kNone) physics.removeBody(e.body);
    e.body = physics.addDynamicBox(e.position, halfExtents, mass, rotationDeg);
}

void World::attachAgent(const std::string& name, float maxSpeed) {
    Entity& e = entity(name);
    if (e.agent >= 0) nav.removeAgent(e.agent);
    e.agent = nav.addAgent(e.position, maxSpeed);
}

bool World::attachCharacter(const std::string& name, const CharacterSettings& settings) {
    Entity& e = entity(name);
    e.character = physics.addCharacter(e.position, settings);
    return e.character != 0;
}

void World::setCharacterVelocity(const std::string& name, const glm::vec3& planarVelocity, bool jump) {
    Entity* e = findEntity(name);
    if (!e || e->character == 0) return;
    e->characterVelocity = planarVelocity;
    e->characterJump = e->characterJump || jump;
}

bool World::characterState(const std::string& name, CharacterState* out) const {
    const auto it = entities_.find(name);
    if (it == entities_.end() || it->second.character == 0) return false;
    return physics.characterState(it->second.character, out);
}

void World::setEntityPosition(const std::string& name, const glm::vec3& p) {
    Entity& e = entity(name);
    e.position = p;
    if (e.body != PhysicsWorld::kNone) physics.setPosition(e.body, p);
    if (e.agent >= 0) {
        nav.removeAgent(e.agent);
        e.agent = nav.addAgent(p);
    }
}

void World::setParam(const std::string& name, const std::string& param, float value) {
    Entity& e = entity(name);
    if (!e.anim) return;
    e.anim->setFloat(param, value);
}

bool World::loadScriptFor(const std::string& entity, const std::string& path, std::string* error) {
    if (!scripts.add(entity, path, error)) return false;
    ScriptHost* host = scripts.find(entity);
    if (!host) return false;
    host->callStart();
    return !host->hasFailed();
}

bool World::loadScript(const std::string& path, std::string* error) { return loadScriptFor(std::string(), path, error); }

ScriptHost& World::sceneScript() { return scripts.ensure(std::string()); }

std::string World::takeSceneRequest() {
    std::string path = scenePath_;
    scenePath_.clear();
    sceneRequested_ = false;
    return path;
}

int World::holdLoad() {
    const int token = nextHold_++;
    loadHolds_.insert(token);
    return token;
}

bool World::releaseLoad(int token) {
    return loadHolds_.erase(token) > 0;
}

std::vector<std::string> World::takePreloads() {
    std::vector<std::string> out;
    out.swap(preloads_);
    return out;
}

namespace {
bool fileExists(const std::string& p) {
    if (std::FILE* f = std::fopen(p.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}
} // namespace

std::string World::resolveMediaPath(const std::string& path) const {
    if (path.empty()) return path;
    const bool absolute = path.front() == '/' || path.front() == '\\' || (path.size() > 2 && path[1] == ':');
    if (absolute || projectRoot_.empty()) return path;
    const std::string joined = projectRoot_ + "/" + path;
    // The project-root candidate wins only when it exists; otherwise the path as written is what load() will report,
    // which is the string the script author recognises.
    return fileExists(joined) ? joined : path;
}

SceneLight* World::findLight(const std::string& name) {
    for (SceneLight& l : sceneLights_)
        if (l.name == name) return &l;
    return nullptr;
}

AudioEngine::SoundId World::soundFor(const std::string& path) {
    auto it = sounds_.find(path);
    if (it != sounds_.end()) return it->second;
    const AudioEngine::SoundId id = audio.load(resolveMediaPath(path));
    if (id) sounds_[path] = id;
    return id;
}

void World::updateCharacters(float dt) {
    for (auto& [name, e] : entities_) {
        if (e.character == 0) continue;
        CharacterState st;
        if (!physics.moveCharacter(e.character, e.characterVelocity, e.characterJump, dt, &st)) continue;
        e.position = st.position;
        e.grounded = st.grounded;
        e.characterJump = false;
        // The capsule turns to face the way it is walking, which is what the animation and the camera read.
        if (glm::length(glm::vec2(e.characterVelocity.x, e.characterVelocity.z)) > 0.1f)
            e.rotationDeg.y = glm::degrees(std::atan2(e.characterVelocity.x, e.characterVelocity.z));
        physics.setCharacterYaw(e.character, glm::radians(e.rotationDeg.y));
    }
}

void World::syncEntities() {
    for (auto& [name, e] : entities_) {
        // A dynamic body owns the whole transform: a box that lands on a tilted plank is drawn tilted, which is the
        // point of letting X and Z turn. A static body never moves, so its rotation stays as authored.
        if (e.body != PhysicsWorld::kNone) {
            e.position = physics.position(e.body);
            if (physics.isDynamic(e.body)) e.rotationDeg = physics.rotationDeg(e.body);
        }
        else if (e.agent >= 0) e.position = nav.agentPosition(e.agent);
    }
}

void World::updateAnimation(float dt) {
    for (auto& [name, e] : entities_) {
        if (!e.anim) continue;
        const std::string entityName = name;
        e.anim->update(dt,
            [this, entityName](const std::string& clip, float fade, float startFraction) {
                Entity* ent = findEntity(entityName);
                if (ent) ent->clip = clip;
                if (onClipChange) onClipChange(entityName, clip, fade, startFraction);
            },
            [this, entityName](const std::vector<std::string>& clips, const std::vector<float>& weights,
                               const std::vector<MotionOptions>& options, float fade) {
                if (onBlendChange) onBlendChange(entityName, clips, weights, options, fade);
            });
    }
}

void World::updateListener() {
    Entity* l = findEntity("listener");
    if (!l) return;
    const float yaw = glm::radians(l->rotationDeg.y);
    const glm::vec3 forward(std::sin(yaw), 0.0f, std::cos(yaw));
    audio.setListener(l->position, forward);
}

void World::setInputActions(const std::vector<engine::InputAction>& actions) {
    actions_ = actions;
    input_.cancelRebind();   // its target is one of the actions about to be replaced
    input_.maps().clear();
    input_.controlSchemes.clear();
    uinput::ActionMap& map = input_.addMap("Gameplay");
    for (const engine::InputAction& a : actions_) {
        if (a.name.empty()) continue;
        uinput::Action& action = map.add(a.name, uinput::ActionType::Button);
        // A binding is a control path, which names its device; a bare key is the keyboard's path, which is what a
        // project written before paths holds. A control the engine does not know keeps its action but can never fire:
        // setKeyHeld() cannot produce it either, so the two stay consistent instead of one silently working.
        std::string path = a.key;
        if (path.empty()) continue;
        if (path.front() != '<') {
            std::string control;
            if (!uinput::controlNameFromPortableKey(path, &control)) continue;
            path = "<Keyboard>/" + control;
        }
        uinput::DeviceKind kind;
        if (!uinput::deviceKindFromPath(path, &kind)) continue;
        const auto& known = kind == uinput::DeviceKind::Keyboard ? uinput::keyboardControls()
                           : kind == uinput::DeviceKind::Mouse    ? uinput::mouseControls()
                           : kind == uinput::DeviceKind::Gamepad  ? uinput::gamepadControls()
                                                                  : uinput::touchControls();
        // After the first slash, not the last: a control name may contain slashes ("primaryTouch/press",
        // "dpad/up"), and cutting at the last one would look up "press" and silently drop the binding.
        const std::string control = path.substr(path.find('/') + 1);
        if (std::find(known.begin(), known.end(), control) == known.end()) continue;
        uinput::Binding binding;
        binding.path = path;
        binding.action = a.name;
        binding.name = control;
        action.bindings.push_back(std::move(binding));
    }
    map.enabled = true;
    input_.resolveBindings();
    // A player holds a copy of the maps, so a project that reloads its actions mid-game has to hand the new ones over.
    if (players_) players_->refreshMaps();
}

void World::setKeyHeld(const std::string& key, bool down) {
    std::string control;
    if (!uinput::controlNameFromPortableKey(key, &control)) return;
    uinput::Device* keyboard = input_.firstDevice(uinput::DeviceKind::Keyboard);
    if (!keyboard) keyboard = &input_.addDevice(uinput::DeviceKind::Keyboard, 0);
    keyboard->setButton(control, down);
    // Key events arrive between ticks, and a script (or the editor's self-test) can ask actionDown() before the next
    // tick. Evaluating now with dt 0 makes the press visible immediately; the next tick's update re-latches it.
    input_.update(0.0f);
}

void World::setTouch(int finger, bool down, const glm::vec2& position, const glm::vec2& delta) {
    const char* prefix = finger == 0 ? "primaryTouch" : finger == 1 ? "secondaryTouch" : nullptr;
    if (!prefix) return;
    uinput::Device* touch = input_.firstDevice(uinput::DeviceKind::Touch);
    if (!touch) touch = &input_.addDevice(uinput::DeviceKind::Touch, 0);
    const std::string base = prefix;
    touch->setButton(base + "/press", down);
    touch->setVector2(base + "/position", position);
    // The delta of the latest event since the previous update: several moves between two ticks leave the last one,
    // which is what a camera drag reads. On release the delta is zero - a lift has no direction.
    touch->setVector2(base + "/delta", down ? delta : glm::vec2(0.0f));
    const float count = (touch->find("primaryTouch/press")->x >= 0.5f ? 1.0f : 0.0f) +
                        (touch->find("secondaryTouch/press")->x >= 0.5f ? 1.0f : 0.0f);
    touch->setAxis("touchCount", count);
    // Same reason as setKeyHeld: a script can ask between ticks.
    input_.update(0.0f);
}

ukiyo::input::InputSystem& World::inputPlayers() {
    if (!players_) {
        players_ = std::make_unique<ukiyo::input::InputSystem>(&input_);
        players_->setAutoUpdateMaster(false);   // tick() updates the master first, once
        players_->setDefaultActionMap("Gameplay");
    }
    return *players_;
}

bool World::actionDown(const std::string& action) const {
    const uinput::Action* a = input_.action("Gameplay", action);
    return a && a->pressed();
}

float pingPongWeight(float phase, bool ease) {
    const float p = phase - std::floor(phase);
    if (ease) return 0.5f - 0.5f * std::cos(6.2831853f * p);
    return 1.0f - std::fabs(2.0f * p - 1.0f);
}

void World::setAction(const std::string& name, const EntityAction& a, const glm::vec3& rest, float restYaw) {
    Entity& e = entity(name);
    e.action = a;
    e.action.rest = rest;
    e.action.restYaw = restYaw;
    e.action.t = -std::max(0.0f, a.startOffset);
    e.position = rest;
    // A spin turns the yaw and leaves pitch and roll as they were authored: the action is a turntable, not a tumble.
    e.rotationDeg.y = restYaw;
}

void World::updateActions(float dt) {
    for (auto& entry : entities_) {
        Entity& e = entry.second;
        EntityAction& a = e.action;
        if (!a.on || e.body != PhysicsWorld::kNone) continue;   // physics owns bodies
        a.t += dt;
        if (a.t < 0.0f) continue;   // still waiting for the start offset; stays at rest
        if (a.pingPong) {
            glm::vec3 axis = a.axis;
            const float len = glm::length(axis);
            axis = len > 1e-6f ? axis / len : glm::vec3(0.0f, 1.0f, 0.0f);
            e.position = a.rest + axis * (a.amplitude * pingPongWeight(a.t * a.speed, a.ease));
        } else {
            e.rotationDeg.y = a.restYaw + a.speed * a.t;
        }
    }
}

void World::dispatchContacts() {
    std::vector<PhysicsWorld::Contact> contacts = physics.takeContacts();
    if (contacts.empty()) return;
    // Every script that defines onContact hears every contact, in the runtime's order. A script that has spent its
    // error budget is skipped by entitiesWith, so one broken script does not stop the others from hearing them.
    const std::vector<std::string> handlers = scripts.entitiesWith("onContact");
    if (handlers.empty()) return;
    // The entity that owns a body, by name. Bodies that are not entities give "".
    auto nameOf = [this](PhysicsWorld::Handle h) -> std::string {
        if (h != PhysicsWorld::kNone)
            for (const auto& [name, e] : entities_)
                if (e.body == h) return name;
        return std::string();
    };
    physics.setDeferChanges(true);
    for (const PhysicsWorld::Contact& c : contacts) {
        for (const std::string& entity : handlers) {
            ScriptHost* host = scripts.find(entity);
            if (!host || host->hasFailed() || !host->state()) continue;
            lua_State* L = host->state();
            lua_getglobal(L, "onContact");
            lua_newtable(L);
            lua_pushstring(L, c.added ? "added" : "removed");
            lua_setfield(L, -2, "kind");
            lua_pushstring(L, nameOf(c.a).c_str());
            lua_setfield(L, -2, "a");
            lua_pushstring(L, nameOf(c.b).c_str());
            lua_setfield(L, -2, "b");
            pushVec3(L, c.point);
            lua_setfield(L, -2, "point");
            pushVec3(L, c.normal);
            lua_setfield(L, -2, "normal");
            host->callPushed(1);
        }
    }
    physics.setDeferChanges(false);
}

float FrameTiming::share(const char* stage) const {
    if (total <= 0.0f || !stage) return 0.0f;
    const std::string s(stage);
    const float v = s == "input" ? input
                  : s == "scripts" ? scripts
                  : s == "physics" ? physics
                  : s == "characters" ? characters
                  : s == "navigation" ? navigation
                  : s == "animation" ? animation
                  : s == "particles" ? particles
                  : s == "entities" ? entities
                  : 0.0f;
    return v / total;
}

void World::tick(float dt) {
    // Script HUD quads belong to the tick that drew them: cleared here, refilled by onUpdate, read when the frame is
    // built after the tick. A script that stops drawing stops showing without a teardown call.
    hudQuads_.clear();
    // The profiler costs four clock reads per stage and nothing when it is off, which is why it is a switch rather than
    // always-on bookkeeping.
    using Clock = std::chrono::steady_clock;
    const auto mark = [this] {
        return profiling_ ? Clock::now() : Clock::time_point{};
    };
    auto since = [this](const Clock::time_point& a, const Clock::time_point& b) {
        return (profiling_ && a != Clock::time_point{} && b != Clock::time_point{})
                   ? std::chrono::duration<float, std::milli>(b - a).count()
                   : 0.0f;
    };

    auto t0 = mark();
    // Input runs on unscaled time: pausing the game (timeScale 0) must still let a script read a menu key. The
    // master is updated here, so the players only mirror and evaluate.
    input_.update(dt);
    if (players_) players_->update(dt);
    audio.tick();   // reaps finished voices; the music fades themselves are scheduled on the audio thread
    auto t1 = mark();
    dt *= timeScale_;
    scripts.callUpdate(dt);
    auto t2 = mark();
    physics.step(dt);
    auto t3 = mark();
    updateCharacters(dt);
    dispatchContacts();
    auto t4 = mark();
    nav.update(dt);
    auto t5 = mark();
    syncEntities();
    updateActions(dt);
    auto t6 = mark();
    updateAnimation(dt);
    auto t7 = mark();
    vfx.update(dt);
    updateListener();
    auto t8 = mark();
    time_ += dt;

    if (profiling_) {
        timing_.input = since(t0, t1);
        timing_.scripts = since(t1, t2);
        timing_.physics = since(t2, t3);
        timing_.characters = since(t3, t4);
        timing_.navigation = since(t4, t5);
        timing_.entities = since(t5, t6);
        timing_.animation = since(t6, t7);
        timing_.particles = since(t7, t8);
        timing_.total = since(t0, t8);
    }
}

} // namespace ukiyo
