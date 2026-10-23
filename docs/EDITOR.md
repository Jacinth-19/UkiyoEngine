# The editor

The editor is a Qt window over the engine. It has the shape Unity taught everyone — Hierarchy, Scene and
Game, Inspector, Project Settings, Console — because the people who will use it already know that shape. What
follows is the part that is specific to this engine: where a panel's work actually happens, and which of it is
tested without a window.

Everything here is C++ in `editor/src`, and everything that could be tested without Qt was taken out of the
window and tested on its own. The editor's own self-test (`ukiyo_editor --selftest`) drives the rest through
real mouse and key events on a real device, in CI.

## Play

One button. It starts the clock *and* the world, from whichever tab is open, and stops both. While it is on,
the keyboard belongs to the game: keys reach the running world through `World::setKeyHeld` regardless of
whether the Scene or the Game tab has focus. A Scene that cannot start its systems (no navmesh, say) still
plays its animation, and the reason is logged rather than swallowed by a refused Play.

## Two cameras

The Scene view and the Game view are different cameras (`gameCam_` and `playCam_`).

* **Scene** is where a level is built. View → *Orthographic Scene View* swaps its projection without moving
  the eye; the arrow keys fly it, Page Up and Page Down rise and drop. Zooming an orthographic view changes
  how much world it covers, not how far away the camera is, and panning follows the mouse at that scale.
* **Game** is the player's. Until it is moved or Play is pressed it follows the Scene view, so the first press
  of Play shows the view the author was working in. After that it stays where it was put.

`OrbitCamera` carries both behaviours (`src/engine/camera.h`), and `tests/camera_test.cpp` holds the
projections, the orthographic pan and zoom, and flying to their definitions.

## Undo is a delta

`undoStack_` no longer holds a copy of the scene per edit. A step is a JSON Patch (`src/engine/json_patch.h`):
the operations that turn the document as it was into the document as the edit left it, plus the inverse, built
in the same pass. Undo applies the inverse to the current document and pushes the forward patch onto the redo
stack; redo applies the forward patch. A step therefore costs what the edit changed, and a scene with a terrain
tile in it no longer makes every keystroke expensive.

* A step is taken when an edit **starts**, as before, so Undo is available immediately and a failed operation
  can drop the step it took (`cancelEdit` → `dropTopEdit`).
* `flushEdit()` records the edit in progress; undo, redo, save and Play call it first, so none of them loses the
  last change.
* Terrain strokes carry their undo records in the step (`SceneEdit::terrain`).
* Prefab mode has its own stack, of the objects the stage shows and the keys it maps them by, because a prefab
  stage is not a scene document. Undo works there now, which it did not before.

## Assets

An asset is a file with a sidecar: `<file>.meta` holds a GUID that belongs to the asset rather than to its path,
and the importer settings that decide how it enters a scene.

```json
{ "format": "ukiyo-asset", "version": 1,
  "guid": "9f3c…", "kind": "Model",
  "importer": { "scale": 1.0, "yawDeg": 0.0, "lods": [], "materialRemap": "" } }
```

`.ukiyo/assets.json` indexes GUID → path. With those two things the Assets page can scan a project, list
references that no longer resolve, and repair references after files were moved outside the editor — by GUID,
not by guessing at names. References stay plain project-relative paths, so a shipped game needs no runtime
support for any of this.

The page also applies the import settings: **Add to Scene** inserts exactly what `sceneEntryForAsset` builds,
scale, facing correction, LOD ladder and material remap included. All of it is tested without Qt
(`tests/asset_db_test.cpp`).

## Build and package

The Build page writes `Build/build.json` (the manifest the player reads) and then runs the packer. Target,
version and icon sit above the Package button; the button writes the version into the manifest through
`pack::setManifestVersion`, builds the argument list through `pack::packArgs` — the same one `ukiyo_pack`'s test
uses, so the editor cannot drift from the tool — runs `ukiyo_pack`, and starts the packaged game.

