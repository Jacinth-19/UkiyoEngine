#include "systems/script.h"

#include "engine/vfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace ukiyo {

namespace {

// Lua's own allocator, so a build that installs none behaves exactly as luaL_newstate would.
void* defaultAllocator(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        std::free(ptr);
        return nullptr;
    }
    return std::realloc(ptr, nsize);
}

lua_Alloc stateAllocator = nullptr;
void* stateAllocatorData = nullptr;

// Lua aborts the process when an API call fails outside any protected call, and its default panic only prints to
// stderr, which a Windows job reads as a number. Naming the engine in the line makes the abort say what happened.
int panicHandler(lua_State* L) {
    static bool inPanic = false;   // a panic handler that panics is only a longer silence
    const char* msg = lua_tostring(L, -1);
    std::fprintf(stderr, "ukiyo: PANIC: unprotected Lua error: %s\n", msg ? msg : "no message");
    if (!inPanic) {
        // The stack is usually intact here: Lua can still walk its own frames, and the traceback names the C function
        // that made the unprotected call.
        inPanic = true;
        if (lua_checkstack(L, 8)) {
            luaL_traceback(L, L, msg, 1);
            std::fprintf(stderr, "%s\n", lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no traceback)");
        }
    }
    std::fflush(stderr);
    return 0;
}

// The message handler that turns "path:3: message" into that plus the call stack. Every protected call goes through it,
// so a script that throws from inside a required module still says where.
int tracebackHandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (!luaL_callmeta(L, 1, "__tostring") || !lua_isstring(L, -1)) return 1;
        msg = lua_tostring(L, -1);
        lua_remove(L, -2);
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// "path:12: message", "[string \"path\"]:3: message", or no location at all.
ScriptError parseError(const std::string& text, const std::string& fallbackFile) {
    ScriptError e;
    e.traceback = text;
    // The message is the first line; the rest is the traceback.
    const size_t newline = text.find('\n');
    std::string first = newline == std::string::npos ? text : text.substr(0, newline);
    e.message = first;

    // Find the ":" that starts a digit run followed by another ":" -- "file:12: msg". A Windows path has a colon
    // after the drive letter, which this skips because what follows is not a digit run.
    size_t at = 0;
    while (true) {
        at = first.find(':', at);
        if (at == std::string::npos) break;
        size_t d = at + 1;
        while (d < first.size() && first[d] >= '0' && first[d] <= '9') ++d;
        if (d > at + 1 && d < first.size() && first[d] == ':') {
            std::string file = first.substr(0, at);
            // Lua names a chunk given as "@path" by its source quote: [string "@path"].
            if (file.rfind("[string \"", 0) == 0) {
                file = file.substr(9);
                if (!file.empty() && file.back() == '"') file.pop_back();
            }
            if (!file.empty() && file[0] == '@') file.erase(file.begin());
            if (file.empty()) file = fallbackFile;
            e.file = file;
            e.line = std::atoi(first.c_str() + at + 1);
            e.message = first.substr(d + 1);
            if (!e.message.empty() && e.message[0] == ' ') e.message.erase(e.message.begin());
            return e;
        }
        ++at;
    }
    e.file = fallbackFile;
    return e;
}

// --- require ------------------------------------------------------------------------------------------------

// The module cache lives in the registry, one table per state.
constexpr const char* kModuleCache = "ukiyo.modules";

bool safeModuleName(const char* name) {
    if (!name || !*name) return false;
    if (name[0] == '/' || name[0] == '\\' || name[0] == '.') return false;   // absolute, or "..", or hidden
    if (std::strstr(name, "..")) return false;
    if (std::strchr(name, ':')) return false;                                // a drive letter
    return true;
}

