// Headless checks for the game systems: physics, navigation, audio, animation state, scripting, particles,
// and the World that ties them together. Needs no window or GPU.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

// The pak test needs a unique temporary directory name, which is the process id. MSVC spells the call with a leading
// underscore and declares it in <process.h>.
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "engine/cull.h"
#include "engine/foliage.h"
#include "engine/grass.h"
#include "engine/terrain_edit.h"
#include "engine/vfs.h"
#include "engine/vfs_cgltf.h"
#include "engine/terrain.h"
#include "engine/terrain_layers.h"
#include "engine/water.h"

#include "meshoptimizer.h"
#include "systems/anim_state.h"
#include "systems/audio.h"
#include "systems/navigation.h"
#include "systems/physics.h"
#include "systems/script.h"
#include "systems/vfx.h"
#include "systems/world.h"

#include <cstdio>
#include <cstdlib>

#include <glm/gtc/matrix_transform.hpp>

extern "C" {
#include "lua.h"
}

using namespace ukiyo;

static int passed = 0;
static int failed = 0;

static void check(bool ok, const std::string& name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
    std::fflush(stdout);
    ok ? ++passed : ++failed;
}

static std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

// --- Physics ---------------------------------------------------------------------------------

static void testPhysics() {
    PhysicsWorld pw;
    check(pw.gravity().y < -9.0f, "physics: gravity points down at about 9.81 m/s^2", num(pw.gravity().y));

    pw.addStaticBox(glm::vec3(0, -0.5f, 0), glm::vec3(20, 0.5f, 20));   // floor, top at y = 0
    const PhysicsWorld::Handle ball = pw.addDynamicSphere(glm::vec3(0, 5, 0), 0.5f, 1.0f);
    check(pw.isDynamic(ball), "physics: a sphere created dynamic reports dynamic");
    for (int i = 0; i < 180; ++i) pw.step(1.0f / 60.0f);
    const float y = pw.position(ball).y;
    check(std::fabs(y - 0.5f) < 0.05f, "physics: a falling sphere comes to rest on the floor", num(y));

    pw.addImpulse(ball, glm::vec3(4, 0, 0));
    for (int i = 0; i < 60; ++i) pw.step(1.0f / 60.0f);
    check(pw.position(ball).x > 0.5f, "physics: an impulse moves a resting body sideways", num(pw.position(ball).x));

    glm::vec3 hit(0.0f);
    const bool floorHit = pw.raycast(glm::vec3(-15, 10, 15), glm::vec3(0, -1, 0), 20.0f, &hit);
    check(floorHit && std::fabs(hit.y) < 0.05f, "physics: a downward ray hits the floor at y = 0", num(hit.y));

    const std::vector<glm::vec3> quad = {{-2, 5, -2}, {2, 5, -2}, {2, 5, 2}, {-2, 5, 2}};
    const PhysicsWorld::Handle mesh = pw.addStaticMesh(quad, {0, 2, 1, 0, 3, 2});
    check(mesh != PhysicsWorld::kNone, "physics: a triangle mesh body is created");
    glm::vec3 meshHit(0.0f);
    const bool meshRay = pw.raycast(glm::vec3(0, 10, 0), glm::vec3(0, -1, 0), 20.0f, &meshHit);
    check(meshRay && std::fabs(meshHit.y - 5.0f) < 0.05f, "physics: a ray hits a triangle mesh above the floor",
          num(meshHit.y));
}

// Interpolation: a body shows the pose before its last fixed step, then blends toward the step's end.
static void testPhysicsInterpolation() {
    PhysicsWorld on;
    PhysicsWorld off;
    off.setInterpolation(false);
    const PhysicsWorld::Handle a = on.addDynamicSphere(glm::vec3(0, 5, 0), 0.5f, 1.0f);
    const PhysicsWorld::Handle b = off.addDynamicSphere(glm::vec3(0, 5, 0), 0.5f, 1.0f);
    check(on.interpolation() && !off.interpolation(), "physics: interpolation is on by default and can be turned off");

    on.step(1.0f / 60.0f);
    off.step(1.0f / 60.0f);
    check(std::fabs(on.position(a).y - 5.0f) < 1e-4f, "physics: right after a whole step the body shows its start pose",
          num(on.position(a).y));

    on.step(0.5f / 60.0f);      // no new fixed step: halfway between the start pose and the stepped pose
    off.step(0.5f / 60.0f);
    const float stepped = off.position(b).y;
    const float blended = on.position(a).y;
    check(stepped < 4.999f && std::fabs(blended - 0.5f * (5.0f + stepped)) < 1e-4f,
          "physics: between steps the body blends halfway between its poses", num(blended));

    on.setPosition(a, glm::vec3(0, 9, 0));
    check(std::fabs(on.position(a).y - 9.0f) < 1e-4f, "physics: setPosition jumps with no blend", num(on.position(a).y));
}

// --- Lua value types: ukiyo.Vector3 and ukiyo.Quaternion ---------------------------------------

static double luaNumber(ScriptHost& sh, const char* name) {
    lua_getglobal(sh.state(), name);
    const double v = lua_tonumber(sh.state(), -1);
    lua_pop(sh.state(), 1);
    return v;
}

static void testLuaMath() {
    World w(true);
    std::string err;
    const bool loaded = w.script.loadString(
        "local a = ukiyo.Vector3(1, 2, 3)\n"
        "local b = a + ukiyo.Vector3(1, 1, 1) - ukiyo.Vector3(0, 0, 1)\n"
        "sumX, sumY, sumZ = b.x, b.y, b.z\n"
        "scaled = (a * 2).z\n"
        "text = tostring(a)\n"
        "local yaw30 = ukiyo.Quaternion.lookRotation(ukiyo.Vector3(math.sin(math.rad(30)), 0, math.cos(math.rad(30))))\n"
        "local f = yaw30 * ukiyo.Vector3(0, 0, 1)\n"
        "fx, fz = f.x, f.z\n"
        "local r = ukiyo.Quaternion.lookRotation(ukiyo.Vector3(1, 0, 0)) * ukiyo.Vector3(0, 0, 1)\n"
        "rx, ry, rz = r.x, r.y, r.z\n"
        "local turn = ukiyo.Quaternion.angleAxis(90, ukiyo.Vector3(0, 1, 0))\n"
        "local t = (turn * turn) * ukiyo.Vector3(0, 0, 1)\n"
        "tx, tz = t.x, t.z\n"
        "local g = turn * ukiyo.Vector3(0, 0, 1)\n"
        "gx, gz = g.x, g.z\n"
        "badMul = not pcall(function() return a * a end)\n"
        "badAdd = not pcall(function() return a + 1 end)\n",
        "math", &err);
    check(loaded, "lua math: the value-type script loads", err);
    check(std::fabs(luaNumber(w.script, "sumX") - 2.0) < 1e-6 && std::fabs(luaNumber(w.script, "sumZ") - 3.0) < 1e-6,
          "lua math: Vector3 adds and subtracts", num(luaNumber(w.script, "sumZ")));
    check(std::fabs(luaNumber(w.script, "scaled") - 6.0) < 1e-6, "lua math: Vector3 scales by a number");
    lua_getglobal(w.script.state(), "text");
    check(std::string(lua_tostring(w.script.state(), -1)) == "(1, 2, 3)", "lua math: tostring prints the vector");
    lua_pop(w.script.state(), 1);
    check(std::fabs(luaNumber(w.script, "fx") - 0.5) < 1e-4 && std::fabs(luaNumber(w.script, "fz") - 0.8660) < 1e-4,
          "lua math: lookRotation with a 30 degree yaw matches the engine's yaw direction",
          num(luaNumber(w.script, "fx")) + " " + num(luaNumber(w.script, "fz")));
    check(std::fabs(luaNumber(w.script, "rx") - 1.0) < 1e-4 && std::fabs(luaNumber(w.script, "rz")) < 1e-4,
          "lua math: lookRotation toward +X turns +Z to +X", num(luaNumber(w.script, "rx")));
    check(std::fabs(luaNumber(w.script, "tz") + 1.0) < 1e-4 && std::fabs(luaNumber(w.script, "tx")) < 1e-4,
          "lua math: two quaternions multiply, 180 degrees turns +Z to -Z");
    check(std::fabs(luaNumber(w.script, "gx") - 1.0) < 1e-4 && std::fabs(luaNumber(w.script, "gz")) < 1e-4,
          "lua math: angleAxis 90 degrees about +Y turns +Z to +X");
    lua_getglobal(w.script.state(), "badMul");
    const bool badMul = lua_toboolean(w.script.state(), -1) != 0;
    lua_pop(w.script.state(), 1);
    lua_getglobal(w.script.state(), "badAdd");
    const bool badAdd = lua_toboolean(w.script.state(), -1) != 0;
    lua_pop(w.script.state(), 1);
    check(badMul && badAdd,
          "lua math: unsupported operands raise an error");
}

// --- Contacts and deferred body changes -------------------------------------------------

static void testContacts() {
    // Deferred changes: nothing moves while deferral is on, and the queue runs at the start of the next step.
    PhysicsWorld phys;
    phys.setGravity(glm::vec3(0.0f));
    const PhysicsWorld::Handle h = phys.addDynamicSphere(glm::vec3(0, 0, 0), 0.5f, 1.0f);
    phys.setDeferChanges(true);
    phys.setPosition(h, glm::vec3(3, 0, 0));
    phys.addImpulse(h, glm::vec3(0, 1, 0));
    check(glm::length(phys.position(h) - glm::vec3(0, 0, 0)) < 1e-5f,
          "contacts: a deferred setPosition does not move the body yet");
    phys.setDeferChanges(false);
    phys.step(0.0f);
    check(glm::length(phys.position(h) - glm::vec3(3, 0, 0)) < 1e-4f,
          "contacts: the deferred setPosition runs at the start of the next step");

    // Contacts from a real collision: two spheres meet with zero gravity.
    World w;
    w.physics.setGravity(glm::vec3(0.0f));
    w.setEntityPosition("a", glm::vec3(-2, 0, 0));
    w.setEntityPosition("b", glm::vec3(2, 0, 0));
    w.attachDynamicSphere("a", 0.5f, 1.0f);
    w.attachDynamicSphere("b", 0.5f, 1.0f);
    w.physics.addImpulse(w.entity("a").body, glm::vec3(20, 0, 0));
    std::string err;
    const bool loaded = w.script.loadString(R"(
        added = 0
        lastKind = ""
        lastNames = ""
        function onContact(ev)
            lastKind = ev.kind
            if ev.kind == "added" then added = added + 1 end
            lastNames = ev.a .. "," .. ev.b
            -- A body change from a handler is deferred to the next step, so it cannot break Jolt's update.
            ukiyo.setPosition("b", 2, 0, 0)
        end
    )", "contacts_test", &err);
    check(loaded, "contacts: test script loads", err);
    int ticks = 0;
    while (ticks < 300) {
        w.tick(1.0f / 60.0f);
        ++ticks;
        lua_State* L = w.script.state();
        lua_getglobal(L, "added");
        const bool any = lua_tointeger(L, -1) > 0;
        lua_pop(L, 1);
        if (any) break;
    }
    lua_State* L = w.script.state();
    lua_getglobal(L, "added");
    const int added = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    check(added >= 1, "contacts: an added contact reaches onContact", "added=" + std::to_string(added));
    lua_getglobal(L, "lastNames");
    const char* namesC = lua_tostring(L, -1);
    const std::string names = namesC ? namesC : "";
    lua_pop(L, 1);
    check(names == "a,b" || names == "b,a", "contacts: the event names the two entities", names);
    check(!w.script.hasFailed(), "contacts: handler runs without error", w.script.lastError());
}

// --- Resource pack: build, mount, read through the hook, cgltf ----------------------------

static void writeTextFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string readTextFile(const std::filesystem::path& p) {
    std::vector<uint8_t> bytes;
    if (!ukiyo::vfs::readBytes(p.string(), bytes)) return "<unreadable>";
    return std::string(bytes.begin(), bytes.end());
}

