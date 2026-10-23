/* The ABI an editor extension is built against.
 *
 * Plain C, with no C++ and no Qt in it, on purpose: a plugin is a shared library this editor loads at run time, and a
 * plugin built by another compiler, another standard library, or another language that can export C functions has to
 * work. Nothing here may include anything but <stdint.h>.
 *
 * The rule the host and the plugin both follow:
 *
 *   * the host fills in a UkiyoEditorApi and calls ukiyo_plugin_load;
 *   * the plugin checks `api->version` and `api->size` before it touches a function pointer it was not compiled
 *     against — the struct only ever grows at the end, so a v1 plugin loaded by a v2 host still finds every field it
 *     knows at the offset it expects;
 *   * the plugin fills in a UkiyoPluginInfo (with `size` set to the size of the struct it was compiled against) and
 *     returns 0.
 *
 * A plugin registers commands and importers and returns. It does not hold the pointers it is given: the table belongs
 * to the host and lives as long as the process does, but the strings it hands back (projectRoot, scenePath) are only
 * good for the length of the call.
 */
#ifndef UKIYO_PLUGIN_API_H
#define UKIYO_PLUGIN_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Bumped whenever a field is added to UkiyoEditorApi, or one changes meaning. */
#define UKIYO_PLUGIN_API_VERSION 1

/* What the plugin tells the editor about itself. `size` must be set by the plugin. */
typedef struct UkiyoPluginInfo {
    uint32_t size;
    const char* name;
    const char* version;
    const char* author;
    const char* description;
} UkiyoPluginInfo;

typedef struct UkiyoEditorApi {
    uint32_t size;       /* sizeof(UkiyoEditorApi) */
    uint32_t version;    /* UKIYO_PLUGIN_API_VERSION */
    void* userData;      /* the host, handed back to every call below */

    /* Writes a line to the editor's console. */
    void (*log)(void* userData, const char* message);
    /* Shows a message in the status bar for `ms` milliseconds. */
    void (*setStatus)(void* userData, const char* message, int ms);
    /* The open project's root directory, or "" when no project is open. Valid for the length of the call. */
    const char* (*projectRoot)(void* userData);
    /* The open scene's path, relative to the project root, or "" when no scene is open. */
    const char* (*scenePath)(void* userData);

    /* Adds a command, shown in the Plugins menu and on the project's Plugins page. `id` is how it is referred to
     * afterwards and must be unique within the plugin. Returns 0 on success. */
    int (*registerCommand)(void* userData, const char* id, const char* label, void (*run)(void* commandUserData),
                           void* commandUserData);
    /* Offers to import files with this extension, given without the dot. The editor calls `import` with an absolute
     * file path; it returns 0 when it imported the file. Returns 0 on success. */
    int (*registerImporter)(void* userData, const char* extension, const char* label,
                            int (*import)(void* importerUserData, const char* path), void* importerUserData);
} UkiyoEditorApi;

/* The two symbols a plugin exports.

   ukiyo_plugin_load is called once, when the editor loads the library. Fill in `info` and return 0; anything else and
   the editor unloads the library again and reports the plugin as failed.

   ukiyo_plugin_unload is called before the library is unloaded, and may be null. Anything the plugin registered is
   dropped by the host either way, so this is only for the plugin's own cleanup. */
/* On Windows nothing leaves a DLL unless it says so: a plugin defines its two functions with UKIYO_PLUGIN_EXPORT
   (empty on platforms where every symbol is exported by default), or brings its own visibility mechanism. The host
   never references the functions by name - it looks them up - so the macro costs it nothing. */
#if defined(UKIYO_PLUGIN_EXPORT)
/* the plugin already chose */
#elif defined(_WIN32)
#  define UKIYO_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define UKIYO_PLUGIN_EXPORT
#endif

UKIYO_PLUGIN_EXPORT int ukiyo_plugin_load(const UkiyoEditorApi* api, UkiyoPluginInfo* info);
UKIYO_PLUGIN_EXPORT void ukiyo_plugin_unload(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UKIYO_PLUGIN_API_H */
