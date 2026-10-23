/* A plugin that declines to load. The editor must report it and carry on: one broken extension cannot be allowed to
 * stop the editor from starting, and the commands it registered before failing must not survive. */
#include "engine/plugin_api.h"

#include <stddef.h>

static void doomed_command(void* userData) { (void)userData; }

UKIYO_PLUGIN_EXPORT int ukiyo_plugin_load(const UkiyoEditorApi* api, UkiyoPluginInfo* info) {
    /* It registers something first, which the host has to take back when it refuses the plugin. */
    api->registerCommand(api->userData, "before-failure", "Should never appear", doomed_command, NULL);
    if (info) {
        info->size = (uint32_t)sizeof(UkiyoPluginInfo);
        info->name = "refuse";
        info->version = "0.1";
    }
    return 7;   /* "I cannot work in this editor" */
}

UKIYO_PLUGIN_EXPORT void ukiyo_plugin_unload(void) {}