static void testResourcePack() {
    namespace vfs = ukiyo::vfs;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("ukiyo_pak_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    writeTextFile(root / "assets" / "a.txt", "hello");
    writeTextFile(root / "assets" / "sub" / "b.bin", std::string("\x00\x01\x02", 3));
    writeTextFile(root / "scripts" / "x.lua", "return 1");
    writeTextFile(root / "outside.txt", "not packed");
    writeTextFile(root / "assets" / "scene.gltf", R"({"asset":{"version":"2.0"}})");
    const fs::path pak = root / "resources.pak";

    std::string err;
    int count = 0;
    const bool built = vfs::buildPak(root.string(), {"assets", "scripts"}, pak.string(), &err, &count);
    check(built && count == 4, "pak: build packs the folders given (4 files)", err + " count=" + std::to_string(count));
    const fs::path pak2 = root / "again.pak";
    const bool built2 = vfs::buildPak(root.string(), {"assets", "scripts"}, pak2.string(), &err);
    check(built2 && readTextFile(pak) == readTextFile(pak2), "pak: the same input gives the same bytes");

    const bool mounted = vfs::mountPak(pak.string(), &err);
    check(mounted, "pak: mounts", err);
    check(readTextFile(root / "assets" / "a.txt") == "hello", "pak: a packed file is read from the pack");
    check(readTextFile(root / "assets" / "sub" / "b.bin") == std::string("\x00\x01\x02", 3),
          "pak: binary data round-trips");
    writeTextFile(root / "assets" / "a.txt", "changed on disk");
    check(readTextFile(root / "assets" / "a.txt") == "hello", "pak: the pack wins over the disk copy while mounted");
    check(readTextFile(root / "outside.txt") == "not packed", "pak: a file not in the pack is read from disk");
    check(vfs::exists((root / "scripts" / "x.lua").string()) && !vfs::exists((root / "missing.txt").string()),
          "pak: exists() finds packed files and rejects missing ones");

    // cgltf reads the .gltf through the hook, so it parses after the file is gone from disk.
    fs::remove(root / "assets" / "scene.gltf");
    cgltf_options opts{};
    vfs::useForCgltf(opts);
    cgltf_data* data = nullptr;
    const cgltf_result r = cgltf_parse_file(&opts, (root / "assets" / "scene.gltf").string().c_str(), &data);
    check(r == cgltf_result_success && data, "pak: cgltf parses a packed .gltf with the file removed from disk");
    if (data) cgltf_free(data);
    vfs::unmountPak();
    check(!vfs::pakMounted(), "pak: unmount");
    fs::remove_all(root);
}

// --- Lua entity handles and the contact filter ----------------------------------------------

static bool luaBool(ScriptHost& sh, const char* name) {
    lua_getglobal(sh.state(), name);
    const bool v = lua_toboolean(sh.state(), -1) != 0;
    lua_pop(sh.state(), 1);
    return v;
}

static std::string luaText(ScriptHost& sh, const char* name) {
    lua_getglobal(sh.state(), name);
    const char* v = lua_tostring(sh.state(), -1);
    std::string out = v ? v : "";
    lua_pop(sh.state(), 1);
    return out;
}

static void testEntityHandles() {
    World w;
    std::string err;
    const bool loaded = w.script.loadString(R"(
        e = ukiyo.Entity("hero")
        e.position = ukiyo.Vector3(1, 2, 3)
        e.yaw = 45
        posX = e.position.x
        posY = e.position.y
        yawBack = e.yaw
        nameBack = e.name
        readonlyBlocked = not pcall(function() e.name = "x" end)
        unknownBlocked = not pcall(function() return e.nosuch end)
        same = (e == ukiyo.Entity("hero"))
        other = (e == ukiyo.Entity("villain"))
        text = tostring(e)
    )", "entity_test", &err);
    check(loaded, "entity: handle script loads", err);
    check(std::fabs(luaNumber(w.script, "posX") - 1.0) < 1e-6 && std::fabs(luaNumber(w.script, "posY") - 2.0) < 1e-6,
          "entity: writable position is read back");
    check(std::fabs(luaNumber(w.script, "yawBack") - 45.0) < 1e-6, "entity: writable yaw is read back");
    check(luaText(w.script, "nameBack") == "hero", "entity: name reads back");
    check(luaBool(w.script, "readonlyBlocked"), "entity: name is read-only");
    check(luaBool(w.script, "unknownBlocked"), "entity: an unknown field is an error");
    check(luaBool(w.script, "same") && !luaBool(w.script, "other"), "entity: handles compare by name");
    check(luaText(w.script, "text") == "Entity(hero)", "entity: tostring names the entity");
    check(w.findEntity("hero") != nullptr && w.findEntity("hero")->yawDeg == 45.0f,
          "entity: writes go to the world entity");
}

static void testContactFilter() {
    World w;
    w.physics.setGravity(glm::vec3(0.0f));
    w.setEntityPosition("a", glm::vec3(-2, 0, 0));
    w.setEntityPosition("b", glm::vec3(2, 0, 0));
    w.attachDynamicSphere("a", 0.5f, 1.0f);
    w.attachDynamicSphere("b", 0.5f, 1.0f);
    w.physics.addImpulse(w.entity("a").body, glm::vec3(20, 0, 0));
    std::string err;
    const bool loaded = w.script.loadString(R"(
        added = 0
        function onContact(ev)
            if ev.kind == "added" then added = added + 1 end
        end
        ukiyo.setContactFilter("a", "b", false)
    )", "filter_test", &err);
    check(loaded && !w.script.hasFailed(), "filter: filter script loads", err);
    for (int i = 0; i < 300; ++i) w.tick(1.0f / 60.0f);
    check(std::fabs(luaNumber(w.script, "added")) < 1e-6, "filter: a filtered pair makes no contact",
          num(luaNumber(w.script, "added")));
}

// --- Visibility: frustum test and distance cull and fade ----------------------------------

static void testVisibility() {
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    const glm::mat4 vp = proj * view;
    const glm::mat4 id(1.0f);
    const glm::vec3 lo(-0.5f), hi(0.5f);

    check(!boxOutsideFrustum(vp, id, lo, hi, 0.0f), "visibility: a box in front of the camera is kept");
    check(boxOutsideFrustum(vp, glm::translate(id, glm::vec3(-50, 0, 0)), lo, hi, 0.0f),
          "visibility: a box far to the side of the view is culled");
    check(!boxOutsideFrustum(vp, glm::translate(id, glm::vec3(2.5f, 0, 0)), lo, hi, 0.0f),
          "visibility: a box that is partly on screen is kept");
    check(!boxOutsideFrustum(vp, glm::translate(id, glm::vec3(-50, 0, 0)), lo, hi, 60.0f),
          "visibility: a margin large enough keeps a box that is outside");
    check(!boxOutsideFrustum(vp, glm::translate(id, glm::vec3(0, 0, 10)), lo, hi, 0.0f),
          "visibility: a box behind the camera is not culled (conservative)");

    const DistanceVisibility off = distanceVisibility(500.0f, 0.0f, 0.0f);
    check(off.visible && off.fade == 0.0f, "distance: a cull distance of 0 turns the cull off");
    const DistanceVisibility far = distanceVisibility(50.0f, 20.0f, 40.0f);
    check(!far.visible, "distance: a model past its cull distance is not drawn");
    const DistanceVisibility mid = distanceVisibility(30.0f, 20.0f, 40.0f);
    check(mid.visible && std::fabs(mid.fade - 0.5f) < 1e-5f, "distance: halfway through the fade band it is half dithered out",
          num(mid.fade));
    const DistanceVisibility near = distanceVisibility(10.0f, 20.0f, 40.0f);
    check(near.visible && near.fade == 0.0f, "distance: inside the fade start it is solid");
}

// --- Navigation ------------------------------------------------------------------------------

static void testNavigation() {
    // A 20 x 20 floor with a wall across the middle: x in [-0.5, 0.5], z in [-3, 3], height 2.
    std::vector<glm::vec3> v = {{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10},
                                {-0.5f, 0, -3}, {0.5f, 0, -3}, {0.5f, 0, 3}, {-0.5f, 0, 3},
                                {-0.5f, 2, -3}, {0.5f, 2, -3}, {0.5f, 2, 3}, {-0.5f, 2, 3}};
    std::vector<uint32_t> t = {0, 2, 1, 0, 3, 2,                    // floor (wound to face up)
                               4, 6, 5, 4, 7, 6,                    // wall bottom (hidden by the top)
                               8, 10, 9, 8, 11, 10,                 // wall top (wound to face up)
                               4, 5, 9, 4, 9, 8, 5, 6, 10, 5, 10, 9, 6, 7, 11, 6, 11, 10, 7, 4, 8, 7, 8, 11};
    NavMesh nav;
    check(nav.build(v, t), "navigation: a navmesh bakes from floor and wall triangles");
    check(nav.built() && nav.polyCount() > 0, "navigation: the navmesh has walkable polygons",
          std::to_string(nav.polyCount()));

    std::vector<glm::vec3> path;
    const bool found = nav.findPath(glm::vec3(-8, 0, 0), glm::vec3(8, 0, 0), &path);
    float maxAbsZ = 0.0f, length = 0.0f;
    for (size_t i = 0; i < path.size(); ++i) {
        maxAbsZ = std::max(maxAbsZ, std::fabs(path[i].z));
        if (i > 0) length += glm::length(path[i] - path[i - 1]);
    }
    check(found && maxAbsZ > 2.5f, "navigation: the path goes around the wall, not through it",
          "max |z| " + num(maxAbsZ));
    check(found && length > 16.0f, "navigation: the detour is longer than the straight line", num(length));
    check(!nav.findPath(glm::vec3(-8, 0, 0), glm::vec3(100, 0, 100), &path),
          "navigation: a point off the mesh has no path");

    const int agent = nav.addAgent(glm::vec3(-8, 0, -8));
    check(agent >= 0 && nav.moveAgent(agent, glm::vec3(8, 0, -8)), "navigation: an agent accepts a move target");
    for (int i = 0; i < 60 * 12; ++i) nav.update(1.0f / 60.0f);
    const glm::vec3 p = nav.agentPosition(agent);
    check(glm::length(glm::vec2(p.x - 8.0f, p.z + 8.0f)) < 1.0f, "navigation: the agent walks to its target",
          num(p.x) + ", " + num(p.z));
    check(nav.agentArrived(agent), "navigation: the crowd reports the agent arrived");
}

// --- Audio -----------------------------------------------------------------------------------

// Writes a mono 16-bit WAV: a sine tone of the given frequency, amplitude, and length.
static std::string writeToneWav(const char* name, double seconds, double freq, double amp) {
    // The system temporary directory, not /tmp: Windows has no /tmp, and a path that cannot be opened would leave the
    // audio checks failing on a platform that is fine.
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    const uint32_t rate = 48000;
    const uint32_t n = uint32_t(seconds * rate);
    std::vector<int16_t> samples(n);
    for (uint32_t i = 0; i < n; ++i)
        samples[i] = int16_t(std::sin(2.0 * M_PI * freq * i / rate) * amp * 32767.0);
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + n * 2);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(n * 2);
    f.write(reinterpret_cast<const char*>(samples.data()), n * 2);
    return path;
}

static double pumpRms(AudioEngine& a, uint32_t frames) {
    std::vector<float> buf(frames * 2, 0.0f);
    a.pump(buf.data(), frames);
    double sum = 0.0;
    for (float s : buf) sum += double(s) * s;
    return std::sqrt(sum / double(buf.size()));
}

static void testAudio() {
    const std::string wav = writeToneWav("ukiyo_systems_test_tone.wav", 1.0, 440.0, 0.5);
    AudioEngine audio(true);
    check(audio.ok(), "audio: the engine starts without an output device", audio.lastError());
    const AudioEngine::SoundId id = audio.load(wav);
    check(id != 0, "audio: a WAV file loads");
    check(audio.play(id) && audio.isPlaying(id), "audio: a loaded sound starts playing");
    const double rms = pumpRms(audio, 4800);
    check(rms > 0.05, "audio: a playing tone produces output", num(rms));
    audio.stop(id);
    check(!audio.isPlaying(id), "audio: stop ends playback");

    // Spatial: the same tone is quieter at 20 m than at 1 m.
    audio.setSpatial(id, true);
    audio.setListener(glm::vec3(0), glm::vec3(0, 0, 1));
    audio.setPosition(id, glm::vec3(1, 0, 0));
    audio.play(id);
    const double near = pumpRms(audio, 4800);
    audio.stop(id);
    audio.setPosition(id, glm::vec3(20, 0, 0));
    audio.play(id);
    const double far = pumpRms(audio, 4800);
    audio.stop(id);
    check(near > 0.0 && far * 3.0 < near, "audio: a sound 20 m away is much quieter than at 1 m",
          "near " + num(near) + ", far " + num(far));
}

// --- Animation state machine ---------------------------------------------------------------

