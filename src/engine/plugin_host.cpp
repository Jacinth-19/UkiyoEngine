#include "engine/plugin_host.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "engine/plugin_api.h"

namespace ukiyo {

namespace {

// The C table. Every function here takes the host back through userData; the plugin never sees a C++ type.
//
// The table is one object per host, built once and never moved: a plugin stores the pointer it was given and calls
// through it from its commands, which run long after load() has returned.
const char* apiProjectRoot(void* userData) {
    auto* host = static_cast<PluginHost*>(userData);
    // The string has to outlive the call, so it is stashed on the host for the length of one import/command. A plugin
    // that keeps the pointer past the call is breaking the contract the header states.
    static thread_local std::string scratch;
    scratch = host->callbacks().projectRoot ? host->callbacks().projectRoot() : std::string();
    return scratch.c_str();
}

const char* apiScenePath(void* userData) {
    auto* host = static_cast<PluginHost*>(userData);
    static thread_local std::string scratch;
    scratch = host->callbacks().scenePath ? host->callbacks().scenePath() : std::string();
    return scratch.c_str();
}

void apiLog(void* userData, const char* message) {
    auto* host = static_cast<PluginHost*>(userData);
    if (host->callbacks().log) host->callbacks().log(message ? message : "");
}

void apiSetStatus(void* userData, const char* message, int ms) {
    auto* host = static_cast<PluginHost*>(userData);
    if (host->callbacks().status) host->callbacks().status(message ? message : "", ms);
}

int apiRegisterCommand(void* userData, const char* id, const char* label,
                       void (*run)(void* commandUserData), void* commandUserData) {
    auto* host = static_cast<PluginHost*>(userData);
    if (!id || !*id || !label) return 1;
    // The callback is a C function pointer the plugin owns; the lambda holds it together with the plugin's own user
    // data, so a plugin with several commands can tell them apart.
    host->registerCommand(host->loadingPluginForApi(), id, label,
                          [run, commandUserData] { if (run) run(commandUserData); });
    return 0;
}

int apiRegisterImporter(void* userData, const char* extension, const char* label,
                        int (*importFile)(void* importerUserData, const char* path), void* importerUserData) {
    auto* host = static_cast<PluginHost*>(userData);
    return host->registerImporterForApi(extension, label, importFile, importerUserData);
}

} // namespace

struct PluginHost::Library {
    void* handle = nullptr;
    std::string path;
    void (*unload)() = nullptr;
};

namespace {

using LoadFn = int (*)(const UkiyoEditorApi*, UkiyoPluginInfo*);
using UnloadFn = void (*)();

UnloadFn symbolUnload(void* handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<UnloadFn>(reinterpret_cast<void (*)()>(GetProcAddress(static_cast<HMODULE>(handle), name)));
#else
    return reinterpret_cast<UnloadFn>(dlsym(handle, name));
#endif
}

LoadFn symbolLoad(void* handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<LoadFn>(reinterpret_cast<void (*)()>(GetProcAddress(static_cast<HMODULE>(handle), name)));
#else
    return reinterpret_cast<LoadFn>(dlsym(handle, name));
#endif
}

void* openLibrary(const std::string& path, std::string* error) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) {
        if (error) *error = "the library could not be loaded (Windows error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    return h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (error) *error = dlerror() ? dlerror() : "the library could not be loaded";
        return nullptr;
    }
    return h;
#endif
}