// require(name): the host's own loader. It returns the module's value (or true when the module returned nothing), runs
// a module once, and answers a cycle with the value of the module that is already being loaded instead of recursing.
int l_require(lua_State* L) {
    auto* host = static_cast<ScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, kModuleCache);   // 2
    if (!lua_istable(L, 2)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kModuleCache);
    }
    lua_getfield(L, 2, name);                           // 3
    if (!lua_isnil(L, 3)) return 1;                      // already loaded (or loading)
    lua_pop(L, 1);

    if (!safeModuleName(name)) return luaL_error(L, "require: '%s' is not a module name", name);
    std::string path, error;
    if (!host || !host->resolveModule(name, &path, &error)) return luaL_error(L, "%s", error.c_str());
    std::vector<uint8_t> bytes;
    if (!vfs::readBytes(path, bytes)) return luaL_error(L, "require: could not read %s", path.c_str());
    const std::string code(bytes.begin(), bytes.end());

    // Compiled under the module's own name, so an error inside it points at the module.
    if (luaL_loadbufferx(L, code.data(), code.size(), ("@" + path).c_str(), "t") != LUA_OK) {
        lua_pushnil(L);      // no cache entry for a module that did not compile
        lua_setfield(L, 2, name);
        lua_error(L);        // the compile error is on the stack
        return 0;
    }
    // A placeholder first: a cycle sees "true" rather than re-entering the file.
    lua_pushboolean(L, 1);
    lua_setfield(L, 2, name);

    lua_pushstring(L, name);                             // the module receives its own name
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pushnil(L);                                  // a module that threw is not cached
        lua_setfield(L, 2, name);
        return lua_error(L);
    }
    lua_pushvalue(L, 3);
    lua_setfield(L, 2, name);
    return 1;
}

} // namespace

void ScriptHost::setAllocator(lua_Alloc alloc, void* userdata) {
    stateAllocator = alloc;
    stateAllocatorData = userdata;
}

ScriptHost::ScriptHost() {
    L_ = lua_newstate(stateAllocator ? stateAllocator : defaultAllocator, stateAllocatorData);
    if (!L_) {
        stopped_ = true;
        lastError_ = error_.message = "could not create a Lua state";
        return;
    }
    lua_atpanic(L_, panicHandler);
    // Open only the libraries that do no file, process, or package access.
    luaL_requiref(L_, "_G", luaopen_base, 1);
    lua_pop(L_, 1);
    luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L_, 1);
    luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L_, 1);
    luaL_requiref(L_, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L_, 1);
    luaL_requiref(L_, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L_, 1);
    luaL_requiref(L_, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L_, 1);
    // base brings in file loaders. Remove them so a script cannot read arbitrary files.
    for (const char* name : {"dofile", "loadfile", "load"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, name);
    }
    // require is the host's, not the package library's, and it is the only way to load another file.
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, l_require, 1);
    lua_setglobal(L_, "require");
    lua_newtable(L_);
    lua_setfield(L_, LUA_REGISTRYINDEX, kModuleCache);
}

ScriptHost::~ScriptHost() {
    if (!L_) return;
    // Collect before closing: a script that smashed a heap block would otherwise have that block freed by the close
    // sweep, sections away from the section that smashed it -- exactly how a wild write drifts away from its cause.
    // A full cycle here walks every object through the allocator while the guilty section is still on the record.
    lua_gc(L_, LUA_GCCOLLECT, 0);
    lua_close(L_);
}

void ScriptHost::setSearchPaths(std::vector<std::string> roots) { roots_ = std::move(roots); }

void ScriptHost::setEntityName(std::string name) {
    entity_ = std::move(name);
    if (!L_) return;
    lua_pushlstring(L_, entity_.c_str(), entity_.size());
    lua_setglobal(L_, "self");
}

bool ScriptHost::resolveModule(const std::string& name, std::string* path, std::string* error) const {
    std::string rel = name;
    for (char& c : rel)
        if (c == '.') c = '/';
    if (roots_.empty()) {
        if (error) *error = "require: this script has no script folders to load '" + name + "' from";
        return false;
    }
    for (const std::string& root : roots_) {
        for (const std::string& candidate : {root + "/" + rel + ".lua", root + "/" + rel + "/init.lua"}) {
            if (vfs::exists(candidate)) {
                if (path) *path = candidate;
                return true;
            }
        }
    }
    if (error) *error = "require: no module '" + name + "' in " + roots_.front();
    return false;
}

void ScriptHost::report(int status) {
    if (status == LUA_OK) {
        resetBudget();
        return;
    }
    const char* msg = lua_tostring(L_, -1);
    const std::string text = msg ? msg : "unknown Lua error";
    lua_pop(L_, 1);
    error_ = parseError(text, entity_.empty() ? std::string("script") : entity_);
    lastError_ = error_.file;
    if (error_.line > 0) lastError_ += ":" + std::to_string(error_.line);
    lastError_ += ": " + error_.message;
    if (++errors_ >= kMaxErrors) {
        stopped_ = true;
        lastError_ += "  (script stopped after " + std::to_string(kMaxErrors) + " errors)";
    }
    ++totalErrors_;
}