static void testAnimState() {
    using C = AnimStateMachine::Condition;
    AnimStateMachine sm;
    sm.addState("Idle", "Idle_Loop");
    sm.addState("Walk", "Walk");
    sm.addState("Run", "Run");
    sm.addState("Jump", "Jump");
    sm.addTransition("*", "Jump", {{"jump", C::Op::Trigger}}, 0.1f, 0.3f);
    sm.addTransition("Jump", "Idle", {{"grounded", C::Op::IsTrue}}, 0.2f);
    sm.addTransition("Idle", "Walk", {{"speed", C::Op::Greater, 0.1f}});
    sm.addTransition("Walk", "Run", {{"speed", C::Op::Greater, 3.0f}});
    sm.addTransition("Run", "Walk", {{"speed", C::Op::Less, 3.0f}});
    sm.addTransition("Walk", "Idle", {{"speed", C::Op::Less, 0.1f}});

    std::vector<std::string> plays;
    auto play = [&](const std::string& clip, float, float) { plays.push_back(clip); };
    sm.update(0.016f, play);
    check(sm.current() == "Idle" && plays.size() == 1 && plays[0] == "Idle_Loop",
          "anim: the default state starts its clip");

    sm.setFloat("speed", 1.0f);
    sm.update(0.016f, play);
    check(sm.current() == "Walk", "anim: speed above 0.1 moves Idle to Walk");
    sm.setFloat("speed", 4.0f);
    sm.update(0.016f, play);
    check(sm.current() == "Run", "anim: speed above 3 moves Walk to Run");
    sm.setFloat("speed", 0.0f);
    for (int i = 0; i < 3; ++i) sm.update(0.016f, play);
    check(sm.current() == "Idle", "anim: slowing to zero returns to Idle through Walk");

    sm.setTrigger("jump");
    sm.update(0.1f, play);
    check(sm.current() == "Idle", "anim: a trigger waits out the minimum dwell time");
    sm.update(0.3f, play);
    check(sm.current() == "Jump", "anim: the trigger fires once the dwell time has passed");
    sm.update(0.5f, play);
    check(sm.current() == "Jump", "anim: a consumed trigger does not fire again");
    sm.setBool("grounded", true);
    sm.update(0.016f, play);
    check(sm.current() == "Idle", "anim: landing returns to Idle");
    check(plays.back() == "Idle_Loop" && plays.size() == 7, "anim: each state change plays its clip once",
          std::to_string(plays.size()) + " plays");
}

// --- Scripting -------------------------------------------------------------------------------

static void testScript() {
    ScriptHost sh;
    std::string err;
    check(sh.loadString("count = 0\nfunction onUpdate(dt) count = count + dt end", "t", &err),
          "script: a chunk with onUpdate loads", err);
    sh.callUpdate(1.0);
    sh.callUpdate(2.0);
    lua_getglobal(sh.state(), "count");
    check(lua_tonumber(sh.state(), -1) == 3.0, "script: onUpdate runs each frame with dt");
    lua_pop(sh.state(), 1);

    ScriptHost box;
    box.loadString("sandboxed = (io == nil) and (os == nil) and (package == nil) and (dofile == nil) and "
                   "(loadfile == nil) and (require == nil)",
                   "sandbox", &err);
    lua_getglobal(box.state(), "sandboxed");
    check(lua_toboolean(box.state(), -1) == 1, "script: io, os, package, and file loaders are not available");
    lua_pop(box.state(), 1);

    ScriptHost bad;
    check(!bad.loadString("error('boom')", "bad", &err) && err.find("boom") != std::string::npos,
          "script: a runtime error is reported, not thrown", err);
}

// --- World: scripts drive the systems ---------------------------------------------------

static void testWorld() {
    World w(true);
    w.physics.addStaticBox(glm::vec3(0, -0.5f, 0), glm::vec3(20, 0.5f, 20));
    w.entity("ball").position = glm::vec3(0, 4, 0);
    w.attachDynamicSphere("ball", 0.5f, 1.0f);

    std::string err;
    const bool loaded = w.script.loadString(
        "function onStart() ukiyo.log('started') ukiyo.setPosition('probe', 1, 2, 3) end\n"
        "function onUpdate(dt)\n"
        "  local hit, x, y, z = ukiyo.raycast(8, 10, 0, 0, -1, 0, 20)\n"
        "  floorY = hit and y or -99\n"
        "  if ukiyo.time() > 0 then timeSeen = true end\n"
        "end\n",
        "world", &err);
    check(loaded, "world: a script with onStart and onUpdate loads", err);
    w.script.callStart();
    check(w.logs.size() == 1 && w.logs[0] == "started", "world: ukiyo.log reaches the host");
    const glm::vec3 probe = w.entity("probe").position;
    check(std::fabs(probe.x - 1.0f) < 1e-4f && std::fabs(probe.y - 2.0f) < 1e-4f,
          "world: ukiyo.setPosition moves a named entity", num(probe.y));

    for (int i = 0; i < 180; ++i) w.tick(1.0f / 60.0f);
    lua_getglobal(w.script.state(), "floorY");
    const double floorY = lua_tonumber(w.script.state(), -1);
    lua_pop(w.script.state(), 1);
    check(std::fabs(floorY) < 0.05, "world: ukiyo.raycast from a script hits the floor", num(floorY));
    check(std::fabs(w.entity("ball").position.y - 0.5f) < 0.05f,
          "world: a physics entity follows its body each tick", num(w.entity("ball").position.y));
    check(!w.script.hasFailed(), "world: scripts ran without error", w.script.lastError());
}

static void testWorldAnimation() {
    using C = AnimStateMachine::Condition;
    World w(true);
    Entity& hero = w.entity("hero");
    hero.anim = std::make_unique<AnimStateMachine>();
    AnimStateMachine& sm = *hero.anim;
    sm.addState("Idle", "Idle_Loop");
    sm.addState("Walk", "Walk");
    sm.addState("Run", "Run");
    sm.addTransition("Idle", "Walk", {{"speed", C::Op::Greater, 0.1f}});
    sm.addTransition("Walk", "Run", {{"speed", C::Op::Greater, 3.0f}});
    sm.addTransition("Run", "Walk", {{"speed", C::Op::Less, 3.0f}});
    sm.addTransition("Walk", "Idle", {{"speed", C::Op::Less, 0.1f}});

    w.tick(0.016f);
    check(w.entity("hero").clip == "Idle_Loop", "world: an entity starts on its default clip");
    w.setParam("hero", "speed", 4.0f);
    w.tick(0.016f);
    check(w.entity("hero").clip == "Walk", "world: a script-set parameter changes the clip on the next tick",
          w.entity("hero").clip);
    w.setParam("hero", "speed", 5.0f);
    w.tick(0.016f);
    check(w.entity("hero").clip == "Run", "world: the state machine moves on to Run", w.entity("hero").clip);
}

static void testWorldNavigation() {
    World w(true);
    std::vector<glm::vec3> v = {{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}};
    w.nav.build(v, {0, 2, 1, 0, 3, 2});
    w.entity("npc").position = glm::vec3(-6, 0, 0);
    w.attachAgent("npc");
    check(w.nav.moveAgent(w.entity("npc").agent, glm::vec3(6, 0, 0)), "world: an agent moves toward a target");
    for (int i = 0; i < 60 * 6; ++i) w.tick(1.0f / 60.0f);
    check(w.entity("npc").position.x > 4.0f, "world: the navmesh drives the entity position",
          num(w.entity("npc").position.x));
}

// --- VFX -------------------------------------------------------------------------------------

static void testVfx() {
    EmitterSettings s;
    s.rate = 20.0f;
    s.lifetime = 2.0f;
    VfxSystem vfx;
    VfxSystem::Handle h = vfx.spawn(s, glm::vec3(0));
    for (int i = 0; i < 60; ++i) vfx.update(1.0f / 60.0f);
    check(vfx.liveParticles() >= 18 && vfx.liveParticles() <= 21, "vfx: a looping emitter spawns at its rate (20 in 1 s)",
          std::to_string(vfx.liveParticles()));

    EmitterSettings capped = s;
    capped.maxParticles = 10;
    VfxSystem::Handle c = vfx.spawn(capped, glm::vec3(0));
    for (int i = 0; i < 120; ++i) vfx.update(1.0f / 60.0f);
    check(vfx.find(c) && vfx.find(c)->liveCount() <= 10, "vfx: maxParticles caps the live count");

    std::vector<ParticleInstance> inst;
    vfx.instances(&inst);
    check(inst.size() == size_t(vfx.liveParticles()), "vfx: one instance per live particle");
    check(!inst.empty() && inst[0].position[3] > 0.0f, "vfx: instances carry a positive size");

    EmitterSettings once;
    once.loop = false;
    once.lifetime = 0.5f;
    VfxSystem::Handle b = vfx.spawn(once, glm::vec3(0));
    vfx.burst(b, 30);
    check(vfx.find(b) && vfx.find(b)->liveCount() == 30, "vfx: a burst emits the requested count");
    for (int i = 0; i < 60; ++i) vfx.update(1.0f / 60.0f);
    check(vfx.find(b) == nullptr, "vfx: a finished one-shot emitter is removed");

    vfx.remove(h);
    check(vfx.find(h) == nullptr, "vfx: remove deletes an emitter");
}

// The project's input actions (a name and portable key text) are compiled into an engine input-system action map, and
// scripts read them through actionDown(). docs/INPUT.md.
static void testWorldInput() {
    World w(true);
    w.setInputActions({{"Jump", "Space"}, {"Run", "Shift"}, {"Typo", "Hyper"}});
    check(w.input().action("Gameplay", "Jump") != nullptr, "input: a project action becomes an action in the Gameplay map");
    check(w.input().map("Gameplay") && w.input().map("Gameplay")->enabled, "input: the Gameplay map is enabled");
    check(!w.actionDown("Jump"), "input: an action nobody is pressing is false");
    check(w.input().action("Gameplay", "Typo") != nullptr, "input: a key the engine cannot name keeps its action");

    w.setKeyHeld("Space", true);
    check(w.actionDown("Jump"), "input: a key press reaches actionDown before the next tick");
    w.tick(0.1f);
    check(w.actionDown("Jump"), "input: and it is still held after a tick");
    w.setKeyHeld("Space", false);
    check(!w.actionDown("Jump"), "input: releasing the key ends the action");

    // A modifier is a normal control name here, not a chord; the key text is the one Qt's QKeySequence writes.
    w.setKeyHeld("Shift", true);
    check(w.actionDown("Run"), "input: Qt's Shift maps to leftShift and drives its action");

    // An unknown key is ignored on both sides, so a typo in the project settings cannot half-work.
    w.setKeyHeld("Hyper", true);
    check(!w.actionDown("Typo"), "input: an action with an unknown key never fires");
    w.setKeyHeld("Shift", false);
    check(!w.actionDown("Run"), "input: the modifier releases too");

    // A project file may spell the control the way Unity does, and setting the actions again mid-session replaces the
    // map instead of growing it.
    w.setInputActions({{"Run", "LeftShift"}});
    w.setKeyHeld("Shift", false);
    w.setKeyHeld("LeftShift", true);
    check(w.actionDown("Run"), "input: LeftShift is accepted as an alias for leftShift");
    check(w.input().action("Gameplay", "Jump") == nullptr, "input: replacing the actions drops the old ones");
}

static void testActions() {
    check(std::fabs(ukiyo::pingPongWeight(0.0f, false)) < 1e-5f, "actions: a linear ping-pong starts at rest");
    check(std::fabs(ukiyo::pingPongWeight(0.25f, false) - 0.5f) < 1e-5f, "actions: a linear ping-pong is halfway at a quarter cycle");
    check(std::fabs(ukiyo::pingPongWeight(0.5f, false) - 1.0f) < 1e-5f, "actions: a linear ping-pong reaches the far end at half a cycle");
    check(std::fabs(ukiyo::pingPongWeight(2.25f, false) - 0.5f) < 1e-5f, "actions: the phase wraps every cycle");
    check(std::fabs(ukiyo::pingPongWeight(0.5f, true) - 1.0f) < 1e-5f, "actions: an eased ping-pong reaches the far end at half a cycle");
    check(std::fabs(ukiyo::pingPongWeight(0.25f, true) - 0.5f) < 1e-5f, "actions: an eased ping-pong is halfway at a quarter cycle");

    ukiyo::World w(true);
    ukiyo::EntityAction spin;
    spin.on = true;
    spin.speed = 90.0f;
    w.setAction("spinner", spin, glm::vec3(0.0f), 10.0f);
    w.tick(0.5f);
    const ukiyo::Entity* s = w.findEntity("spinner");
    check(s && std::fabs(s->yawDeg - 55.0f) < 1e-3f, "actions: a spin at 90 deg/s turns 45 degrees in half a second", s ? num(s->yawDeg) : "missing");

    ukiyo::EntityAction pp;
    pp.on = true;
    pp.pingPong = true;
    pp.speed = 1.0f;
    pp.axis = glm::vec3(1.0f, 0.0f, 0.0f);
    pp.amplitude = 2.0f;
    w.setAction("bob", pp, glm::vec3(1.0f, 2.0f, 3.0f), 0.0f);
    w.tick(0.25f);
    const ukiyo::Entity* b = w.findEntity("bob");
    check(b && glm::length(b->position - glm::vec3(2.0f, 2.0f, 3.0f)) < 1e-4f,
          "actions: a ping-pong at a quarter cycle is halfway to its 2 m far end", b ? num(b->position.x) : "missing");

    // A start offset keeps the object at rest until the offset has passed, then the action runs from zero.
    ukiyo::EntityAction late;
    late.on = true;
    late.speed = 90.0f;
    late.startOffset = 1.0f;
    w.setAction("late", late, glm::vec3(0.0f), 0.0f);
    w.tick(0.5f);
    const ukiyo::Entity* l1 = w.findEntity("late");
    check(l1 && std::fabs(l1->yawDeg) < 1e-3f, "actions: a spin waits at rest during its start offset",
          l1 ? num(l1->yawDeg) : "missing");
    w.tick(1.0f);
    const ukiyo::Entity* l2 = w.findEntity("late");
    check(l2 && std::fabs(l2->yawDeg - 45.0f) < 1e-3f, "actions: a spin runs for its time after the offset ends",
          l2 ? num(l2->yawDeg) : "missing");
}

