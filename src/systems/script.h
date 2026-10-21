// Lua 5.4 host. Scripts run in a sandbox: only base, string, table, math, utf8, and coroutine are open.
// File, OS, and package access are not available to scripts.
#pragma once

#include <string>

struct lua_State;
using lua_CFunction = int (*)(lua_State*);

namespace ukiyo {

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    bool loadFile(const std::string& path, std::string* error);
    bool loadString(const std::string& code, const std::string& chunkName, std::string* error);
    // Compiles a file without running it, for the editor's script check. Errors carry the file and line.
    bool compileFile(const std::string& path, std::string* error);

    // Registers ukiyo.<name>. `ctx` is passed to the function as upvalue 1 (see lua_upvalueindex).
    void registerFunction(const char* name, lua_CFunction fn, void* ctx);

    // Calls the script's onStart() and onUpdate(dt) when they are defined. Errors are kept in lastError().
    void callStart();
    void callUpdate(float dt);
    // Calls onSceneLoaded() when defined. The editor calls it once a scene has been loaded and started.
    void callSceneLoaded();
    // Calls the function that the caller has pushed, with `nargs` arguments pushed after it. Errors are kept in
    // lastError() and stop the scripts, as in callUpdate.
    void callPushed(int nargs);
    bool hasFunction(const char* name) const;
    // True once a chunk has been loaded successfully. A host that was never given a script runs nothing; the player
    // reports that rather than counting an empty host as a running script.
    bool loaded() const { return loaded_; }
    bool hasFailed() const { return failed_; }
    const std::string& lastError() const { return lastError_; }

    lua_State* state() const { return L_; }

private:
    bool report(int status, std::string* error);
    lua_State* L_ = nullptr;
    std::string lastError_;
    bool failed_ = false;
    bool loaded_ = false;
};

} // namespace ukiyo
