/* A sample editor plugin, built as a shared library and loaded by tests/plugin_test.cpp.
 *
 * It exists to prove the ABI on the platform CI runs on: the header is plain C, so this file is compiled by the C
 * compiler, and everything it needs from the editor arrives through the function table. A plugin written in C++ or in
 * another language that can export two symbols does the same three things: check the version, register, return.
 *
 * The command writes a file, so the test can see that the pointer the editor handed over still reaches this library
 * after load() has returned; the importer appends a line to a log, so the test can see the file path arrive intact.
 */
#include "engine/plugin_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const UkiyoEditorApi* g_api = NULL;
static char g_output[1024];

static void write_line(const char* path, const char* text) {
    FILE* f = fopen(path, "a");
    if (!f) return;
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
}

static void command_hello(void* userData) {
    char line[1200];
    const char* root;
    (void)userData;
    if (!g_api) return;
    write_line(g_output, "command ran");
    /* The root the editor reported during this call, not a copy kept from load(): the header says it is only valid
       for the length of the call, and this is what a plugin that followed the rule sees. */
    root = g_api->projectRoot ? g_api->projectRoot(g_api->userData) : "";
    snprintf(line, sizeof(line), "root=%s", root ? root : "");
    write_line(g_output, line);
    if (g_api->log) g_api->log(g_api->userData, "sample plugin: hello");
}

static int import_txt(void* userData, const char* path) {
    (void)userData;
    char line[1400];
    snprintf(line, sizeof(line), "import=%s", path ? path : "");
    write_line(g_output, line);
    return 0;
}

/* A command that does nothing, so the test can check two commands keep their own ids and labels. */
static void command_second(void* userData) {
    (void)userData;
    write_line(g_output, "second ran");
}

int ukiyo_plugin_load(const UkiyoEditorApi* api, UkiyoPluginInfo* info) {
    const char* out;
    /* The rule every plugin has to follow: the table may be shorter than the header it was compiled against. */
    if (!api || api->version < UKIYO_PLUGIN_API_VERSION) return 2;
    g_api = api;

    out = getenv("UKIYO_PLUGIN_SAMPLE_OUT");
    if (out) {
        strncpy(g_output, out, sizeof(g_output) - 1);
        g_output[sizeof(g_output) - 1] = '\0';
    }

    if (info) {
        info->size = (uint32_t)sizeof(UkiyoPluginInfo);
        info->name = "sample";
        info->version = "1.2.3";
        info->author = "UkiyoEngine";
        info->description = "A sample plugin: one command, one importer.";
    }
    api->registerCommand(api->userData, "hello", "Sample: say hello", command_hello, NULL);
    api->registerCommand(api->userData, "second", "Sample: the second one", command_second, NULL);
    api->registerImporter(api->userData, "TXT", "Sample text", import_txt, NULL);
    return 0;
}

void ukiyo_plugin_unload(void) { g_api = NULL; }