static void testSceneSwitch() {
    ukiyo::World w(true);
    check(!w.sceneRequested() && !w.sceneSwitchReady(), "scene: nothing is requested at start");

    // A request with no holds is ready at once. Taking it clears the request.
    w.requestScene("levels/two.ukscene");
    check(w.sceneSwitchReady(), "scene: a request with no holds is ready to switch");
    check(w.takeSceneRequest() == "levels/two.ukscene" && !w.sceneRequested(), "scene: taking the request returns its path and clears it");

    // A hold stops the switch until it is released. Releasing an unknown token does nothing.
    const int hold = w.holdLoad();
    w.requestScene("levels/three.ukscene");
    check(w.sceneRequested() && !w.sceneSwitchReady(), "scene: an open hold keeps the request waiting");
    check(!w.releaseLoad(hold + 99), "scene: releasing an unknown hold returns false");
    check(w.releaseLoad(hold) && w.sceneSwitchReady(), "scene: releasing the last hold makes the switch ready");
    check(!w.releaseLoad(hold), "scene: a hold cannot be released twice");

    // Two holds: the switch waits for both. A second request replaces the first.
    const int a = w.holdLoad();
    const int b = w.holdLoad();
    w.requestScene("levels/first.ukscene");
    w.requestScene("levels/last.ukscene");
    w.releaseLoad(a);
    check(!w.sceneSwitchReady() && w.loadHoldCount() == 1, "scene: the switch waits until every hold is released");
    w.releaseLoad(b);
    check(w.takeSceneRequest() == "levels/last.ukscene", "scene: a second request replaces the first");

    // Preload requests queue up and are taken once.
    w.requestPreload("assets/a.vrm");
    w.requestPreload("assets/b.glb");
    std::vector<std::string> pre = w.takePreloads();
    check(pre.size() == 2 && pre[0] == "assets/a.vrm" && pre[1] == "assets/b.glb", "scene: preload requests are taken in order");
    check(w.takePreloads().empty(), "scene: preload requests are taken only once");

    // Scripts reach the same calls, and onSceneLoaded runs when the scene starts.
    ukiyo::World s(true);
    std::string err;
    const std::string code =
        "function onSceneLoaded() ukiyo.log(\"loaded\") end\n"
        "local h = ukiyo.holdLoad()\n"
        "ukiyo.loadScene(\"levels/next.ukscene\")\n"
        "ukiyo.preloadModel(\"assets/next.vrm\")\n"
        "HOLD = h\n";
    check(s.script.loadString(code, "scene-test", &err), "scene: the script with the scene calls loads", err);
    check(s.sceneRequested() && !s.sceneSwitchReady(), "scene: ukiyo.loadScene waits behind ukiyo.holdLoad");
    s.notifySceneLoaded();
    check(s.logs.size() == 1 && s.logs[0] == "loaded", "scene: onSceneLoaded runs once the scene starts");
    lua_getglobal(s.script.state(), "HOLD");
    const int token = static_cast<int>(lua_tointeger(s.script.state(), -1));
    lua_pop(s.script.state(), 1);
    check(token > 0 && s.script.loadString("ukiyo.releaseLoad(HOLD)", "release", &err) && s.sceneSwitchReady(),
          "scene: ukiyo.releaseLoad lets the requested switch run", err);
    check(s.takeSceneRequest() == "levels/next.ukscene" && s.takePreloads().size() == 1,
          "scene: the Lua request and preload reach the host");
}

// Water surface (item 16): flat when the waves are off, unit normals, a normal that matches the surface's own
// slope, no folds for the default volume, and waves that move with time.
static void testWater() {
    ukiyo::WaterVolume flat;
    for (ukiyo::WaterWave& w : flat.waves) w.amplitude = 0.0f;
    flat.level = 1.5f;
    const ukiyo::WaterSurface a = ukiyo::waterSurface(flat, 3.0f, -2.0f, 0.7f);
    check(std::fabs(a.pos.y - 1.5f) < 1e-5f && std::fabs(a.normal.y - 1.0f) < 1e-5f,
          "water: with no waves the surface is flat at the level, normal up");
    check(a.crest == 0.0f, "water: with no waves there is no crest foam");

    ukiyo::WaterVolume v;   // defaults: four waves
    v.level = 0.25f;
    bool unit = true, upward = true, matches = true, folded = false;
    float worstDot = 1.0f;
    const float e = 2e-3f;
    for (int i = -20; i <= 20; ++i)
        for (int j = -20; j <= 20; ++j) {
            const float x = 0.4f * float(i), z = 0.4f * float(j);
            const float t = 1.3f;
            const ukiyo::WaterSurface s = ukiyo::waterSurface(v, x, z, t);
            if (std::fabs(glm::length(s.normal) - 1.0f) > 1e-4f) unit = false;
            if (s.normal.y <= 0.0f) folded = true;
            if (s.normal.y <= 0.0f) upward = false;
            // Tangents from the solved surface itself. Their cross product must agree with the analytic normal.
            const glm::vec3 dx = ukiyo::waterSurface(v, x + e, z, t).pos - ukiyo::waterSurface(v, x - e, z, t).pos;
            const glm::vec3 dz = ukiyo::waterSurface(v, x, z + e, t).pos - ukiyo::waterSurface(v, x, z - e, t).pos;
            glm::vec3 nFd = glm::cross(dz, dx);
            if (glm::length(nFd) < 1e-9f) continue;
            nFd = glm::normalize(nFd);
            const float d = glm::dot(nFd, s.normal);
            if (d < worstDot) worstDot = d;
            if (d < 0.999f) matches = false;
        }
    check(unit, "water: normals have unit length");
    check(upward && !folded, "water: no fold on the default volume (every normal points up)");
    check(matches, "water: the analytic normal matches the surface's own tangents (worst dot " +
                       std::to_string(worstDot) + ")");

    const ukiyo::WaterSurface t0 = ukiyo::waterSurface(v, 1.0f, 1.0f, 0.0f);
    const ukiyo::WaterSurface t1 = ukiyo::waterSurface(v, 1.0f, 1.0f, 1.0f);
    check(std::fabs(t0.pos.y - t1.pos.y) > 1e-3f, "water: the waves move with time");
    check(ukiyo::waterHeight(v, 1.0f, 1.0f, 0.0f) == t0.pos.y && ukiyo::waterNormal(v, 1.0f, 1.0f, 0.0f) == t0.normal,
          "water: waterHeight and waterNormal agree with waterSurface");
    check(ukiyo::waterSurface(v, 0.0f, 0.0f, 0.0f).pos.y >= v.level - 0.5f,
          "water: height stays within the summed amplitude");

    v.centre = glm::vec2(2.0f, -1.0f);
    v.half = glm::vec2(3.0f, 2.0f);
    check(ukiyo::waterContainsXZ(v, 4.9f, -2.9f) && !ukiyo::waterContainsXZ(v, 5.1f, -1.0f) &&
              !ukiyo::waterContainsXZ(v, 2.0f, 1.1f),
          "water: the containment test is the authored rectangle");
}

// Buoyancy (item 17). The factor is water density over body density, so a light box (1 kg in 1 m^3) floats: a
// submerged one rises to the surface, and one dropped on it stays near it. A heavy box (2000 kg in 1 m^3) sinks.
// Without water both fall freely.
static void testBuoyancy() {
    ukiyo::WaterVolume still;
    for (ukiyo::WaterWave& w : still.waves) w.amplitude = 0.0f;
    still.level = 0.0f;
    still.centre = glm::vec2(0.0f);
    still.half = glm::vec2(20.0f);

    {
        ukiyo::PhysicsWorld p;
        p.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
        const ukiyo::PhysicsWorld::Handle h = p.addDynamicBox(glm::vec3(0.0f, -1.5f, 0.0f), glm::vec3(0.5f), 1.0f);
        p.setWater(&still);
        for (int i = 0; i < 600; ++i) p.step(1.0f / 60.0f);
        const float y = p.position(h).y;
        check(y > -1.0f && y < 1.0f, "buoyancy: a submerged light box rises and floats (y " + std::to_string(y) + ")");
    }
    {
        ukiyo::PhysicsWorld p;
        p.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
        const ukiyo::PhysicsWorld::Handle h = p.addDynamicBox(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), 1.0f);
        p.setWater(&still);
        p.setWaterTime(0.0f);
        for (int i = 0; i < 480; ++i) p.step(1.0f / 60.0f);
        const float y = p.position(h).y;
        check(y > -1.0f && y < 1.0f, "buoyancy: a light box dropped on the surface floats near it (y " + std::to_string(y) + ")");
    }
    {
        ukiyo::PhysicsWorld p;
        p.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
        const ukiyo::PhysicsWorld::Handle h = p.addDynamicBox(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), 2000.0f);
        p.setWater(&still);
        for (int i = 0; i < 240; ++i) p.step(1.0f / 60.0f);
        const float y = p.position(h).y;
        check(y < -2.0f, "buoyancy: a heavy box sinks in the water (y " + std::to_string(y) + ")");
    }
    {
        ukiyo::PhysicsWorld p;
        p.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
        const ukiyo::PhysicsWorld::Handle h = p.addDynamicBox(glm::vec3(0.0f, -1.5f, 0.0f), glm::vec3(0.5f), 1.0f);
        for (int i = 0; i < 120; ++i) p.step(1.0f / 60.0f);
        check(p.position(h).y < -15.0f, "buoyancy: without water the same box falls freely");
    }
}

