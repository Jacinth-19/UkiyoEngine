#include "engine/vfs.h"
#include "systems/script.h"

#include <fstream>
#include <sstream>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace ukiyo {

ScriptHost::ScriptHost() {
    L_ = luaL_newstate();
    if (!L_) {
        failed_ = true;
        lastError_ = "could not create a Lua state";
        return;
    }
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
    for (const char* name : {"dofile", "loadfile"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, name);
    }
}

ScriptHost::~ScriptHost() {
    if (L_) lua_close(L_);
}

bool ScriptHost::report(int status, std::string* error) {
    if (status == LUA_OK) return true;
    const char* msg = lua_tostring(L_, -1);
    lastError_ = msg ? msg : "unknown Lua error";
    lua_pop(L_, 1);
    failed_ = true;
    if (error) *error = lastError_;
    return false;
}

bool ScriptHost::loadFile(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!vfs::readBytes(path, bytes)) {
        lastError_ = "could not open " + path;
        if (error) *error = lastError_;
        return false;
    }
    return loadString(std::string(bytes.begin(), bytes.end()), "@" + path, error);
}

bool ScriptHost::loadString(const std::string& code, const std::string& chunkName, std::string* error) {
    if (!L_) return false;
    if (!report(luaL_loadbufferx(L_, code.data(), code.size(), chunkName.c_str(), "t"), error)) return false;
    if (!report(lua_pcall(L_, 0, 0, 0), error)) return false;
    loaded_ = true;
    return true;
}

bool ScriptHost::compileFile(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!vfs::readBytes(path, bytes)) {
        if (error) *error = "could not open " + path;
        return false;
    }
    const std::string code(bytes.begin(), bytes.end());
    if (!L_) return false;
    if (!report(luaL_loadbufferx(L_, code.data(), code.size(), ("@" + path).c_str(), "t"), error)) return false;
    lua_pop(L_, 1);   // the compiled chunk is not run
    return true;
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

void ScriptHost::callStart() {
    if (!L_ || !hasFunction("onStart")) return;
    lua_getglobal(L_, "onStart");
    report(lua_pcall(L_, 0, 0, 0), nullptr);
}

void ScriptHost::callSceneLoaded() {
    if (!L_ || failed_ || !hasFunction("onSceneLoaded")) return;
    lua_getglobal(L_, "onSceneLoaded");
    report(lua_pcall(L_, 0, 0, 0), nullptr);
}

void ScriptHost::callPushed(int nargs) {
    if (!L_ || failed_) {
        lua_pop(L_, nargs + 1);
        return;
    }
    report(lua_pcall(L_, nargs, 0, 0), nullptr);
}

void ScriptHost::callUpdate(float dt) {
    if (!L_ || failed_ || !hasFunction("onUpdate")) return;
    lua_getglobal(L_, "onUpdate");
    lua_pushnumber(L_, dt);
    report(lua_pcall(L_, 1, 0, 0), nullptr);
}

} // namespace ukiyo
