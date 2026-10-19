# Ukiyo Hub

The Hub is the launcher for Ukiyo Engine projects, in the spirit of Unity Hub: it lists the
projects you have opened, creates new ones from the built-in templates, and opens everything in
the World Editor.

```
build/ukiyo_hub
```

It is built with the editor (both are Qt6 Widgets targets), so `cmake --build build --target ukiyo_hub`
is enough after a configure.

## Creating a project

**New project** asks for a name, a folder, and a template, then shows the loading splash while the
project is written. The splash is the picture in `editor/res/splash.png` with:

- the project name and the folder it is being written to,
- a progress bar that eases toward each real step,
- a line that names the work in progress, for example *Sculpting the terrain: 257 x 257 samples*,
  *Scattering grass and pines*, or *Linking the character from the engine assets*,
- a percentage and a turning ring, so a slow disk is visibly working.

The steps come from `engine::createProject` itself, not from a timer, so the line always matches
what is happening. Opening an existing project shows the same splash while it reads the manifest
and checks the main scene, then starts the editor with `--project`.

The editor shows the same splash while it starts the engine (render device, animation library,
scene). Pass `--no-splash` to skip it, or `--selftest` to run the headless checks.

## What a project is

A project is a plain folder:

```
My Project/
  ukiyo.project.json                     name, template, engine root, main scene
  ProjectSettings/ProjectSettings.json   product name, input actions, tags, time and physics
  Scenes/Main.ukscene                    a version 2 Ukiyo scene
  Scripts/hello.lua                      a starter script for play mode
  assets/                                the project's own files
    README.md
    Terrain/hills.u16                    (Terrain Lab) a 257 x 257 raw heightmap
```

Template content that ships with the engine (the starter character, for example) is **not copied**.
The scene stores a path relative to the project, and the manifest records the engine root, so a
new project is a few kilobytes and still opens the character. Moving the engine checkout after
creating a project breaks that link; the scene then reports the missing model in the Console.

## Templates

| Id | Name | What it sets up |
|---|---|---|
| `empty` | Empty Scene | One empty object, studio light, anime tonemapping, grid and outlines, starter script. |
| `anime` | Anime Character | Seed-san linked from the engine assets, portrait camera, outlines and bloom. |
| `terrain` | Terrain Lab | Generated 257 x 257 heightmap with a lake basin, grass and pine foliage, water plane. |

Templates live in `src/engine/project_template.cpp`. Add one there and it appears in the hub's
Templates page and in the new-project dialog without any other change.

## Editor arguments

```
ukiyo_editor --project <folder>   open a project made by the hub
ukiyo_editor --no-splash          start without the loading splash
ukiyo_editor --selftest           run the editor's built-in checks and exit
```

Without `--project` the editor uses the engine checkout, as it did before projects existed.

## Design preview

`docs/index.html` is a static mockup of the hub, the new-project dialog, and the splash. It uses the
same layout, palette and the real progress lines (pulled from `project_template.cpp`), so the
launcher can be reviewed without a Qt build. It is not part of the application.

```
python3 -m http.server 8000 --directory docs
```

## Checks

```
./build/project_test                              # templates, manifests, scaffolds, heightmap
QT_QPA_PLATFORM=offscreen ./build/ukiyo_hub --selftest   # hub, dialog, splash, launcher paths
```

`project_test` needs no window or GPU. The hub self-test runs offscreen, so it works in CI.