// Mesh LOD (item 15): level selection by distance, and the vendored simplifier on a grid. Simplification keeps
// the boundary edges (border locked), the index list stays valid, and the triangle count drops.
static void testLod() {
    const std::vector<float> sw = {25.0f, 60.0f};
    check(ukiyo::lodForDistance(10.0f, sw) == 0 && ukiyo::lodForDistance(25.0f, sw) == 1 &&
              ukiyo::lodForDistance(59.9f, sw) == 1 && ukiyo::lodForDistance(60.0f, sw) == 2 &&
              ukiyo::lodForDistance(1000.0f, sw) == 2,
          "lod: a distance selects the level whose switch it has reached");
    check(ukiyo::lodForDistance(1000.0f, {}) == 0, "lod: no switches means full detail");

    // A 40 x 40 quad grid in the z = 0 plane: 1681 vertices, 9600 indices (3200 triangles).
    const uint32_t N = 40;
    std::vector<float> verts;
    for (uint32_t j = 0; j <= N; ++j)
        for (uint32_t i = 0; i <= N; ++i) {
            verts.push_back(float(i));
            verts.push_back(float(j));
            verts.push_back(0.0f);
        }
    std::vector<uint32_t> idx;
    for (uint32_t j = 0; j < N; ++j)
        for (uint32_t i = 0; i < N; ++i) {
            const uint32_t a = j * (N + 1) + i, b = a + 1, c = a + (N + 1), d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    auto boundaryEdges = [](const std::vector<uint32_t>& t) {
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        for (size_t k = 0; k + 2 < t.size(); k += 3)
            for (int e = 0; e < 3; ++e) {
                uint32_t p = t[k + e], q = t[k + (e + 1) % 3];
                if (p > q) std::swap(p, q);
                ++edges[{p, q}];
            }
        int n = 0;
        for (const auto& kv : edges) n += kv.second == 1 ? 1 : 0;
        return n;
    };
    const int boundaryBefore = boundaryEdges(idx);
    std::vector<uint32_t> out(idx.size());
    float error = 0.0f;
    const size_t target = idx.size() / 2 - (idx.size() / 2) % 3;
    const size_t count = meshopt_simplify(out.data(), idx.data(), idx.size(), verts.data(), verts.size() / 3,
                                          3 * sizeof(float), target, 0.05f, meshopt_SimplifyLockBorder, &error);
    out.resize(count);
    bool valid = count % 3 == 0 && count > 0 && count < idx.size();
    for (uint32_t i : out) valid = valid && i < verts.size() / 3;
    check(valid, "lod: simplification gives a shorter, valid triangle list", "indices " + std::to_string(count));
    check(boundaryEdges(out) == boundaryBefore,
          "lod: the locked border keeps its boundary edges (" + std::to_string(boundaryBefore) + ")");
}

// Cleaning of LOD switch distances from files and the Inspector (item 15).
static void testLodSwitches() {
    check(ukiyo::normalizeLodSwitches({}).empty(), "lod switches: empty means off");
    check(ukiyo::normalizeLodSwitches({60.0f, 25.0f, 25.0f, -1.0f, 0.0f}) == std::vector<float>({25.0f, 60.0f}),
          "lod switches: sorted, repeats and non-positive values dropped");
    check(ukiyo::normalizeLodSwitches({std::nanf(""), 10.0f, INFINITY}) == std::vector<float>({10.0f}),
          "lod switches: NaN and infinity dropped");
    check(ukiyo::normalizeLodSwitches({1.0f, 2.0f, 3.0f, 4.0f, 5.0f}).size() == ukiyo::kMaxLodSwitches,
          "lod switches: capped at the most levels one model may have");
}

// Terrain step 1 (item 18): heightmap sampling, chunk meshes, seams between chunks, the raw 16-bit file, and a
// physics ray that hits a chunk's collider.
// Step 2: layer weights. Every sample's weights sum to one, a layer follows its height and slope bands, paint switches
// a layer off, and a sample that no layer matches goes to layer 0.
static void testTerrainLayers() {
    auto weightAt = [](const std::vector<float>& w, const Heightmap& m, uint32_t x, uint32_t z, size_t layer) {
        return w[(size_t(z) * m.size + x) * kTerrainMaxLayers + layer];
    };

    // A ramp that rises 1 m per metre (heightScale 128, one sample step is 1/128): 45 degrees everywhere inside.
    Heightmap ramp;
    ramp.size = 129;
    ramp.cellSize = 1.0f;
    ramp.heightScale = 128.0f;
    ramp.samples.resize(size_t(129) * 129);
    for (uint32_t z = 0; z < 129; ++z)
        for (uint32_t x = 0; x < 129; ++x) ramp.samples[size_t(z) * 129 + x] = float(x) / 128.0f;
    const float slope45 = slopeDegAt(ramp, 64, 64);
    check(std::fabs(slope45 - 45.0f) < 0.01f, "terrain layers: a 1 m per metre ramp reads 45 degrees", std::to_string(slope45));

    std::vector<TerrainLayer> flatOrSteep(2);
    flatOrSteep[0].name = "flat";
    flatOrSteep[0].slopeMin = 0.0f;
    flatOrSteep[0].slopeMax = 25.0f;
    flatOrSteep[0].slopeFade = 5.0f;
    flatOrSteep[1].name = "steep";
    flatOrSteep[1].slopeMin = 30.0f;
    flatOrSteep[1].slopeMax = 90.0f;
    flatOrSteep[1].slopeFade = 5.0f;
    const std::vector<float> wr = layerWeightMap(ramp, flatOrSteep, {});
    bool sumsRamp = wr.size() == size_t(129) * 129 * kTerrainMaxLayers;
    for (size_t s = 0; sumsRamp && s < wr.size() / kTerrainMaxLayers; ++s) {
        float sum = 0.0f;
        for (uint32_t k = 0; k < kTerrainMaxLayers; ++k) sum += wr[s * kTerrainMaxLayers + k];
        sumsRamp = std::fabs(sum - 1.0f) < 1e-5f;
    }
    check(sumsRamp, "terrain layers: every sample's weights sum to one");
    check(weightAt(wr, ramp, 64, 64, 1) > 0.999f && weightAt(wr, ramp, 64, 64, 0) < 0.001f,
          "terrain layers: a 45 degree slope takes the steep layer, not the flat one",
          "steep " + std::to_string(weightAt(wr, ramp, 64, 64, 1)) + ", flat " + std::to_string(weightAt(wr, ramp, 64, 64, 0)));

    // Height bands on flat ground: low ground takes layer 0, high ground takes layer 1.
    Heightmap flat;
    flat.size = 65;
    flat.cellSize = 1.0f;
    flat.heightScale = 10.0f;
    flat.samples.assign(size_t(65) * 65, 0.2f);
    std::vector<TerrainLayer> bands(2);
    bands[0].heightMin = 0.0f;
    bands[0].heightMax = 0.5f;
    bands[0].heightFade = 0.05f;
    bands[1].heightMin = 0.5f;
    bands[1].heightMax = 1.0f;
    bands[1].heightFade = 0.05f;
    const std::vector<float> wLow = layerWeightMap(flat, bands, {});
    check(std::fabs(weightAt(wLow, flat, 10, 10, 0) - 1.0f) < 1e-5f && weightAt(wLow, flat, 10, 10, 1) < 1e-5f,
          "terrain layers: low ground (0.2) takes the low band only");
    for (auto& s : flat.samples) s = 0.9f;
    const std::vector<float> wHigh = layerWeightMap(flat, bands, {});
    check(std::fabs(weightAt(wHigh, flat, 10, 10, 1) - 1.0f) < 1e-5f && weightAt(wHigh, flat, 10, 10, 0) < 1e-5f,
          "terrain layers: high ground (0.9) takes the high band only");

    // Slope bands on flat ground: nothing is steep, so the steep layer gets nothing and layer 0 gets everything.
    const std::vector<float> wFlat = layerWeightMap(flat, flatOrSteep, {});
    check(std::fabs(weightAt(wFlat, flat, 5, 5, 0) - 1.0f) < 1e-5f && weightAt(wFlat, flat, 5, 5, 1) < 1e-5f,
          "terrain layers: flat ground takes the flat layer, never the steep one");

    // Paint: switching layer 1 off at one sample gives that sample to layer 0. Both layers cover the flat ground.
    std::vector<TerrainLayer> both(2);
    flat.samples.assign(size_t(65) * 65, 0.2f);
    std::vector<uint8_t> paint(size_t(65) * 65 * kTerrainMaxLayers, 255);
    paint[1 * kTerrainMaxLayers + 1] = 0;   // sample (1, 0), layer 1 painted off
    const std::vector<float> wp = layerWeightMap(flat, both, paint);
    check(std::fabs(weightAt(wp, flat, 1, 0, 0) - 1.0f) < 1e-5f && weightAt(wp, flat, 1, 0, 1) < 1e-5f,
          "terrain layers: paint switches a layer off at its sample");
    check(std::fabs(weightAt(wp, flat, 2, 0, 0) - 0.5f) < 1e-5f && std::fabs(weightAt(wp, flat, 2, 0, 1) - 0.5f) < 1e-5f,
          "terrain layers: two layers that both match share a sample equally");

    // Nothing matches, or everything is painted off: layer 0 takes the sample, so the sum is still one.
    std::vector<TerrainLayer> nothing(1);
    nothing[0].heightMin = 0.6f;
    nothing[0].heightMax = 1.0f;
    nothing[0].heightFade = 0.0f;
    const std::vector<float> wn = layerWeightMap(flat, nothing, {});
    check(std::fabs(weightAt(wn, flat, 3, 3, 0) - 1.0f) < 1e-5f, "terrain layers: an unmatched sample goes to layer 0");
    std::vector<uint8_t> none(size_t(65) * 65 * kTerrainMaxLayers, 0);
    const std::vector<float> wz = layerWeightMap(flat, both, none);
    check(std::fabs(weightAt(wz, flat, 3, 3, 0) - 1.0f) < 1e-5f, "terrain layers: fully painted-off ground goes to layer 0");

    // Bad input gives an empty result rather than wrong weights.
    check(layerWeightMap(flat, {}, {}).empty(), "terrain layers: no layers gives no weights");
    check(layerWeightMap(flat, both, std::vector<uint8_t>(10, 255)).empty(), "terrain layers: a paint map of the wrong size is refused");

    // The default layers, on a bumpy map, still sum to one everywhere.
    Heightmap bumpy;
    bumpy.size = 129;
    bumpy.cellSize = 1.0f;
    bumpy.heightScale = 40.0f;
    bumpy.samples.resize(size_t(129) * 129);
    for (uint32_t z = 0; z < 129; ++z)
        for (uint32_t x = 0; x < 129; ++x)
            bumpy.samples[size_t(z) * 129 + x] =
                0.5f + 0.4f * std::sin(0.11f * float(x)) * std::cos(0.07f * float(z));
    const std::vector<float> wd = layerWeightMap(bumpy, defaultTerrainLayers(), {});
    bool sumsBumpy = wd.size() == size_t(129) * 129 * kTerrainMaxLayers;
    float worst = 0.0f;
    for (size_t s = 0; sumsBumpy && s < wd.size() / kTerrainMaxLayers; ++s) {
        float sum = 0.0f;
        for (uint32_t k = 0; k < kTerrainMaxLayers; ++k) sum += wd[s * kTerrainMaxLayers + k];
        worst = std::max(worst, std::fabs(sum - 1.0f));
    }
    check(sumsBumpy && worst < 1e-5f, "terrain layers: the default layers sum to one on a bumpy map", std::to_string(worst));
    check(defaultTerrainLayers().size() == 3, "terrain layers: a new tile starts with three layers");
}

// Step 2b: layer textures. The atlas packs one cell per layer. A layer with no maps gets white diffuse, flat normals,
// and its constant roughness. Named files are read, and a missing file is an error.
static void testTerrainAtlases() {
    const uint32_t cell = kTerrainAtlasCell;
    auto texel = [&](const TerrainAtlases& a, const std::vector<uint8_t>& img, uint32_t col, uint32_t row, uint32_t x, uint32_t y) {
        const size_t i = (size_t(row * cell + y) * a.width + (col * cell + x)) * 4;
        return std::array<uint8_t, 4>{img[i], img[i + 1], img[i + 2], img[i + 3]};
    };

    TerrainAtlases plain;
    std::string err;
    const bool okPlain = buildTerrainAtlases(defaultTerrainLayers(), &plain, &err);
    check(okPlain && plain.width == 2048 && plain.height == 1024 && plain.diffuse.size() == size_t(2048) * 1024 * 4,
          "terrain atlas: the atlases are 4 by 2 cells of 512 texels", err);
    auto d0 = texel(plain, plain.diffuse, 0, 0, 10, 10);
    check(okPlain && d0[0] == 255 && d0[1] == 255 && d0[2] == 255 && std::abs(int(d0[3]) - 230) <= 1,
          "terrain atlas: a layer with no diffuse map is white, with its roughness in alpha (grass 0.9)",
          std::to_string(int(d0[3])));
    auto n0 = texel(plain, plain.normal, 0, 0, 10, 10);
    check(okPlain && n0[0] == 128 && n0[1] == 128 && n0[2] == 255, "terrain atlas: no normal map is a flat normal");
    check(okPlain && n0[3] == 255, "terrain atlas: no occlusion map means full ambient light");
    auto d7 = texel(plain, plain.diffuse, 3, 1, 5, 5);   // cell 7 is unused by the defaults
    check(okPlain && std::abs(int(d7[3]) - 204) <= 1 && d7[0] == 255, "terrain atlas: an unused cell is white with roughness 0.8");

    // Files: a checker for the diffuse of layer 0 and a flat grey roughness of 128.
    std::vector<TerrainLayer> textured = defaultTerrainLayers();
    textured[0].diffuseFile = std::string(UKIYO_ROOT) + "/tests/data/terrain_checker_64.png";
    textured[0].roughnessFile = std::string(UKIYO_ROOT) + "/tests/data/terrain_grey_64.png";
    textured[0].aoFile = std::string(UKIYO_ROOT) + "/tests/data/terrain_grey_64.png";
    TerrainAtlases withFiles;
    err.clear();
    const bool okFiles = buildTerrainAtlases(textured, &withFiles, &err);
    check(okFiles, "terrain atlas: a layer's diffuse and roughness files are read", err);
    if (okFiles) {
        int darkest = 255, brightest = 0;
        bool roughOk = true;
        for (uint32_t y = 0; y < cell; ++y)
            for (uint32_t x = 0; x < cell; ++x) {
                auto t = texel(withFiles, withFiles.diffuse, 0, 0, x, y);
                darkest = std::min<int>(darkest, t[0]);
                brightest = std::max<int>(brightest, t[0]);
                roughOk = roughOk && std::abs(int(t[3]) - 128) <= 2;
            }
        check(darkest < 60 && brightest > 200, "terrain atlas: the checker's dark and light squares both reach the cell",
              std::to_string(darkest) + " and " + std::to_string(brightest));
        check(roughOk, "terrain atlas: the roughness file's red channel becomes the alpha");
        auto d1 = texel(withFiles, withFiles.diffuse, 1, 0, 5, 5);   // layer 1 keeps its own white diffuse
        check(d1[0] == 255 && std::abs(int(d1[3]) - 178) <= 1, "terrain atlas: other layers keep their own maps");
        auto ao0 = texel(withFiles, withFiles.normal, 0, 0, 5, 5);
        check(std::abs(int(ao0[3]) - 128) <= 2, "terrain atlas: the occlusion file's red channel becomes the normal alpha",
              std::to_string(int(ao0[3])));
        auto ao1 = texel(withFiles, withFiles.normal, 1, 0, 5, 5);
        check(ao1[3] == 255, "terrain atlas: a layer without occlusion stays unoccluded");
    }

    // A named file that does not exist is an error, not a silent white.
    std::vector<TerrainLayer> missing = defaultTerrainLayers();
    missing[0].normalFile = std::string(UKIYO_ROOT) + "/tests/data/no_such_file.png";
    TerrainAtlases none;
    err.clear();
    check(!buildTerrainAtlases(missing, &none, &err) && !err.empty(), "terrain atlas: a missing normal file is an error",
          err);
}

// Foliage (item 18, step 3): scatter rules, determinism, density, grouping, batch rows and meshes.
static void testFoliage() {
    const float kTwoPi = 6.2831853f;
    // A flat tile at normalised height 0.6: 65 samples, so 64 by 64 one-metre cells.
    Heightmap flat;
    flat.size = 65;
    flat.cellSize = 1.0f;
    flat.heightScale = 10.0f;
    flat.samples.assign(size_t(65) * 65, 0.6f);
    const std::vector<uint8_t> noField;

    FoliageType grass;
    grass.density = 1.0f;
    grass.slopeMax = 35.0f;
    grass.seed = 7;

    const auto full = scatterFoliage(flat, grass, noField);
    check(full.size() == 64 * 64, "foliage: density 1 on flat ground puts one instance in every cell",
          std::to_string(full.size()));
    bool inside = true;
    for (const FoliageInstance& f : full)
        if (f.pos.x < 0.0f || f.pos.x > 64.0f || f.pos.z < 0.0f || f.pos.z > 64.0f) inside = false;
    check(inside, "foliage: every instance is inside the tile");

    grass.density = 0.0f;
    check(scatterFoliage(flat, grass, noField).empty(), "foliage: density 0 places nothing");

    grass.density = 0.5f;
    const auto half = scatterFoliage(flat, grass, noField);
    grass.density = 0.25f;
    const auto quarter = scatterFoliage(flat, grass, noField);
    check(half.size() > 1900 && half.size() < 2200, "foliage: density 0.5 places about half the cells",
          std::to_string(half.size()));
    check(quarter.size() < half.size() && quarter.size() > 900 && quarter.size() < 1300,
          "foliage: density 0.25 places fewer than 0.5", std::to_string(quarter.size()));
    std::set<std::pair<float, float>> halfPos;
    for (const FoliageInstance& f : half) halfPos.insert({f.pos.x, f.pos.z});
    bool nested = true;
    for (const FoliageInstance& f : quarter)
        if (!halfPos.count({f.pos.x, f.pos.z})) nested = false;
    check(nested, "foliage: a lower density keeps a subset of a higher density's instances");

    // Same seed, same instances. Another seed moves them.
    grass.density = 0.5f;
    const auto again = scatterFoliage(flat, grass, noField);
    bool same = again.size() == half.size();
    for (size_t i = 0; same && i < half.size(); ++i)
        same = again[i].pos == half[i].pos && again[i].yaw == half[i].yaw && again[i].scale == half[i].scale;
    check(same, "foliage: the same seed gives the same instances");
    grass.seed = 8;
    const auto other = scatterFoliage(flat, grass, noField);
    bool differs = other.size() != half.size();
    for (size_t i = 0; !differs && i < half.size(); ++i) differs = !(other[i].pos == half[i].pos);
    check(differs, "foliage: another seed gives other instances");
    grass.seed = 7;

    // Height band: the tile is at 0.6, so a band below it places nothing and a band around it places all.
    grass.density = 1.0f;
    grass.heightMax = 0.5f;
    check(scatterFoliage(flat, grass, noField).empty(), "foliage: nothing above the height band");
    grass.heightMax = 1.0f;
    grass.heightMin = 0.55f;
    check(scatterFoliage(flat, grass, noField).size() == 64 * 64, "foliage: everything inside the height band");
    grass.heightMin = 0.0f;

    // Slope: a ramp rising 1 m per metre is 45 degrees everywhere.
    Heightmap ramp;
    ramp.size = 65;
    ramp.cellSize = 1.0f;
    ramp.heightScale = 64.0f;
    ramp.samples.resize(size_t(65) * 65);
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x) ramp.samples[size_t(z) * 65 + x] = float(x) / 64.0f;
    grass.slopeMax = 30.0f;
    check(scatterFoliage(ramp, grass, noField).empty(), "foliage: nothing on slopes steeper than slopeMax (45 deg ramp)");
    grass.slopeMax = 60.0f;
    check(scatterFoliage(ramp, grass, noField).size() == 64 * 64, "foliage: slopes under slopeMax are planted");
    grass.slopeMax = 35.0f;

    // Density field: zero blocks everything, full changes nothing, and a field of the wrong size places nothing.
    const std::vector<uint8_t> zeros(size_t(65) * 65, 0), fieldFull(size_t(65) * 65, 255);
    check(scatterFoliage(flat, grass, zeros).empty(), "foliage: a zero density field places nothing");
    check(scatterFoliage(flat, grass, fieldFull).size() == 64 * 64, "foliage: a full density field changes nothing");
    check(scatterFoliage(flat, grass, std::vector<uint8_t>(10, 255)).empty(), "foliage: a density field of the wrong size is refused");

    // Scale and yaw are inside their ranges.
    grass.scaleMin = 0.5f;
    grass.scaleMax = 2.0f;
    bool scaleOk = true, yawOk = true;
    for (const FoliageInstance& f : full) {
        if (f.scale < 0.5f || f.scale > 2.0f) scaleOk = false;
        if (f.yaw < 0.0f || f.yaw >= kTwoPi) yawOk = false;
    }
    const auto ranged = scatterFoliage(flat, grass, noField);
    scaleOk = scaleOk && !ranged.empty();
    for (const FoliageInstance& f : ranged)
        if (f.scale < 0.5f || f.scale > 2.0f || f.yaw < 0.0f || f.yaw >= kTwoPi) scaleOk = false;
    check(scaleOk && yawOk, "foliage: scale stays in its range and yaw in [0, 2 pi)");

    // Grouping: a 129-sample tile (128 cells) in 64-cell chunks is four groups of 4096 cells each.
    Heightmap big = flat;
    big.size = 129;
    big.samples.assign(size_t(129) * 129, 0.6f);
    FoliageType all;
    all.density = 1.0f;
    const auto bigList = scatterFoliage(big, all, noField);
    const auto groups = groupByChunk(bigList, 1.0f, 64, 2);
    size_t total = 0;
    for (const auto& g : groups) total += g.size();
    check(bigList.size() == 128 * 128 && total == bigList.size() && groups.size() == 4,
          "foliage: grouping by chunk keeps every instance", std::to_string(total));
    check(groups[0].size() == 4096 && groups[1].size() == 4096 && groups[3].size() == 4096,
          "foliage: each chunk gets the instances inside its cells");

    // Batch rows: a 2 m instance at (3, 4, 5) with no yaw. The scale is on the diagonal, the translation is in w.
    FoliageInstance one;
    one.pos = glm::vec3(3.0f, 4.0f, 5.0f);
    one.scale = 2.0f;
    one.yaw = 0.0f;
    std::vector<glm::vec4> rows(3 * kFoliageBatch);
    packFoliageRows(&one, 1, rows.data());
    check(rows[0] == glm::vec4(2, 0, 0, 3) && rows[1] == glm::vec4(0, 2, 0, 4) && rows[2] == glm::vec4(0, 0, 2, 5) &&
              rows[3] == glm::vec4(0.0f),
          "foliage: batch rows hold the 3 x 4 transform, and unused rows are zero");

    // Meshes: indices in range, and one normal per position.
    bool meshOk = true;
    for (FoliageShape s : {FoliageShape::Tuft, FoliageShape::Cone}) {
        const FoliageMesh m = buildFoliageMesh(s);
        meshOk = meshOk && !m.indices.empty() && m.indices.size() % 3 == 0 && m.normals.size() == m.positions.size();
        for (uint32_t i : m.indices) meshOk = meshOk && i < m.positions.size();
    }
    check(meshOk, "foliage: tuft and cone meshes have valid indices and normals");

    // Density maps: the checker file is read at the tile size, and a missing file is an error.
    std::vector<uint8_t> field;
    std::string err;
    check(loadDensityField(std::string(UKIYO_ROOT) + "/tests/data/terrain_checker_64.png", 65, &field, &err) &&
              field.size() == size_t(65) * 65,
          "foliage: a density map is resampled to the tile size", err);
    err.clear();
    check(!loadDensityField(std::string(UKIYO_ROOT) + "/tests/data/no_such_density.png", 65, &field, &err) && !err.empty(),
          "foliage: a missing density map is an error", err);
}

