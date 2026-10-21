// The scripts a running game owns: one ScriptHost per entity, in the order the scene lists them.
//
// The engine used to run one script per scene (the first instance that had one), which made a level with two actors
// impossible. A runtime owns the set, hands each new host the same API, and calls the lifecycle in a stable order.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "systems/script.h"

namespace ukiyo {

// One script as the runtime knows it, for the editor's list and the player's warnings.
struct ScriptInfo {
    std::string entity;    // "" for a scene-wide script
    std::string path;      // the file it was loaded from
    bool loaded = false;
    bool stopped = false;  // spent its error budget
    std::string error;     // the last error, empty when it has none
};

class ScriptRuntime {
public:
    // Applied to every host as it is created, before any file is loaded: this is where the ukiyo.* API and the
    // vector/quaternion tables go, so a script sees them from its first line and a script added later sees them too.
    using Setup = std::function<void(ScriptHost&)>;
    void onHostCreate(Setup setup) { setup_ = std::move(setup); }

    // Folders require() may read from. Set before adding scripts; applied to the hosts that already exist as well.
    void setSearchPaths(std::vector<std::string> roots);

    // The host for `entity`, created empty (no file loaded) when there is none yet. A scene-level script — a world with
    // logic but no object to hang it on — and the tests use this to get a state without a file on disk.
    ScriptHost& ensure(const std::string& entity);

    // Adds (or replaces) the script for `entity`. The path is read through the virtual file system, so a packed
    // project works the same as a folder.
    bool add(const std::string& entity, const std::string& path, std::string* error);
    // Re-reads a script from disk, running onStart again. The old host is dropped first, so state does not leak
    // between versions; a reload that does not compile leaves the runtime without that script and reports the error.
    bool reload(const std::string& entity, std::string* error);
    void remove(const std::string& entity);
    void clear();

    size_t size() const { return hosts_.size(); }
    const std::vector<ScriptInfo>& scripts() const { return info_; }
    ScriptHost* find(const std::string& entity);

    // Lifecycle. Every host gets each call, in the order they were added.
    void callStart();
    void callUpdate(float dt);
    void callSceneLoaded();
    void callDestroy();

    // True when any script has spent its error budget.
    bool anyFailed() const;
    // The first error in the set as "entity: file:line: message", or empty.
    std::string lastError() const;
    // Entities whose script defines `fn`, for callbacks that belong to one script (contacts, for instance).
    std::vector<std::string> entitiesWith(const char* fn) const;

private:
    void refreshInfo();

    std::vector<std::unique_ptr<ScriptHost>> hosts_;
    std::vector<std::string> entities_;
    std::vector<std::string> paths_;
    std::vector<ScriptInfo> info_;
    std::vector<std::string> roots_;
    Setup setup_;
};

} // namespace ukiyo
