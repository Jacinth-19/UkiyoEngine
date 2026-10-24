# Editor plugins

A plugin is a shared library that adds commands to the editor and can claim a file extension for import. The ABI is
plain C (`src/engine/plugin_api.h`), so a plugin can be written in C, in C++, or in anything that can export two
symbols.

This is deliberately a small surface, and it is small on purpose. The interesting thing about an editor extension
system is not how much it exposes; it is whether the things it exposes can be kept working. What is here is enough for
what plugins are actually asked to do — convert a file format the importer does not know, and put a button on a
repetitive job — and every part of it is checked by a test that loads a real library (`tests/plugin_test.cpp`).

## Where plugins live

```
<project>/Plugins/<anything>.so      Linux
<project>/Plugins/<anything>.dll     Windows
<project>/Plugins/<anything>.dylib   macOS
```

The editor loads every file with the platform's extension when it opens the project, in name order, and again when you
press **Reload Plugins** in the **Plugins** menu or on the **Plugins** page in Project Settings. Reloading makes a fresh
host: the old libraries are unloaded before the new ones are opened, so a rebuilt plugin is picked up without
restarting the editor.

A library that is not a plugin, a plugin built against a newer table, and a plugin whose `load` returns non-zero are
all reported — in the console, in the menu's page, and in the table on the Plugins page — and none of them stops the
editor from starting.

## Writing one

The whole interface is one function that fills in a struct and one that cleans up:

```c
#include "engine/plugin_api.h"

#include <stddef.h>
#include <stdio.h>

static const UkiyoEditorApi* g_api = NULL;

static void convert_command(void* userData) {
    (void)userData;
    g_api->log(g_api->userData, "converting...");
    g_api->setStatus(g_api->userData, "Converted", 2000);
}

static int import_csv(void* userData, const char* path) {
    (void)userData;
    FILE* f = fopen(path, "r");
    if (!f) return 1;            /* non-zero: the plugin declined, and the editor reports the failure */
    fclose(f);
    return 0;                    /* zero: imported */
}

UKIYO_PLUGIN_EXPORT int ukiyo_plugin_load(const UkiyoEditorApi* api, UkiyoPluginInfo* info) {
    /* The rule: the table can be shorter than the header you compiled against. Never touch a field you have not
       checked for. Version 1 has everything below. */
    if (!api || api->version < UKIYO_PLUGIN_API_VERSION) return 2;
    g_api = api;

    info->size = (uint32_t)sizeof(UkiyoPluginInfo);
    info->name = "csv";
    info->version = "1.0.0";
    info->author = "you";
    info->description = "Imports CSV tables as data assets.";

    api->registerCommand(api->userData, "convert", "Convert CSV Tables", convert_command, NULL);
    api->registerImporter(api->userData, "csv", "CSV table", import_csv, NULL);
    return 0;
}

UKIYO_PLUGIN_EXPORT void ukiyo_plugin_unload(void) { g_api = NULL; }
```

Build it as a shared library with no prefix:

```
gcc -std=c99 -fPIC -shared csv_plugin.c -I <engine>/src -o <project>/Plugins/csv.so
cl /LD csv_plugin.c /I <engine>\src /Fe:<project>\Plugins\csv.dll
```

The header is C99 and includes nothing but `<stdint.h>` — if it stops compiling as C, that is a bug in the header and
not in your plugin, and `plugin_test` fails on it.

Two things about the two functions themselves:

* **`UKIYO_PLUGIN_EXPORT` on both definitions.** The header defines it as `__declspec(dllexport)` on Windows, where a
  symbol otherwise stays inside the DLL and the editor's `GetProcAddress` comes back empty, and as nothing anywhere
  else. If your build already exports every symbol its own way, define `UKIYO_PLUGIN_EXPORT` to that before including
  the header and the macro steps aside.
* **Include the header before you define them, in C++ too.** The header declares both functions inside `extern "C"`,
  so a definition in C++ that follows it and matches it exactly keeps C linkage; a plugin that defines them without
  the header's declaration gets C++-mangled names the editor cannot look up. Signatures have to match the header —
  `const UkiyoEditorApi*` and all.

### What the table gives you

| Function | What it does |
| --- | --- |
| `log(userData, message)` | writes a line to the editor's console |
| `setStatus(userData, message, ms)` | shows a message in the status bar for `ms` milliseconds |
| `projectRoot(userData)` | the open project's root; valid **only for the length of the call** |
| `scenePath(userData)` | the open scene's path, relative to the project root; also only for the length of the call |
| `registerCommand(userData, id, label, run, commandUserData)` | adds a Commands entry to the Plugins menu |
| `registerImporter(userData, extension, label, import, importerUserData)` | claims an extension for import |

Two things to know:

* **Do not keep the strings.** `projectRoot` and `scenePath` hand back a pointer that is valid until your call returns;
  copy it if you need it (in a command, the project root is the natural base for a path you are about to open).
* **Command ids are per plugin.** `registerCommand(..., "convert", ...)` from `csv.so` becomes `csv.so/convert`, so two
  plugins can both have a `convert` without colliding. The id is what the menu records and what a future scripting
  hook would call.

### What happens on failure

* `ukiyo_plugin_load` returning non-zero: the plugin is refused, and **anything it registered before failing is
  dropped**, so a half-loaded plugin leaves nothing behind.
* A missing `ukiyo_plugin_load` symbol: reported as "it does not export ukiyo_plugin_load".
* An `info` block with a `size` smaller than the host's, or no name: refused, with the reason.
* `ukiyo_plugin_unload` is optional and is called before the library is closed.

## What is not here yet

* A plugin cannot read or write the scene document, add editor panels, or draw in the viewport. Those need a permission
  model before they can be given to an arbitrary library, and until then the safe way to script the editor is
  `docs/LUA.md`'s tooling and the command line (`ukiyo_pack`, the shader tool), which a plugin command can shell out
  to.
* Plugins are loaded from the project, not from a user-wide directory, so a plugin travels with the project that needs
  it — which is also what makes "it works on my machine" hard to hide.
* No hot reload of a single plugin: **Reload Plugins** tears down all of them and loads them again.

## Tests

`tests/plugin_test.cpp` loads two libraries built by the C compiler — one that registers a command and an importer and
writes a file when they run, and one that registers a command and then fails. It checks the ABI end to end: the symbols
export, the table survives past `load()`, a command reaches the plugin's code with the project root the editor
reported, an importer receives the exact path it was given, a claimed extension is routed to its owner and an
unclaimed one is not, a failing plugin's command is taken back, and unloading leaves nothing pointing into a closed
library. The editor's own self-test checks the other half: a `Plugins` folder containing something that is not a plugin
is reported, not fatal.