// Grass (item 19): the tuft mesh, the scatter's masks, the distance bands and the packed rows. The scatter is CPU work
// and the bands are a CPU-side rule the shader repeats, so both can be tested without a device.
static void testGrass() {
    // A flat tile at normalised height 0.3: 65 samples, so 64 by 64 one-metre cells, and 64 m of ground per side.
    Heightmap flat;
    flat.size = 65;
    flat.cellSize = 1.0f;
    flat.heightScale = 10.0f;
    flat.samples.assign(size_t(65) * 65, 0.3f);
    const std::vector<uint8_t> noField;

    // Two layers that split the tile cleanly by height: the low one covers the flat ground, the high one nothing.
    std::vector<TerrainLayer> layers(2);
    layers[0].name = "low";
    layers[0].heightMin = 0.0f;
    layers[0].heightMax = 0.5f;
    layers[0].heightFade = 0.0f;
    layers[0].slopeMin = 0.0f;
    layers[0].slopeMax = 90.0f;
    layers[0].slopeFade = 0.0f;
    layers[1] = layers[0];
    layers[1].name = "high";
    layers[1].heightMin = 0.6f;
    layers[1].heightMax = 1.0f;

    GrassSettings g;
    g.enabled = true;
    g.density = 0.5f;      // 64 m of ground, so about 2048 candidates at full density
    g.includeLayers = 0u;  // no layer test until one is asked for
    g.excludeLayers = 0u;
    const std::vector<uint8_t> noPaint;

    const auto full = scatterGrass(flat, layers, noPaint, g, noField);
    check(!full.empty() && full.size() > 1800 && full.size() < 2300,
          "grass: a half-density scatter fills the tile with about 2000 tufts", std::to_string(full.size()));
    bool inside = true;
    for (const GrassInstance& b : full)
        if (b.pos.x < 0.0f || b.pos.x > 64.0f || b.pos.z < 0.0f || b.pos.z > 64.0f ||
            std::fabs(b.pos.y - 3.0f) > 1e-4f)
            inside = false;
    check(inside, "grass: every tuft stands on the ground inside the tile");

    g.density = 0.0f;
    check(scatterGrass(flat, layers, noPaint, g, noField).empty(), "grass: density 0 places nothing");
    g.density = 0.5f;

    // Determinism, and a second seed moving the tufts.
    const auto again = scatterGrass(flat, layers, noPaint, g, noField);
    bool same = again.size() == full.size();
    for (size_t i = 0; same && i < full.size(); ++i)
        same = again[i].pos == full[i].pos && again[i].yaw == full[i].yaw && again[i].scale == full[i].scale;
    check(same, "grass: the same seed gives the same tufts");
    g.seed = 11;
    const auto other = scatterGrass(flat, layers, noPaint, g, noField);
    bool differs = other.size() != full.size();
    for (size_t i = 0; !differs && i < full.size(); ++i) differs = !(other[i].pos == full[i].pos);
    check(differs, "grass: another seed gives other tufts");
    g.seed = 3;

    // The detail map scales the density: zero clears the tile, full changes nothing.
    check(scatterGrass(flat, layers, noPaint, g, std::vector<uint8_t>(size_t(65) * 65, 0)).empty(),
          "grass: a zero detail map places nothing");
    check(scatterGrass(flat, layers, noPaint, g, std::vector<uint8_t>(size_t(65) * 65, 255)).size() == full.size(),
          "grass: a full detail map changes nothing");
    check(scatterGrass(flat, layers, noPaint, g, std::vector<uint8_t>(10, 255)).empty(),
          "grass: a detail map of the wrong size is refused");

    // Layer masks: layer 0 covers the flat ground, layer 1 does not. Including one grows grass, the other clears it.
    g.includeLayers = 1u << 0;
    check(scatterGrass(flat, layers, noPaint, g, noField).size() == full.size(),
          "grass: including the layer under the tufts changes nothing");
    g.includeLayers = 1u << 1;
    check(scatterGrass(flat, layers, noPaint, g, noField).empty(),
          "grass: including only a layer that is not there clears the tile");
    g.includeLayers = 0u;
    g.excludeLayers = 1u << 0;
    check(scatterGrass(flat, layers, noPaint, g, noField).empty(),
          "grass: excluding the layer under the tufts clears the tile");
    g.excludeLayers = 1u << 1;
    check(scatterGrass(flat, layers, noPaint, g, noField).size() == full.size(),
          "grass: excluding an absent layer changes nothing");
    g.excludeLayers = 0u;

    // A paint map a brush writes: with both layers matching the ground, painting layer 0 off moves every sample's
    // whole weight to layer 1, so including layer 0 clears the tile and including layer 1 plants all of it.
    std::vector<TerrainLayer> bothLayers = layers;
    bothLayers[1].heightMin = 0.0f;
    bothLayers[1].heightMax = 1.0f;
    std::vector<uint8_t> paint = std::vector<uint8_t>(size_t(65) * 65 * kTerrainMaxLayers, 0);
    for (size_t s = 0; s < size_t(65) * 65; ++s) paint[s * kTerrainMaxLayers + 1] = 255;
    g.includeLayers = 1u << 0;
    check(scatterGrass(flat, bothLayers, paint, g, noField).empty(),
          "grass: the layer paint a brush writes clears the grass where layer 0 is painted off");
    g.includeLayers = 1u << 1;
    check(scatterGrass(flat, bothLayers, paint, g, noField).size() == full.size(),
          "grass: the same paint plants every tuft once the layer that took the weight is included");
    g.includeLayers = 0u;

    // Height and slope limits. The ramp rises 1 m per metre, so it is 45 degrees everywhere.
    g.heightMax = 0.2f;
    check(scatterGrass(flat, layers, noPaint, g, noField).empty(), "grass: nothing above the height band");
    g.heightMax = 1.0f;
    Heightmap ramp = flat;
    ramp.heightScale = 64.0f;   // one metre of rise per metre of ground, so 45 degrees everywhere
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x) ramp.samples[size_t(z) * 65 + x] = float(x) / 64.0f;
    g.slopeMax = 30.0f;
    check(scatterGrass(ramp, layers, noPaint, g, noField).empty(), "grass: nothing on a 45 degree ramp past slopeMax");
    g.slopeMax = 60.0f;
    check(scatterGrass(ramp, layers, noPaint, g, noField).size() > 1800, "grass: the same ramp is planted past slopeMax");
    g.slopeMax = 35.0f;

    // Scale and ground alignment stay in their ranges.
    bool ranges = true;
    for (const GrassInstance& b : full) {
        if (b.yaw < 0.0f || b.yaw >= 6.2831853f) ranges = false;
        if (b.scale < g.scaleMin || b.scale > g.scaleMax) ranges = false;
        if (std::fabs(glm::length(b.up) - 1.0f) > 1e-4f) ranges = false;
        if (b.up.y < 0.0f) ranges = false;
    }
    check(ranges, "grass: scale, yaw and the up vector stay in their ranges");

    // The bands: full inside bandNear, half at bandMid, gone at cullDistance, and never rising with distance.
    g.bandNear = 20.0f;
    g.bandMid = 50.0f;
    g.cullDistance = 100.0f;
    check(std::fabs(grassDensityAt(0.0f, g) - 1.0f) < 1e-6f && std::fabs(grassDensityAt(20.0f, g) - 1.0f) < 1e-6f,
          "grass: the near band keeps every blade");
    check(std::fabs(grassDensityAt(50.0f, g) - 0.5f) < 1e-4f, "grass: the mid band keeps half");
    check(grassDensityAt(100.0f, g) < 1e-5f && grassDensityAt(160.0f, g) < 1e-5f, "grass: past the cull distance nothing is kept");
    bool monotone = true;
    float last = 1.0f;
    for (float d = 0.0f; d <= 120.0f; d += 0.5f) {
        const float v = grassDensityAt(d, g);
        if (v > last + 1e-6f) monotone = false;
        last = v;
    }
    check(monotone, "grass: density never rises with distance, so a near view keeps every far blade");

    // The shader's rule, applied on the CPU: the blades kept at 90 m are a subset of the ones kept at 45 m.
    g.cullDistance = 200.0f;
    g.bandMid = 120.0f;
    size_t keptNear = 0, keptFar = 0, keptFarInsideNear = 0;
    for (int i = 0; i < 4000; ++i) {
        const glm::vec2 p(float(i % 61) * 1.7f, float(i / 61) * 2.3f);
        const float h = grassBladeHash(p);
        const bool near = h < grassDensityAt(45.0f, g);
        const bool far = h < grassDensityAt(90.0f, g);
        keptNear += near ? 1 : 0;
        keptFar += far ? 1 : 0;
        keptFarInsideNear += (far && !near) ? 1 : 0;
    }
    check(keptNear > keptFar && keptFar > 0 && keptFarInsideNear == 0,
          "grass: the far band draws a subset of the near band, never the other way round",
          std::to_string(keptNear) + " then " + std::to_string(keptFar));

    // Grouping by chunk, on a 129-sample tile with 64-cell chunks.
    Heightmap big = flat;
    big.size = 129;
    big.samples.assign(size_t(129) * 129, 0.3f);
    const auto bigList = scatterGrass(big, layers, noPaint, g, noField);
    const auto groups = groupGrassByChunk(bigList, 1.0f, 64, 2);
    size_t total = 0;
    for (const auto& grp : groups) total += grp.size();
    check(groups.size() == 4 && total == bigList.size(), "grass: grouping by chunk keeps every tuft",
          std::to_string(total));

    // The instance budget: a dense tile is thinned, and the result is still deterministic.
    GrassSettings dense = g;
    dense.density = 64.0f;   // 512 by 512 candidates on this tile, three and a half times the budget
    dense.bandNear = 0.0f;
    dense.bandMid = 0.0f;
    dense.cullDistance = 1.0f;   // the bands are the shader's business; the scatter must not care
    const auto capped = scatterGrass(flat, layers, noPaint, dense, noField);
    const auto cappedAgain = scatterGrass(flat, layers, noPaint, dense, noField);
    check(capped.size() <= kGrassMaxInstances && capped.size() > kGrassMaxInstances / 4,
          "grass: a tile past the instance budget is thinned to it", std::to_string(capped.size()));
    bool cappedSame = capped.size() == cappedAgain.size();
    for (size_t i = 0; cappedSame && i < capped.size(); ++i) cappedSame = capped[i].pos == cappedAgain[i].pos;
    check(cappedSame, "grass: the budget's subset is the same every build");

    // The mesh: 3 blades of 4 segments, uv.y from root to tip, one normal per vertex, indices in range.
    GrassSettings meshSettings;   // three blades of four quads, so the counts below are easy to read
    meshSettings.blades = 3;
    meshSettings.segments = 4;
    const GrassMesh mesh = buildGrassMesh(meshSettings);
    check(mesh.positions.size() == 3 * 5 * 2 && mesh.indices.size() == 3 * 4 * 6,
          "grass: the tuft mesh is three blades of four quads", std::to_string(mesh.positions.size()));
    bool meshOk = mesh.normals.size() == mesh.positions.size() && mesh.uvs.size() == mesh.positions.size();
    float rootWidth = 0.0f;
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        meshOk = meshOk && std::fabs(glm::length(mesh.normals[i]) - 1.0f) < 1e-4f;
        meshOk = meshOk && mesh.uvs[i].y >= 0.0f && mesh.uvs[i].y <= 1.0f;
        if (mesh.uvs[i].y < 1e-4f) rootWidth = std::max(rootWidth, mesh.positions[i].y);
    }
    for (uint32_t i : mesh.indices) meshOk = meshOk && i < mesh.positions.size();
    check(meshOk && rootWidth < 1e-4f, "grass: the tuft mesh has unit normals, tips at uv.y 1 and roots at 0");
    // Vertices come in pairs (one across the blade) per row, so a pair's distance is the blade's width at that height.
    bool taper = true;
    for (uint32_t b = 0; b < 3; ++b) {
        const size_t base = size_t(b) * 5 * 2;
        const float root = glm::distance(mesh.positions[base], mesh.positions[base + 1]);
        const float tip = glm::distance(mesh.positions[base + 8], mesh.positions[base + 9]);
        taper = taper && root > meshSettings.width * 0.9f && tip < root * 0.25f;
    }
    check(taper, "grass: a blade starts at its full width and tapers to a point");

    // Packed rows: with no yaw and no ground alignment the basis is a swap of two axes, so a local point maps to a
    // known world point. Checked through the same dots the shader does, which is the part that can actually be wrong.
    GrassInstance one;
    one.pos = glm::vec3(2.0f, 5.0f, 7.0f);
    one.up = glm::vec3(0.0f, 1.0f, 0.0f);
    one.yaw = 0.0f;
    one.scale = 2.0f;
    std::vector<glm::vec4> rows(3 * kGrassBatch);
    packGrassRows(&one, 1, rows.data());
    auto transform = [&rows](glm::vec3 local) {
        return glm::vec3(glm::dot(rows[0], glm::vec4(local, 1.0f)), glm::dot(rows[1], glm::vec4(local, 1.0f)),
                         glm::dot(rows[2], glm::vec4(local, 1.0f)));
    };
    const glm::vec3 root = transform(glm::vec3(0.0f));
    const glm::vec3 tip = transform(glm::vec3(0.0f, 1.0f, 0.0f));
    check(glm::length(root - one.pos) < 1e-5f && glm::length(tip - (one.pos + glm::vec3(0.0f, 2.0f, 0.0f))) < 1e-5f,
          "grass: the packed rows put the root at the tuft and the tip two metres up");
    check(glm::length(transform(glm::vec3(1.0f, 0.0f, 0.0f)) - one.pos) > 1.9f &&
              rows[3] == glm::vec4(0.0f),
          "grass: a local side point is scaled by the tuft, and unused rows are zero");

    // A tilted tuft keeps its up vector: the shader's normal is the basis applied to the mesh normal.
    GrassInstance tilted = one;
    tilted.up = glm::normalize(glm::vec3(0.6f, 0.8f, 0.0f));
    packGrassRows(&tilted, 1, rows.data());
    const glm::vec3 upAxis(glm::dot(glm::vec3(rows[0]), glm::vec3(0.0f, 1.0f, 0.0f)),
                           glm::dot(glm::vec3(rows[1]), glm::vec3(0.0f, 1.0f, 0.0f)),
                           glm::dot(glm::vec3(rows[2]), glm::vec3(0.0f, 1.0f, 0.0f)));
    check(glm::length(upAxis - tilted.up * 2.0f) < 1e-4f,
          "grass: a tuft on a slope leans with the ground normal it was given");
}

