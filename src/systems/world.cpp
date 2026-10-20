#include "systems/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// ukiyo.playSound(path, name) -> bool. Plays at the entity's position when name is given.
int l_playSound(lua_State* L) {
    World* w = worldOf(L);
    const AudioEngine::SoundId id = w->soundFor(luaL_checkstring(L, 1));
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
        ok = w->audio.play(id);
    }
    lua_pushboolean(L, ok);
    return 1;
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
    else if (key == "yaw") lua_pushnumber(L, e ? e->yawDeg : 0.0);
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
        e.yawDeg = static_cast<float>(luaL_checknumber(L, 3));
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
    script.registerFunction("log", l_log, this);
    script.registerFunction("time", l_time, this);
    script.registerFunction("actionDown", l_actionDown, this);
    script.registerFunction("joinPlayer", l_joinPlayer, this);
    script.registerFunction("playerCount", l_playerCount, this);
    script.registerFunction("leavePlayer", l_leavePlayer, this);
    script.registerFunction("playerActionDown", l_playerActionDown, this);
    script.registerFunction("playerActionValue", l_playerActionValue, this);
    script.registerFunction("playerActionVector2", l_playerActionVector2, this);
    script.registerFunction("getPosition", l_getPosition, this);
    script.registerFunction("setPosition", l_setPosition, this);
    script.registerFunction("moveTo", l_moveTo, this);
    script.registerFunction("impulse", l_impulse, this);
    script.registerFunction("setParam", l_setParam, this);
    script.registerFunction("playSound", l_playSound, this);
    script.registerFunction("spawnEffect", l_spawnEffect, this);
    script.registerFunction("raycast", l_raycast, this);
    script.registerFunction("loadScene", l_loadScene, this);
    script.registerFunction("holdLoad", l_holdLoad, this);
    script.registerFunction("releaseLoad", l_releaseLoad, this);
    script.registerFunction("preloadModel", l_preloadModel, this);
    script.registerFunction("setContactFilter", l_setContactFilter, this);
    script.registerFunction("Entity", l_entityNew, this);
    installMath(script.state());
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

void World::attachDynamicBox(const std::string& name, const glm::vec3& halfExtents, float mass) {
    Entity& e = entity(name);
    if (e.body != PhysicsWorld::kNone) physics.removeBody(e.body);
    e.body = physics.addDynamicBox(e.position, halfExtents, mass);
}

void World::attachAgent(const std::string& name, float maxSpeed) {
    Entity& e = entity(name);
    if (e.agent >= 0) nav.removeAgent(e.agent);
    e.agent = nav.addAgent(e.position, maxSpeed);
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

bool World::loadScript(const std::string& path, std::string* error) {
    if (!script.loadFile(path, error)) return false;
    script.callStart();
    return !script.hasFailed();
}

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

AudioEngine::SoundId World::soundFor(const std::string& path) {
    auto it = sounds_.find(path);
    if (it != sounds_.end()) return it->second;
    const AudioEngine::SoundId id = audio.load(path);
    if (id) sounds_[path] = id;
    return id;
}

void World::syncEntities() {
    for (auto& [name, e] : entities_) {
        if (e.body != PhysicsWorld::kNone) e.position = physics.position(e.body);
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
    const float yaw = glm::radians(l->yawDeg);
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
                           : kind == uinput::DeviceKind::Mouse  ? uinput::mouseControls()
                                                                : uinput::gamepadControls();
        const std::string control = path.substr(path.rfind('/') + 1);
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
    e.yawDeg = restYaw;
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
            e.yawDeg = a.restYaw + a.speed * a.t;
        }
    }
}

void World::dispatchContacts() {
    std::vector<PhysicsWorld::Contact> contacts = physics.takeContacts();
    if (contacts.empty() || !script.state() || script.hasFailed() || !script.hasFunction("onContact")) return;
    // The entity that owns a body, by name. Bodies that are not entities give "".
    auto nameOf = [this](PhysicsWorld::Handle h) -> std::string {
        if (h != PhysicsWorld::kNone)
            for (const auto& [name, e] : entities_)
                if (e.body == h) return name;
        return std::string();
    };
    lua_State* L = script.state();
    physics.setDeferChanges(true);
    for (const PhysicsWorld::Contact& c : contacts) {
        if (script.hasFailed()) break;
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
        script.callPushed(1);
    }
    physics.setDeferChanges(false);
}

void World::tick(float dt) {
    // Input runs on unscaled time: pausing the game (timeScale 0) must still let a script read a menu key. The
    // master is updated here, so the players only mirror and evaluate.
    input_.update(dt);
    if (players_) players_->update(dt);
    dt *= timeScale_;
    script.callUpdate(dt);
    physics.step(dt);
    dispatchContacts();
    nav.update(dt);
    syncEntities();
    updateActions(dt);
    updateAnimation(dt);
    vfx.update(dt);
    updateListener();
    time_ += dt;
}

} // namespace ukiyo