If the tool or the player is missing, the page says which path it looked at instead of failing quietly.

## Localisation

One file per language: `assets/locales/<code>.json`.

```json
{ "format": "ukiyo-locale", "version": 1, "locale": "ja", "strings": { "key": "text" } }
```

`ukiyo.tr("key")` returns the text, or the key itself when the language has no line for it, so a missing
translation is visible rather than blank. `ukiyo.locale()` says which language the game loaded. The page lists
the languages in a project, edits their strings, and starts a new language as a copy of the base language's
keys with the base text — untranslated, not empty.

## Version control

The Version Control page runs `git` as argv in the project directory: status, add all, commit, and the last
twenty commits. It decides nothing about merging, branching or history — that is git's job, and an editor that
tries to be cleverer than git is an editor that loses work. `git status --porcelain=v1 -b` is parsed by
`src/engine/vcs.h`, which is tested on its own.

## Profiler

`World::tick` reports what each stage cost (`FrameTiming`) behind a switch, so a shipped game pays nothing for
it. The Game view's stats overlay shows the total and the stages that took measurable time. The number is CPU
time the game's own systems spent — physics, scripts, animation, navigation, particles — and it is deliberately
not the GPU frame time, which the RHI does not expose.

## The gizmos

Both gizmos follow Blender's, because the shapes and the rules a designer's hand already knows are worth more
than a house style: `source/blender/editors/transform/transform_gizmo_3d.cc` for the transform handles, and
`source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc` for the corner axis widget. What is borrowed
is the *behaviour*; the geometry, drawing and drag code here are this engine's own.

The transform gizmo has three sets of handles, one per tool:

| Tool | Handles |
| --- | --- |
| Move | three arrows (X, Y, Z) and three plane squares (XY, YZ, ZX) |
| Rotate | one ring per axis; the half of a ring behind the object is drawn faint |
| Scale | three cubes, one per axis, and a cube at the centre that scales all three together |

Three rules from Blender make it feel right, and each is in `src/engine/gizmo_math.h` with its own test:

* **Hit tests are in pixels.** A handle is grabbed if the pointer is within eight pixels of what was drawn, so
  a handle is equally easy to catch whether the object is near or far. The math lives in a Qt-free engine file,
  `src/engine/gizmo_math.cpp`, so it runs in CI without a window (`gizmo_math_test`, 36 checks).
* **A handle that points at you is not drawn, and cannot be clicked.** `axisVisibility` is Blender's
  `gizmo_get_idot`: an axis is shown when `1 - |dot(view, axis)| >= 0.02`, and a plane when the angle is larger
  (`|dot| >= 0.175`), because a plane seen edge-on is a line you cannot aim at. A dot is not a target.
* **The nearest handle wins.** A plane square under the pointer takes the click outright, since a square cannot
  be pressed by accident; otherwise the closest line does, and a hovered or dragged handle is drawn wider and in
  the highlight colour.

A drag writes the transform once per mouse-move and takes **one** undo step for the whole drag, pushed when the
drag starts and dropped again if the pointer never moved. `Escape` cancels a drag and puts the object back.

Rotation is the engine's Euler order (ZXY, so yaw about Y, pitch about X, roll about Z). The Inspector shows
the three angles and the three scale axes; a scene file writes `yawDeg` while a model only turns, and
`rotation: [pitch, yaw, roll]` plus `scale` (a number when even, a vector when not) once it is tilted or
stretched — the same fields `SceneRuntime` reads.

The view gizmo sits in the top-right corner of the Scene view: six handles on a unit sphere, drawn back to
front and mixed toward the view background by depth, so a ball behind reads as behind. Clicking one looks along
that axis and switches the view to orthographic, which is what the widget is for — checking a silhouette or a
side elevation against a known direction.

## What is not here yet

* Multi-object transform. The gizmo edits the selected instance.
* A plugin API for editor extensions.