void ScriptHost::resetBudget() { errors_ = 0; }

bool ScriptHost::protectedCall(int nargs, int nresults) {
    // The handler has to sit below the function: lua_pcall pops the function and its arguments before it needs the
    // handler, so an index above them points into the frame that is being removed.
    lua_pushcfunction(L_, tracebackHandler);
    lua_insert(L_, -(nargs + 2));
    const int handler = lua_gettop(L_) - nargs - 1;
    const int status = lua_pcall(L_, nargs, nresults, handler);
    lua_remove(L_, handler);
    report(status);
    return status == LUA_OK;
}

bool ScriptHost::loadFile(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!vfs::readBytes(path, bytes)) {
        error_ = ScriptError{path, 0, "could not open " + path, "could not open " + path};
        lastError_ = error_.message;
        if (error) *error = error_.message;
        return false;
    }
    return loadString(std::string(bytes.begin(), bytes.end()), "@" + path, error);
}

bool ScriptHost::loadString(const std::string& code, const std::string& chunkName, std::string* error) {
    if (!L_) return false;
    if (luaL_loadbufferx(L_, code.data(), code.size(), chunkName.c_str(), "t") != LUA_OK) {
        report(LUA_ERRSYNTAX);   // puts the compile error in error_
        if (error) *error = lastError_;
        return false;
    }
    if (!protectedCall(0, 0)) {
        if (error) *error = lastError_;
        return false;
    }
    loaded_ = true;
    return true;
}

bool ScriptHost::compileString(const std::string& code, const std::string& chunkName, std::string* error) {
    if (!L_) return false;
    if (luaL_loadbufferx(L_, code.data(), code.size(), chunkName.c_str(), "t") != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
        const std::string text = msg ? msg : "unknown Lua error";
        lua_pop(L_, 1);
        error_ = parseError(text, chunkName);
        lastError_ = error_.file;
        if (error_.line > 0) lastError_ += ":" + std::to_string(error_.line);
        lastError_ += ": " + error_.message;
        if (error) *error = lastError_;
        return false;
    }
    lua_pop(L_, 1);   // the compiled chunk is not run
    return true;
}

bool ScriptHost::compileFile(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!vfs::readBytes(path, bytes)) {
        if (error) *error = "could not open " + path;
        return false;
    }
    return compileString(std::string(bytes.begin(), bytes.end()), "@" + path, error);
}

void ScriptHost::registerFunction(const char* name, lua_CFunction fn, void* ctx) {
    if (!L_) return;
    lua_getglobal(L_, "ukiyo");
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_pushvalue(L_, -1);
        lua_setglobal(L_, "ukiyo");
    }
    lua_pushlightuserdata(L_, ctx);
    lua_pushcclosure(L_, fn, 1);
    lua_setfield(L_, -2, name);
    lua_pop(L_, 1);
}

bool ScriptHost::hasFunction(const char* name) const {
    if (!L_) return false;
    lua_getglobal(L_, name);
    const bool is = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return is;
}

// Every call below pushes a global function and its arguments, then runs it through protectedCall, which is what
// counts errors and builds the traceback.
void ScriptHost::callStart() {
    if (!L_ || !loaded_ || stopped_) return;
    lua_getglobal(L_, "onStart");
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return;
    }
    protectedCall(0, 0);
}

void ScriptHost::callUpdate(float dt) {
    if (!L_ || !loaded_ || stopped_ || !hasFunction("onUpdate")) return;
    lua_getglobal(L_, "onUpdate");
    lua_pushnumber(L_, dt);
    protectedCall(1, 0);
}

void ScriptHost::callSceneLoaded() {
    if (!L_ || !loaded_ || stopped_ || !hasFunction("onSceneLoaded")) return;
    lua_getglobal(L_, "onSceneLoaded");
    protectedCall(0, 0);
}

void ScriptHost::callDestroy() {
    if (!L_ || !loaded_ || !hasFunction("onDestroy")) return;
    lua_getglobal(L_, "onDestroy");
    protectedCall(0, 0);
}

void ScriptHost::callPushed(int nargs) {
    if (!L_) return;
    if (stopped_ || !loaded_) {
        lua_pop(L_, nargs + 1);
        return;
    }
    protectedCall(nargs, 0);
}

} // namespace ukiyo