// Terrain editing (item 18, steps 4 and 5): brush weights and edits (with exact undo), noise, terrace, stamps, erosion,
// paint masks, the ray pick, and a density mask that limits the scatter.
static void testTerrainEdit() {
    // Weight: full inside the hard part, zero at the radius, and falling in between.
    BrushDab d;
    d.radius = 10.0f;
    d.falloff = 0.5f;
    check(brushWeight(d, 0.0f) == 1.0f && brushWeight(d, 5.0f) == 1.0f && brushWeight(d, 10.0f) == 0.0f,
          "terrain brush: full weight inside the hard part, none at the radius");
    check(brushWeight(d, 6.0f) > brushWeight(d, 8.0f) && brushWeight(d, 8.0f) > 0.0f,
          "terrain brush: the weight falls smoothly towards the edge");

    // Height dab: raises the centre, leaves the outside alone, and the undo restores every sample exactly.
    Heightmap flat;
    flat.size = 65;
    flat.cellSize = 1.0f;
    flat.heightScale = 10.0f;
    flat.samples.assign(size_t(65) * 65, 0.5f);
    BrushDab hd;
    hd.centre = glm::vec2(32.0f, 32.0f);
    hd.radius = 10.0f;
    hd.falloff = 0.3f;
    hd.strength = 1.0f;
    auto raise = heightDab(flat, hd, [](int, int, float h) { return h + 0.1f; });
    Heightmap edited = flat;
    applyHeightEdits(edited, raise, true);
    check(!raise.empty() && std::fabs(edited.samples[32 * 65 + 32] - 0.6f) < 1e-5f,
          "terrain brush: a dab at full strength moves the centre to the target",
          std::to_string(edited.samples[32 * 65 + 32]));
    check(edited.samples[0] == 0.5f && edited.samples[64 * 65 + 64] == 0.5f,
          "terrain brush: samples outside the radius are unchanged");
    applyHeightEdits(edited, raise, false);
    bool restored = true;
    for (size_t i = 0; i < flat.samples.size(); ++i) restored = restored && edited.samples[i] == flat.samples[i];
    check(restored, "terrain brush: the undo (before values) restores the heights exactly");
    hd.strength = 0.5f;
    auto half = heightDab(flat, hd, [](int, int, float h) { return h + 0.1f; });
    check(!half.empty() && half[0].after > 0.5f && half[0].after < 0.6f,
          "terrain brush: strength scales the move", half.empty() ? "" : std::to_string(half[0].after));

    // Noise: bounded, deterministic for a seed, and different for another seed.
    bool bounded = true;
    for (int i = 0; i < 400; ++i) {
        const float v = fractalNoise(i * 0.37f, i * 0.91f, 5, 4);
        bounded = bounded && v >= -1.0f && v <= 1.0f;
    }
    check(bounded, "terrain noise: fractal noise stays in [-1, 1]");
    check(fractalNoise(3.3f, 7.1f, 5, 4) == fractalNoise(3.3f, 7.1f, 5, 4) &&
              fractalNoise(3.3f, 7.1f, 5, 4) != fractalNoise(3.3f, 7.1f, 6, 4),
          "terrain noise: the same seed gives the same value, another seed another");

    // Terrace: quantised to the steps.
    check(std::fabs(terraceLevel(0.37f, 4) - 0.25f) < 1e-6f && terraceLevel(1.0f, 4) == 1.0f &&
              terraceLevel(0.0f, 4) == 0.0f,
          "terrain terrace: heights round down to the step levels");

    // Stamps: the dome peaks in the middle and is zero at the rim; a stamp image is read as grey.
    Stamp dome = domeStamp(33);
    BrushDab sd;
    sd.centre = glm::vec2(0.0f);
    sd.radius = 10.0f;
    check(stampAt(dome, sd, 0.0f, 0.0f) > 0.95f && stampAt(dome, sd, 9.9f, 0.0f) < 0.1f &&
              stampAt(dome, sd, 11.0f, 0.0f) == 0.0f,
          "terrain stamp: the dome peaks at the centre and falls to zero at the rim");
    Stamp grey;
    std::string serr;
    check(loadStamp(std::string(UKIYO_ROOT) + "/tests/data/terrain_grey_64.png", &grey, &serr) && grey.size == 64 &&
              grey.values.size() == size_t(64) * 64,
          "terrain stamp: a square image is read as a stamp", serr);

    // Erosion: changes some heights, keeps them in range, is deterministic, and the undo restores the map.
    Heightmap hills;
    hills.size = 129;
    hills.cellSize = 1.0f;
    hills.heightScale = 20.0f;
    hills.samples.resize(size_t(129) * 129);
    for (uint32_t z = 0; z < 129; ++z)
        for (uint32_t x = 0; x < 129; ++x)
            hills.samples[size_t(z) * 129 + x] = 0.5f + 0.4f * fractalNoise(x * 0.06f, z * 0.06f, 9, 3);
    ErosionSettings es;
    es.droplets = 6000;
    es.seed = 4;
    auto eroded = erodeHeightmap(hills, es);
    check(eroded.size() > 100, "terrain erosion: droplets change the heights", std::to_string(eroded.size()));
    bool inRange = true;
    for (const HeightEdit& e : eroded) inRange = inRange && e.after >= 0.0f && e.after <= 1.0f && std::isfinite(e.after);
    check(inRange, "terrain erosion: every height stays in [0, 1]");
    auto again = erodeHeightmap(hills, es);
    check(again.size() == eroded.size() && (again.empty() || again[0].after == eroded[0].after),
          "terrain erosion: the same seed gives the same result");
    Heightmap worn = hills;
    applyHeightEdits(worn, eroded, true);
    applyHeightEdits(worn, eroded, false);
    bool same = true;
    for (size_t i = 0; i < hills.samples.size(); ++i) same = same && worn.samples[i] == hills.samples[i];
    check(same, "terrain erosion: the undo restores the heightmap exactly");

    // Paint masks: a layer's byte is set at the centre and not in the other layers, and the undo clears it.
    const uint32_t size = 65;
    std::vector<uint8_t> paint(size_t(size) * size * kTerrainMaxLayers, 0);
    BrushDab pd;
    pd.centre = glm::vec2(32.0f, 32.0f);
    pd.radius = 6.0f;
    pd.falloff = 0.5f;
    auto painted = paintDab(paint, size, kTerrainMaxLayers, 2, pd, 255, 1.0f);
    applyPaintEdits(paint, painted, true);
    const size_t centre = (size_t(32) * size + 32) * kTerrainMaxLayers;
    check(!painted.empty() && paint[centre + 2] == 255 && paint[centre + 1] == 0,
          "terrain paint: a mask dab sets its layer at the centre and no other layer");
    check(paint[0] == 0, "terrain paint: the mask is unchanged away from the dab");
    applyPaintEdits(paint, painted, false);
    check(std::all_of(paint.begin(), paint.end(), [](uint8_t v) { return v == 0; }),
          "terrain paint: the undo clears the mask exactly");

    // Strokes: two dabs merged into one record. Undo returns every sample to where the stroke found it, and redo
    // (after values) gives the same map as the two dabs applied in turn.
    Heightmap stroke = flat;
    Heightmap stepwise = flat;
    std::vector<HeightEdit> record;
    BrushDab first = hd;
    first.centre = glm::vec2(30.0f, 32.0f);
    BrushDab second = hd;
    second.centre = glm::vec2(34.0f, 32.0f);
    auto up = [](int, int, float h) { return h + 0.2f; };
    auto e1 = heightDab(stroke, first, up);
    applyHeightEdits(stroke, e1, true);
    mergeHeightEdits(record, e1);
    auto e2 = heightDab(stroke, second, up);
    applyHeightEdits(stroke, e2, true);
    mergeHeightEdits(record, e2);
    applyHeightEdits(stepwise, e1, true);
    applyHeightEdits(stepwise, heightDab(stepwise, second, up), true);
    bool sameAfter = true;
    for (size_t i = 0; i < flat.samples.size(); ++i) sameAfter = sameAfter && stroke.samples[i] == stepwise.samples[i];
    check(sameAfter, "terrain stroke: the merged record gives the same map as the dabs in turn");
    Heightmap undone = stroke;
    applyHeightEdits(undone, record, false);
    bool back = true;
    for (size_t i = 0; i < flat.samples.size(); ++i) back = back && undone.samples[i] == flat.samples[i];
    check(back && record.size() < e1.size() + e2.size(), "terrain stroke: undo of the merged record restores the start");

    // Ray pick: straight down onto a flat tile at 5 m, and a slanted ray that lands 10 m along x.
    Heightmap plane = flat;   // 5 m at heightScale 10 with 0.5 samples
    glm::vec3 hit;
    check(raycastHeightmap(plane, glm::vec3(20.0f, 30.0f, 20.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f, &hit) &&
              std::fabs(hit.y - 5.0f) < 1e-3f && std::fabs(hit.x - 20.0f) < 1e-3f,
          "terrain pick: a ray down hits the surface at its height");
    check(raycastHeightmap(plane, glm::vec3(-5.0f, 20.0f, 32.0f), glm::vec3(1.0f, -1.0f, 0.0f), 100.0f, &hit) &&
              std::fabs(hit.x - 10.0f) < 0.05f && std::fabs(hit.y - 5.0f) < 0.05f,
          "terrain pick: a slanted ray lands where it crosses the surface");
    check(!raycastHeightmap(plane, glm::vec3(20.0f, 30.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f, &hit),
          "terrain pick: a ray pointing away misses");

    // Density mask: painted density only, so the scatter lands inside the painted disk and nowhere else.
    std::vector<uint8_t> density(size_t(size) * size, 0);
    BrushDab dd;
    dd.centre = glm::vec2(32.0f, 32.0f);
    dd.radius = 8.0f;
    dd.falloff = 0.5f;
    applyPaintEdits(density, paintDab(density, size, 1, 0, dd, 255, 1.0f), true);
    FoliageType trees;
    trees.density = 1.0f;
    trees.slopeMax = 90.0f;
    auto placed = scatterFoliage(plane, trees, density);
    bool inDisk = !placed.empty();
    for (const FoliageInstance& f : placed)
        inDisk = inDisk && glm::distance(glm::vec2(f.pos.x, f.pos.z), glm::vec2(32.0f, 32.0f)) < 8.0f + 1.5f;
    check(inDisk, "terrain density mask: instances appear only where the mask is painted", std::to_string(placed.size()));
}

static void testTerrain() {
    Heightmap flat;
    flat.size = 65;
    flat.cellSize = 1.0f;
    flat.heightScale = 2.0f;
    flat.samples.assign(65 * 65, 0.5f);   // every sample is 1 m high
    check(flat.valid() && chunksPerSide(flat, 64) == 1 && chunksPerSide(flat, 16) == 4 && chunksPerSide(flat, 10) == 0,
          "terrain: chunks divide the grid, and a size that does not divide gives none");
    check(std::fabs(flat.heightAtMetres(10.3f, 40.7f) - 1.0f) < 1e-5f, "terrain: a flat map reads 1 m everywhere");

    const ChunkMesh c = buildChunk(flat, 0, 0, 64);
    bool indicesOk = c.indices.size() == size_t(64) * 64 * 6 && c.positions.size() == size_t(65) * 65;
    for (uint32_t i : c.indices) indicesOk = indicesOk && i < c.positions.size();
    check(indicesOk, "terrain: a chunk has (cells + 1)^2 vertices and six indices per cell, all in range");
    bool up = true;
    for (const glm::vec3& n : c.normals) up = up && std::fabs(n.y - 1.0f) < 1e-5f;
    check(up, "terrain: a flat chunk has upward normals");
    bool ccw = true;   // every triangle's normal points up, which is counter-clockwise seen from above
    for (size_t k = 0; k + 2 < c.indices.size(); k += 3) {
        const glm::vec3& a = c.positions[c.indices[k]];
        const glm::vec3& b = c.positions[c.indices[k + 1]];
        const glm::vec3& d = c.positions[c.indices[k + 2]];
        ccw = ccw && glm::dot(glm::cross(b - a, d - a), glm::vec3(0, 1, 0)) > 0.0f;
    }
    check(ccw, "terrain: triangles face up");
    check(buildChunk(flat, 1, 0, 64).positions.empty() && buildChunk(flat, 0, 0, 10).positions.empty(),
          "terrain: a chunk outside the grid, or a bad chunk size, is empty");

    // A ramp rising along x, two chunks wide. The edge at x = 64 must match exactly between the two chunks.
    Heightmap ramp;
    ramp.size = 129;
    ramp.cellSize = 0.5f;
    ramp.heightScale = 10.0f;
    ramp.samples.resize(size_t(129) * 129);
    for (uint32_t z = 0; z < 129; ++z)
        for (uint32_t x = 0; x < 129; ++x) ramp.samples[size_t(z) * 129 + x] = float((x * 7 + z * 13) % 17) / 16.0f;
    const ChunkMesh left = buildChunk(ramp, 0, 0, 64);
    const ChunkMesh right = buildChunk(ramp, 1, 0, 64);
    bool seam = left.positions.size() == right.positions.size() && !left.positions.empty();
    for (uint32_t j = 0; j < 65 && seam; ++j)
        seam = left.positions[j * 65 + 64] == right.positions[j * 65];
    check(seam, "terrain: neighbouring chunks share their edge exactly");
    check(std::fabs(ramp.heightAtMetres(32.0f, 16.0f) - ramp.heightAt(64, 32)) < 1e-4f,
          "terrain: a point on a sample reads that sample's height");

    // The raw file round trip. Heights come back within the 16-bit step, and a wrong size is refused.
    const std::string tmp = (std::filesystem::temp_directory_path() / "ukiyo_terrain_test.u16").string();
    std::string err;
    check(saveHeightmapU16(tmp, ramp, &err), "terrain: a heightmap saves to a raw file", err);
    Heightmap back;
    back.size = 0;
    back.cellSize = ramp.cellSize;
    back.heightScale = ramp.heightScale;
    check(loadHeightmapU16(tmp, 129, &back, &err) && back.samples.size() == ramp.samples.size(),
          "terrain: the raw file loads at its size", err);
    float worst = 0.0f;
    for (size_t k = 0; k < ramp.samples.size() && back.samples.size() == ramp.samples.size(); ++k)
        worst = std::max(worst, std::fabs(back.samples[k] - ramp.samples[k]));
    check(worst <= 1.0f / 65535.0f + 1e-6f, "terrain: heights survive the 16-bit round trip", num(worst));
    Heightmap wrong;
    check(!loadHeightmapU16(tmp, 65, &wrong, &err) && !err.empty(), "terrain: a file of the wrong size is refused");
    std::remove(tmp.c_str());

    // A chunk's collider is a mesh of the same triangles. A ray straight down hits it at the plateau height.
    PhysicsWorld pw;
    pw.addStaticMesh(c.positions, c.indices);
    glm::vec3 hit(0.0f);
    const bool landed = pw.raycast(glm::vec3(20.0f, 10.0f, 20.0f), glm::vec3(0, -1, 0), 20.0f, &hit);
    check(landed && std::fabs(hit.y - 1.0f) < 0.05f, "terrain: a ray down onto a chunk hits its surface",
          num(hit.y));
}

int main() {
    testPhysics();
    testActions();
    testWorldInput();
    testNavigation();
    testAudio();
    testAnimState();
    testScript();
    testWorld();
    testWorldAnimation();
    testWorldNavigation();
    testVfx();
    testSceneSwitch();
    testPhysicsInterpolation();
    testLuaMath();
    testVisibility();
    testContacts();
    testResourcePack();
    testEntityHandles();
    testContactFilter();
    testWater();
    testBuoyancy();
    testLod();
    testLodSwitches();
    testTerrain();
    testTerrainLayers();
    testTerrainAtlases();
    testFoliage();
    testGrass();
    testTerrainEdit();
    std::printf("systems: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
