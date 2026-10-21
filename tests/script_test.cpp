// Tests for the Lua scripting system: the sandbox, require(), error reporting, the per-entity runtime, and the files a
// project needs for an external editor to understand it.
//
// No device and no window: everything here is the engine's CPU code plus Lua 5.4.

#include "engine/script_project.h"
#include "systems/script.h"
#include "systems/script_runtime.h"
#include "systems/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using namespace ukiyo;

namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string& what, const std::string& detail = {}) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("FAIL  %s", what.c_str());
    if (!detail.empty()) std::printf("  -- %s", detail.c_str());
    std::printf("\n");
}

static fs::path testRoot() {
    static const fs::path root = fs::temp_directory_path() / "ukiyo_script_test";
    return root;
}

static std::string pathOf(const std::string& rel) { return (testRoot() / rel).string(); }

static void makeTree() {
    std::error_code ec;
    fs::remove_all(testRoot(), ec);
    fs::create_directories(testRoot() / "mods" / "sub", ec);
}

static bool writeFile(const std::string& rel, const std::string& text) {
    const fs::path p = testRoot() / rel;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

// --- the sandbox --------------------------------------------------------------------------------------------

static void testSandbox() {
    const char* probe =
        "assert(io == nil, 'io is open')\n"
        "assert(os == nil, 'os is open')\n"
        "assert(package == nil, 'package is open')\n"
        "assert(debug == nil, 'debug is open')\n"
        "assert(dofile == nil and loadfile == nil and load == nil, 'a file loader is open')\n";
    {
        ScriptHost h;
        std::string err;
        check(h.loadString(probe, "@probe.lua", &err), "sandbox: no io, os, package, debug or file loaders", err);
    }
    const char* safe =
        "assert(type(string) == 'table' and type(table) == 'table' and type(math) == 'table')\n"
        "assert(type(utf8) == 'table' and type(coroutine) == 'table')\n"
        "assert(type(require) == 'function', 'require is not installed')\n";
    ScriptHost h;
    std::string err;
    check(h.loadString(safe, "@safe.lua", &err), "sandbox: the pure libraries and require() are there", err);
}

// --- errors -------------------------------------------------------------------------------------------------

static void testErrors() {
    ScriptHost h;
    h.setEntityName("hero");
    std::string err;
    check(h.loadString("function onUpdate(dt)\n  local t = nil\n  return t.x\nend\n", "@hero.lua", &err),
          "errors: a script with a runtime fault loads", err);
    h.callUpdate(0.016f);
    check(h.error().file == "hero.lua", "errors: the file is the chunk's", h.error().file);
    check(h.error().line == 3, "errors: the line is the line that failed", std::to_string(h.error().line));
    check(h.error().message.find("nil value") != std::string::npos, "errors: the message says what went wrong",
          h.error().message);
    check(h.error().traceback.find("stack traceback") != std::string::npos ||
              h.error().traceback.find("onUpdate") != std::string::npos,
          "errors: a traceback comes with it", h.error().traceback);
    check(h.lastError().find("hero.lua:3:") == 0, "errors: lastError reads file:line", h.lastError());

    // The budget: three failures in a row stop the script, and a success in between resets the count.
    ScriptHost once;
    std::string e2;
    check(once.loadString("function onUpdate(dt) error('boom') end", "@boom.lua", &e2), "errors: the doomed script loads",
          e2);
    for (int i = 0; i < ScriptHost::kMaxErrors; ++i) once.callUpdate(0.016f);
    check(once.hasFailed(), "errors: three failures in a row stop the script");
    check(once.errorCount() == ScriptHost::kMaxErrors, "errors: and it counted them",
          std::to_string(once.errorCount()));
    once.callUpdate(0.016f);
    check(once.errorCount() == ScriptHost::kMaxErrors, "errors: a stopped script is not called again");

    ScriptHost alternating;
    std::string e3;
    check(alternating.loadString("n = 0\nfunction onUpdate(dt) n = n + 1 if n % 2 == 1 then error('odd') end end",
                                 "@odd.lua", &e3),
          "errors: the alternating script loads", e3);
    for (int i = 0; i < 5; ++i) alternating.callUpdate(0.016f);
    check(!alternating.hasFailed() && alternating.errorCount() == 3,
          "errors: a call that succeeds resets the budget", std::to_string(alternating.errorCount()));

    // A compile error is a check, not a run: it names the file and line and executes nothing.
    ScriptHost compile;
    std::string bad = "local = = 1\n";
    check(!compile.compileString(bad, "@bad.lua", &err), "errors: a syntax error is reported");
    check(err.find("bad.lua:1:") != std::string::npos, "errors: the syntax error names the file and line", err);
    check(!compile.loaded(), "errors: a script that did not compile is not loaded");
}

// --- require ------------------------------------------------------------------------------------------------

static void testRequire() {
    makeTree();
    check(writeFile("mods/counter.lua", "COUNT = (COUNT or 0) + 1\nreturn { value = 42, count = COUNT }\n"),
          "require: the test module is written");
    check(writeFile("mods/sub/leaf.lua", "return { name = 'leaf' }\n"), "require: the nested module is written");
    check(writeFile("mods/cycle_a.lua", "local b = require('cycle_b')\nreturn { name = 'a' }\n"),
          "require: the cycle's first half is written");
    check(writeFile("mods/cycle_b.lua", "local a = require('cycle_a')\nreturn { name = 'b' }\n"),
          "require: the cycle's second half is written");

    ScriptHost h;
    h.setSearchPaths({(testRoot() / "mods").string()});
    std::string err;
    const char* code =
        "local a = require('counter')\n"
        "local b = require('counter')\n"
        "assert(a == b, 'a module is loaded twice')\n"
        "assert(COUNT == 1, 'the module body ran twice')\n"
        "local leaf = require('sub.leaf')\n"
        "assert(leaf.name == 'leaf', 'a dotted name did not become a path')\n";
    check(h.loadString(code, "@uses_modules.lua", &err), "require: a module loads once and a dotted name resolves", err);

    ScriptHost cycle;
    cycle.setSearchPaths({(testRoot() / "mods").string()});
    std::string e2;
    check(cycle.loadString("local a = require('cycle_a')\nassert(a.name == 'a')\n", "@cycle.lua", &e2),
          "require: a cycle is answered with the half-built module, not a hang", e2);

    ScriptHost escapes;
    escapes.setSearchPaths({(testRoot() / "mods").string()});
    std::string e3;
    check(escapes.loadString("local ok = pcall(require, '../outside')\nassert(not ok, 'a traversal was allowed')\n"
                             "local ok2 = pcall(require, '/etc/passwd')\nassert(not ok2, 'an absolute path was allowed')\n",
                             "@escape.lua", &e3),
          "require: .. and an absolute path are refused", e3);

    ScriptHost missing;
    missing.setSearchPaths({(testRoot() / "mods").string()});
    missing.loadString("function onStart() require('nope') end", "@missing.lua", &e3);
    missing.callStart();
    check(missing.error().message.find("nope") != std::string::npos,
          "require: a missing module says which one", missing.error().message);

    ScriptHost noroots;
    std::string e4;
    noroots.loadString("function onStart() require('counter') end", "@noroots.lua", &e4);
    noroots.callStart();
    check(!noroots.error().empty() && noroots.error().message.find("no script folders") != std::string::npos,
          "require: a host with no roots says so", noroots.error().message);
}

// --- the runtime ----------------------------------------------------------------------------------------------

static std::vector<std::string> gCalls;

static int l_record(lua_State* L) {
    gCalls.push_back(luaL_checkstring(L, 1));
    return 0;
}

static int l_self(lua_State* L) {
    lua_getglobal(L, "self");
    gCalls.push_back(std::string("self:") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?"));
    return 0;
}

static void testRuntime() {
    makeTree();
    writeFile("hero.lua",
              "function onStart() ukiyo.record('start') ukiyo.self_name() end\n"
              "function onUpdate(dt) ukiyo.record('update') end\n"
              "function onContact(c) ukiyo.record('contact:' .. c.kind) end\n"
              "function onDestroy() ukiyo.record('destroy') end\n");
    writeFile("enemy.lua", "function onStart() ukiyo.record('start') end\nfunction onUpdate(dt) ukiyo.record('update') end\n");
    gCalls.clear();

    ScriptRuntime rt;
    rt.onHostCreate([](ScriptHost& h) {
        h.registerFunction("record", l_record, nullptr);
        h.registerFunction("self_name", l_self, nullptr);
    });
    std::string err;
    check(rt.add("hero", pathOf("hero.lua"), &err), "runtime: the hero's script loads", err);
    check(rt.add("enemy", pathOf("enemy.lua"), &err), "runtime: the enemy's script loads too", err);
    check(rt.size() == 2, "runtime: two scripts are running", std::to_string(rt.size()));

    rt.callStart();
    check(gCalls.size() == 3 && gCalls[0] == "start" && gCalls[1] == "self:hero" && gCalls[2] == "start",
          "runtime: onStart runs for each script, in the order they were added, and self is the entity",
          gCalls.size() > 1 ? gCalls[1] : std::string());
    gCalls.clear();
    rt.callUpdate(0.016f);
    check(gCalls == std::vector<std::string>{"update", "update"}, "runtime: onUpdate runs for each script");

    const std::vector<std::string> contacts = rt.entitiesWith("onContact");
    check(contacts.size() == 1 && contacts[0] == "hero", "runtime: only the script that defines onContact is listed");

    const std::vector<ScriptInfo>& info = rt.scripts();
    check(info.size() == 2 && info[0].entity == "hero" && info[0].loaded && !info[0].stopped &&
              info[0].path == pathOf("hero.lua"),
          "runtime: the list says what is loaded, from where, and whether it stopped");
    check(rt.lastError().empty(), "runtime: a healthy set has no error");

    gCalls.clear();
    check(rt.reload("hero", &err), "runtime: a script reloads", err);
    rt.callStart();
    check(gCalls.size() == 3 && gCalls[0] == "start" && gCalls[1] == "self:hero",
          "runtime: the reloaded script runs its onStart again, and so does the other one",
          std::to_string(gCalls.size()));

    rt.callDestroy();
    check(std::find(gCalls.begin(), gCalls.end(), "destroy") != gCalls.end(), "runtime: onDestroy reaches the script");

    // A script that does not compile is reported and left out, without taking the others down.
    writeFile("broken.lua", "function onUpdate(dt)\n  local = 1\nend\n");
    std::string badErr;
    check(!rt.add("broken", pathOf("broken.lua"), &badErr), "runtime: a script that does not compile is refused");
    check(badErr.find("broken.lua:2:") != std::string::npos, "runtime: and the error names the line", badErr);
    check(rt.size() == 2, "runtime: the other scripts are untouched", std::to_string(rt.size()));

    rt.remove("enemy");
    check(rt.size() == 1 && rt.find("enemy") == nullptr, "runtime: removing a script leaves the rest");
    rt.clear();
    check(rt.size() == 0 && rt.scripts().empty(), "runtime: clear removes every script");
}

// --- the editor support files ------------------------------------------------------------------------------------

static void testProjectFiles() {
    makeTree();
    const std::string root = testRoot().string();
    std::vector<std::string> written;
    std::string err;
    check(script_project::writeSupportFiles(root, &written, &err), "project: the support files are written", err);
    for (const std::string& rel : script_project::supportFiles()) {
        check(fs::exists(fs::path(root) / rel), "project: " + rel + " exists");
    }
    check(written.size() == script_project::supportFiles().size(), "project: every file was written once",
          std::to_string(written.size()));

    std::ifstream luarc(fs::path(root) / ".luarc.json");
    const std::string text((std::istreambuf_iterator<char>(luarc)), std::istreambuf_iterator<char>());
    check(text.find("Scripts/.ukiyo/lua") != std::string::npos && text.find("Lua 5.4") != std::string::npos,
          "project: .luarc.json points the language server at the stub, for Lua 5.4");

    // The editor's own files are not overwritten, the stub and .luarc.json always are.
    writeFile("stylua.toml", "-- mine\n");
    std::vector<std::string> second;
    check(script_project::writeSupportFiles(root, &second, &err), "project: writing again succeeds", err);
    check(second.size() == 2, "project: only the stub and .luarc.json are rewritten", std::to_string(second.size()));
    std::ifstream stylua(fs::path(root) / "stylua.toml");
    const std::string kept((std::istreambuf_iterator<char>(stylua)), std::istreambuf_iterator<char>());
    check(kept == "-- mine\n", "project: a formatter config the project changed is kept");

    // The stub is Lua the engine can compile, and it documents every function the engine binds.
    ScriptHost h;
    std::string stubErr;
    const std::string stub = script_project::apiStub();
    check(h.compileString(stub, "@ukiyo.lua", &stubErr), "project: the stub is valid Lua", stubErr);
    for (const ScriptApiBinding& b : World::apiBindings()) {
        const std::string decl = std::string("function ukiyo.") + b.name;
        check(stub.find(decl) != std::string::npos, std::string("project: the stub declares ") + b.name);
    }
    bool everyDocIsBound = true;
    std::string unbound;
    for (const script_project::ApiDoc& d : script_project::apiDocs()) {
        const std::string name = d.name;
        if (name == "Vector3" || name == "Quaternion") continue;   // classes, in installMath
        const bool bound = std::any_of(World::apiBindings().begin(), World::apiBindings().end(),
                                       [&](const ScriptApiBinding& b) { return name == b.name; });
        if (!bound && name != "Entity") {
            everyDocIsBound = false;
            unbound += name + " ";
        }
    }
    check(everyDocIsBound, "project: every documented function is one the engine binds", unbound);
}

int main() {
    makeTree();
    testSandbox();
    testErrors();
    testRequire();
    testRuntime();
    testProjectFiles();
    std::error_code ec;
    fs::remove_all(testRoot(), ec);
    std::printf("script: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
