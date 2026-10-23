// Loading editor extensions.
//
// A plugin is a shared library that exports two C functions (src/engine/plugin_api.h) and is handed a table of the
// editor's own functions. This file is the host half: it finds the libraries, loads them, fills in the table, and keeps
// what each plugin registered. It is Qt-free, so a test can load a real library and check the whole path without a
// window.
//
// What a plugin may do is deliberately small: add a command to the Plugins menu, and claim a file extension for
// import. That is enough for the thing plugins are actually wanted for here - a converter for a file format the
// importer does not know, and a button that runs a script over the project - and it avoids handing an arbitrary
// library the editor's scene document, which would need a permission model and a version of this file three times the
// size before anyone could trust it. The table only grows, so what is missing now can be added without breaking a
// plugin built today.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ukiyo {

// What one plugin told the editor about itself, and whether it loaded.
struct PluginReport {
    std::string path;
    std::string name;
    std::string version;
    std::string error;   // why it did not load, or empty
    bool ok = false;
};

struct PluginCommand {
    std::string id;      // "plugin-id/command-id", unique across the session
    std::string label;   // what the menu shows
    std::string plugin;
    std::function<void()> run;
};

struct PluginImporter {
    std::string extension;   // lower case, no dot
    std::string label;
    std::string plugin;
    std::function<bool(const std::string& absolutePath)> import;
};

// The editor's side of the table. Every one of these is called on the main thread, from inside the plugin's own
// callbacks, so an implementation may touch the window.
struct PluginHostCallbacks {
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&, int)> status;   // message, milliseconds
    std::function<std::string()> projectRoot;
    std::function<std::string()> scenePath;
};

class PluginHost {
public:
    explicit PluginHost(PluginHostCallbacks callbacks = {});
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Loads every library in `directory` whose name ends in the platform's plugin extension (".so", ".dylib", ".dll"),
    // sorted by name so one plugin's commands come in a predictable order. A file that fails to load is reported, not
    // thrown: one broken plugin must not stop the editor from starting.
    int loadDirectory(const std::string& directory);
    bool loadFile(const std::string& path);
    // Unloads every library, newest first. Commands and importers go with them. The ABI says a plugin's strings are
    // only valid during a call, so nothing here may outlive its library.
    void unloadAll();

    const std::vector<PluginReport>& reports() const { return reports_; }
    const std::vector<PluginCommand>& commands() const { return commands_; }
    const std::vector<PluginImporter>& importers() const { return importers_; }
    const PluginHostCallbacks& callbacks() const { return callbacks_; }
    // How many libraries are loaded right now. This is not the number of reports: a report is a record of an attempt,
    // and the Plugins page shows the attempts so a plugin that failed is visible instead of merely absent.
    int loadedCount() const { return static_cast<int>(libraries_.size()); }

    // Runs a registered command. False for an unknown id.
    bool runCommand(const std::string& id);
    // Hands `path` to the plugin that claimed its extension. False when no plugin claims it, or the plugin declined.
    bool importFile(const std::string& path);

    static std::string pluginExtension();

    // The three the C table calls back into. Public because the table's functions are plain C functions with no access
    // to the class; nothing outside a plugin's own load() has any reason to call them.
    void registerCommand(const std::string& plugin, const char* id, const char* label, std::function<void()> run);
    int registerImporterForApi(const char* extension, const char* label,
                               int (*importFile)(void* importerUserData, const char* path), void* importerUserData);
    std::string loadingPluginForApi() const;

private:
    struct Library;
    bool adopt(void* handle, const std::string& path);

    PluginHostCallbacks callbacks_;
    // The table handed to every plugin. It is a member, not a local in loadFile: a plugin's command runs long after its
    // load() returned, and the pointers it was given have to still be there when it calls them. This is the one piece
    // of the host that must outlive every library, and it does, because it is destroyed after them (unloadAll runs
    // first in the destructor).
    struct ApiTable;
    std::unique_ptr<ApiTable> api_;
    std::vector<Library*> libraries_;
    std::vector<PluginReport> reports_;
    std::vector<PluginCommand> commands_;
    std::vector<PluginImporter> importers_;
    std::string loadingPlugin_;   // the plugin whose load() is running, for registrations that arrive during it
};

} // namespace ukiyo