void closeLibrary(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace

struct PluginHost::ApiTable {
    UkiyoEditorApi api{};
};

PluginHost::PluginHost(PluginHostCallbacks callbacks) : callbacks_(std::move(callbacks)), api_(new ApiTable) {
    UkiyoEditorApi& api = api_->api;
    api.size = sizeof(UkiyoEditorApi);
    api.version = UKIYO_PLUGIN_API_VERSION;
    api.userData = this;
    api.log = &apiLog;
    api.setStatus = &apiSetStatus;
    api.projectRoot = &apiProjectRoot;
    api.scenePath = &apiScenePath;
    api.registerCommand = &apiRegisterCommand;
    api.registerImporter = &apiRegisterImporter;
}

PluginHost::~PluginHost() { unloadAll(); }

std::string PluginHost::pluginExtension() {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

int PluginHost::loadDirectory(const std::string& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> found;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string path = it->path().string();
        if (path.size() < pluginExtension().size()) continue;
        if (path.compare(path.size() - pluginExtension().size(), pluginExtension().size(), pluginExtension()) != 0)
            continue;
        found.push_back(path);
    }
    std::sort(found.begin(), found.end());
    // A scan is a fresh look at a directory, so the reports describe what is there now rather than every attempt since
    // the editor started.
    reports_.clear();
    int loaded = 0;
    for (const std::string& path : found)
        if (loadFile(path)) ++loaded;
    return loaded;
}

bool PluginHost::loadFile(const std::string& path) {
    PluginReport report;
    report.path = path;

    std::string error;
    void* handle = openLibrary(path, &error);
    if (!handle) {
        report.error = error;
        reports_.push_back(report);
        if (callbacks_.log) callbacks_.log("plugin " + path + ": " + error);
        return false;
    }
    if (!adopt(handle, path)) {
        closeLibrary(handle);
        return false;
    }
    return true;
}

bool PluginHost::adopt(void* handle, const std::string& path) {
    const LoadFn load = symbolLoad(handle, "ukiyo_plugin_load");
    if (!load) {
        PluginReport r;
        r.path = path;
        r.error = "it does not export ukiyo_plugin_load";
        reports_.push_back(r);
        if (callbacks_.log) callbacks_.log("plugin " + path + ": " + r.error);
        return false;
    }

    const UkiyoEditorApi& api = api_->api;

    auto* lib = new Library;
    lib->handle = handle;
    lib->path = path;

    UkiyoPluginInfo info{};
    info.size = sizeof(UkiyoPluginInfo);

    // The plugin compares api.version with the one it was compiled against. A plugin built for a newer table than this
    // one offers would read a function pointer past the end of what the host filled in, so the host refuses it before
    // calling in: the size it filled in is the size the plugin must not exceed.
    const std::string previous = loadingPlugin_;
    loadingPlugin_ = path;
    const int rc = load(&api, &info);
    loadingPlugin_ = previous;

    PluginReport r;
    r.path = path;
    if (rc != 0) {
        r.error = "its load function returned " + std::to_string(rc);
        reports_.push_back(r);
        if (callbacks_.log) callbacks_.log("plugin " + path + ": " + r.error);
        // Anything it registered before failing goes with it.
        const std::string name = path;
        commands_.erase(std::remove_if(commands_.begin(), commands_.end(),
                                       [&](const PluginCommand& c) { return c.plugin == name; }),
                        commands_.end());
        importers_.erase(std::remove_if(importers_.begin(), importers_.end(),
                                        [&](const PluginImporter& i) { return i.plugin == name; }),
                         importers_.end());
        delete lib;
        return false;
    }
    if (info.size < sizeof(UkiyoPluginInfo) || !info.name) {
        r.error = "its info block is incomplete";
        reports_.push_back(r);
        if (callbacks_.log) callbacks_.log("plugin " + path + ": " + r.error);
        delete lib;
        return false;
    }
    r.ok = true;
    r.name = info.name;
    r.version = info.version ? info.version : "";
    lib->unload = symbolUnload(handle, "ukiyo_plugin_unload");
    libraries_.push_back(lib);
    reports_.push_back(r);
    if (callbacks_.log)
        callbacks_.log("plugin " + r.name + " " + r.version + " loaded from " + path);
    return true;
}

void PluginHost::registerCommand(const std::string& plugin, const char* id, const char* label,
                                 std::function<void()> run) {
    PluginCommand c;
    c.plugin = plugin;
    c.id = plugin + "/" + id;
    c.label = label;
    c.run = std::move(run);
    commands_.push_back(std::move(c));
}

int PluginHost::registerImporterForApi(const char* extension, const char* label,
                                       int (*importFile)(void* importerUserData, const char* path),
                                       void* importerUserData) {
    if (!extension || !*extension || !importFile) return 1;
    PluginImporter imp;
    imp.plugin = loadingPlugin_;
    imp.extension = extension;
    imp.label = label ? label : extension;
    std::transform(imp.extension.begin(), imp.extension.end(), imp.extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!imp.extension.empty() && imp.extension[0] == '.') imp.extension.erase(0, 1);
    imp.import = [importFile, importerUserData](const std::string& absolutePath) {
        return importFile(importerUserData, absolutePath.c_str()) == 0;
    };
    importers_.push_back(std::move(imp));
    return 0;
}

std::string PluginHost::loadingPluginForApi() const {
    return loadingPlugin_.empty() ? std::string("<unknown>") : loadingPlugin_;
}

void PluginHost::unloadAll() {
    // Newest first: a plugin loaded later may be using one loaded earlier.
    for (auto it = libraries_.rbegin(); it != libraries_.rend(); ++it) {
        Library* lib = *it;
        if (lib->unload) lib->unload();
        closeLibrary(lib->handle);
        delete lib;
    }
    libraries_.clear();
    commands_.clear();
    importers_.clear();
}

bool PluginHost::runCommand(const std::string& id) {
    for (const PluginCommand& c : commands_) {
        if (c.id == id) {
            if (c.run) c.run();
            return true;
        }
    }
    return false;
}

bool PluginHost::importFile(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext.empty()) return false;
    for (const PluginImporter& i : importers_) {
        if (i.extension == ext) return i.import ? i.import(path) : false;
    }
    return false;
}

} // namespace ukiyo
