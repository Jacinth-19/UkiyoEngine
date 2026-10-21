// Lua 5.4 host. Scripts run in a sandbox: only base, string, table, math, utf8, and coroutine are open. File, OS and
// package access are not available to scripts; require() is the host's own, and it can only read files under the script
// folders it was given.
//
// One host is one script. The runtime that owns several of them is ScriptRuntime (script_runtime.h).
#pragma once

#include <string>
#include <vector>

struct lua_State;
using lua_CFunction = int (*)(lua_State*);

namespace ukiyo {

// Where an error happened, when Lua could say. `file` is the chunk name without its leading '@'; for a required module
// it is the module's file, so a message about a module points at the module, not at the script that required it.
struct ScriptError {
    std::string file;
    int line = 0;
    std::string message;     // one line: the error without its location prefix
    std::string traceback;   // the message followed by the Lua stack, for the log
    bool empty() const { return message.empty(); }
};

class ScriptHost {
public:
    // Consecutive errors after which a host stops running. One transient nil should not kill a level's logic, but a
    // script that fails every frame would fill the console; after this many in a row it is stopped and says so.
    static constexpr int kMaxErrors = 3;

    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Folders require() may read from. Absolute paths and ".." in a module name are refused, so a module is always
    // inside one of these roots. Empty by default: a script with no roots can require nothing.
    void setSearchPaths(std::vector<std::string> roots);
    const std::vector<std::string>& searchPaths() const { return roots_; }

    // The entity this script belongs to. It reaches Lua as the string `self`, and errors carry it.
    void setEntityName(std::string name);
    const std::string& entityName() const { return entity_; }

    bool loadFile(const std::string& path, std::string* error);
    bool loadString(const std::string& code, const std::string& chunkName, std::string* error);
    // Compiles a file without running it, for the editor's script check. Errors carry the file and line.
    bool compileFile(const std::string& path, std::string* error);
    // Compiles a string without running it. `chunkName` is what errors call it (pass the file path for a real script).
    bool compileString(const std::string& code, const std::string& chunkName, std::string* error);
    // Resolves a require() name to a file: <root>/<name>.lua, then <root>/<name>/init.lua. Exposed for the editor's
    // "where is this module" check; require() uses it too.
    bool resolveModule(const std::string& name, std::string* path, std::string* error) const;

    // Registers ukiyo.<name>. `ctx` is passed to the function as upvalue 1 (see lua_upvalueindex).
    void registerFunction(const char* name, lua_CFunction fn, void* ctx);

    // Lifecycle. The engine calls, in order: onStart once, onUpdate(dt) every frame before physics, onContact for
    // physics contacts, onSceneLoaded once the scene has started, and onDestroy when the scene goes away.
    void callStart();
    void callUpdate(float dt);
    void callSceneLoaded();
    void callDestroy();
    // Calls the function that the caller has pushed, with `nargs` arguments pushed after it. Errors are recorded and
    // counted like any other call.
    void callPushed(int nargs);

    bool hasFunction(const char* name) const;
    // True once a chunk has been loaded successfully. A host that was never given a script runs nothing; the player
    // reports that rather than counting an empty host as a running script.
    bool loaded() const { return loaded_; }
    // True once the error budget is spent: the host refuses to run more code.
    bool hasFailed() const { return stopped_; }
    const std::string& lastError() const { return lastError_; }
    const ScriptError& error() const { return error_; }
    int errorCount() const { return totalErrors_; }   // every error this host has reported

    lua_State* state() const { return L_; }

private:
    // Pushes the message handler, calls, and removes it again. `nargs` values and the function are on the stack.
    bool protectedCall(int nargs, int nresults);
    void report(int status);
    void resetBudget();

    lua_State* L_ = nullptr;
    std::vector<std::string> roots_;
    std::string entity_;
    ScriptError error_;
    std::string lastError_;
    int errors_ = 0;          // consecutive errors, reset by a call that returns
    int totalErrors_ = 0;
    bool stopped_ = false;
    bool loaded_ = false;
};

} // namespace ukiyo
