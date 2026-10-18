# Editor panel audit

Grouped by panel. "Present" means the source has it (checked in `editor/src/`). "Missing" means it is
not in the source. Nothing here is implemented yet. We take these one panel at a time, then test each.

Reference for the default panel set: Unity's editor windows (Scene, Game, Hierarchy, Inspector, Project,
Console) as described in the Unity manual, "Unity Editor windows and views reference"
(https://docs.unity.com/en-us/engine/6000.5/manual/unity-editor/editor-windows-views-reference).

## 1. Menu bar

| Menu | Present | Missing |
|---|---|---|
| File | New Scene, Open Scene, Save, Save As, Open Model, Add Model to Scene, Models in Scene, Open Animation Library, Save Screenshot, Save Window Capture, Quit | Recent scenes, Revert scene, Import asset, Build/export |
| Edit | Preferences, Project Settings, Reset Camera (R), Focus Selection (F), Reset Project Settings, Undo, Redo, Cut, Copy, Paste, Duplicate (Ctrl+D), Delete (model instances, with undo history) | Select All (out of scope for this pass) |
| Prefab | Instance, Edit Transform, Apply Overrides, Revert Overrides, Create Prefab from Scene, Edit Prefab, Add Prefab to Open Prefab, Save Prefab, Exit Prefab Mode | Unpack, Variant prefabs (listed as not built), delete from prefab |
| View | Scene, Game, Play/Pause, Back to Start, Content Browser (icon, list, details) | Frame step, panel reset |
| Tools | Clear Console | Profiler, stats overlay, frame debugger, asset tools |
| Window | Layout presets, Shader Graph, panel toggles | Layout reset to default |
| Help | About, shortcuts | Documentation link, release notes |

## 2. Toolbar

- Present: play, pause, Focus, capture, Layout dropdown.
- Missing: step frame, layer and tag dropdowns, tool buttons (move, rotate, scale, rect).

## 3. Hierarchy

- Present: tree of nodes, parent and child display, selection, the Scene Camera row is removed (camera is the Main Camera).
- Done in this pass: Create > Empty Object, Model, Prefab; rename in place (F2, or Rename in the menu); duplicate, copy, cut, paste and delete from the right-click menu, with undo. Empty objects are saved as a flag and are not drawn in the Scene view yet (there is no marker, and no gizmo either).
- Missing: drag to reparent (not confirmed in source), search or filter, tags and layers.

## 4. Inspector

Sections present: Transform, Node, Prefab, Overrides, Instance, Play, Physics, Navigation, Audio, Animation, Script, Particles, Light, Lighting, Lens, Scene, Quality, Graphics, Color Grading, Bloom, Toon Look, Graph Material.

- Done: Add Component (menu of the systems that are off), Remove on each system (turns it off and resets its values), per-field reset to default on every system field, and a Lock button. A locked Inspector keeps its page, read-only, and selection changes do not replace it. Unlock shows the current selection. Selftest section 11k covers these.
- Limitations: the lock is read-only, not editable. Locked values do not refresh if the object changes in the viewport. The reset buttons are always enabled, even at the default. Remove does not ask for confirmation.
- Done: material slot list for mesh nodes (one row per primitive the node draws, with its material, shading, triangle count, and flags). Multi-object editing: Ctrl or Shift select several model rows, and the Inspector shows one page for them. Move by and Turn by are relative and undo as one step. Systems apply to all selected objects. Only a changed system is copied to the others, and each object keeps its own other systems and state machine. Selftest sections 11l and 11m cover these.
- Limitations of the slot list: it is read-only. Per-slot Shader Graph or MMD assignment is not yet available, and the node's setting applies to all its slots. Limitations of multi-object editing: the fields show the first object's values, with no mixed-value marker. Only model instances can be multi-selected. Cameras, lights and nodes stay single-select.

## 5. Scene view

- Present: navigation (orbit, pan, zoom) that moves the Main Camera, grid, selection.
- Done (gizmos pass): Move (W) with X, Y and Z arrows, and Rotate (E) with a yaw ring, both on the selected model instance. Each drag is one undo step. Escape during a drag puts the model back. Snap is a toggle (off by default) with move steps of 0.1, 0.25, 0.5 and 1 m and turn steps of 5, 15, 45 and 90 degrees. Verified by 24 selftest checks (section 11e), plus a screenshot (`renders/gizmo_scene_view.png`) that confirms the arrow directions.
- Not done: Scale handles are shown but disabled, because model instances have no scale yet. Gizmos are not shown for empty objects or prefab stages.
- Missing: view modes (shaded, wireframe), view gizmo (axis indicator), per-view stats.

## 6. Game view

- Present: aspect presets, play and pause, render at a chosen aspect, Color Grading, Bloom, Lens controls.
- Done (this pass):
  - Maximize on Play: while playing, the panels are hidden and the Game view is shown. Stopping restores the panels that were visible and the tab that was showing. Checkbox in the Game bar, remembered between runs.
  - Stats overlay: FPS, frame time (ms), mesh draw calls for the frame (shadow and scene passes), and render size. Drawn over the frame's top-left corner. Checkbox "Stats".
  - Gizmos: draws the selected object's move or turn handles over the frame, using the Game camera. Not interactive. Checkbox "Gizmos".
  - Mute Audio: silences the play-mode audio engine (Systems play). Applies to any audio engine created later. Remembered between runs.
- Limits:
  - Maximize hides the dock panels only. The menu bar and toolbar stay visible.
  - FPS is the editor's shared frame counter, so it counts frames from both views.
  - Gizmos shows only the transform handles. Other scene gizmos (light and camera icons, colliders, navigation) are not drawn in the Game view.
  - Mute covers the play-mode audio only. Timeline playback does not play audio.
- Selftest: section 12b covers draw-call stats, the stats box, the handles, mute, and maximize on play. Full run: 347 passed, 0 failed.

## 7. Project (Content Browser)

- Present: folder tree, icon, list and details views, search, thumbnails, prefab drag and drop, `.ukmat` material assets.
- Done (browser pass): New Folder (toolbar, right-click, or the empty area), Rename (F2 or the right-click menu), Delete (Delete key or the right-click menu; moves to the system trash after a confirmation), Import (toolbar button, right-click, or drop files and folders from the OS onto the grid, details view or a folder in the tree), Show in Explorer / Finder / Files, and a details pane with "Uses" and "Used by" lines.
- Rename rewrites every project file (.ukscene, .ukprefab, .ukmat, .ukshader) that names the moved file or folder. Rename and Delete are refused when the open scene uses the file (the model, an instance, a prefab, or the saved scene file names it), so the scene is never left pointing at nothing.
- Import never overwrites: a taken name gets " 2", " 3"... Importing into the folder a file already lives in is refused.
- Verified by 25 selftest checks (section 11f): the file operations run on a temporary project, and the browser's create, rename, import, trash and open-scene protection run on the real project in a temporary folder. Keyboard shortcuts and OS drag-and-drop were not exercised by the selftest; they need a manual check.
- Not done: none in this list.

## 8. Animation

Status: done for this pass, tested (selftest 305/0 with the Animation sections 11h and 11i).

- Present before: clip picker, animation library loading, Animation inspector section.
- Done:
  - Timeline with its own ruler, playhead and scrub. Lanes per track, an event lane, keys shown as diamonds.
  - Key editing: add by double-clicking a lane, move (drag, snapped to 1/30 s, Shift for free), delete, edit time and values in the properties strip, step or linear interpolation per track.
  - Curve view for one track and channel, with values dragged vertically. Double-click adds a key.
  - Events: add (double-click the event lane or the toolbar button), rename (double-click), delete, times in the list.
  - Tracks: add (node and property), remove.
  - Undo and redo for timeline edits (Ctrl+Z, Ctrl+Y), kept per panel.
  - Edits saved to `<library>.ukanim` beside the glTF. The glTF is not changed. The worker reads the file on library load and applies it, so a clip that grows with an edit plays with the new duration.
  - State graph (second tab): states as movable nodes, transitions as arrows, Connect mode, an "Any state" source, delete, default state, crossfade and minimum time per transition, conditions (Greater, Less, Equal, IsTrue, IsFalse, Trigger), parameters (Float, Bool, Trigger), start from the legacy Idle/Walk/Run machine, undo and redo.
  - Checks list (validateGraph) and a test pane that runs the real state machine with live parameter controls.
  - The graph is written to the object's components. The Inspector write copies the graph fields from the worker first, so Inspector edits do not drop the graph.
- Still missing or limited:
  - Events cannot be dragged on the timeline; their time is changed by the properties strip only.
  - No zoom on the timeline (it always fits the clip width).
  - Curve edits move values only; there are no tangent handles, and an edited cubic track becomes linear.
  - Scale tracks are stored and listed, but the retargeting sampler does not use scale.
  - Undo for the timeline and the graph are separate from the main window's undo stack.
  - The graph shows the object selected in the Inspector. It does not follow scene selection changes that bypass the Inspector.
  - Tests drive the editing operations directly. Mouse drag, double-click and keyboard handling were checked by running the editor and looking at screenshots, not by automated input tests.

## 9. Console

- Present: log, Clear Console (Ctrl+L), search, level toggles for Info, Warning and Error with counts. (Corrected in the console pass: search and level filters were already in the code, and the earlier audit wrongly listed them as missing.)
- Done (console pass): collapse repeated lines into one line with a count (the Collapse toggle); Copy for selected lines (Ctrl+C, or Copy in the right-click menu); double-click a line that names a project file to show an asset in the Project browser, or open any other file in the system's default application; Open Source in the right-click menu. Lines name files relative to the project root or to assets/, optionally with a ":line" suffix.
- Verified by 11 selftest checks (section 11g). 266/0 total.
- Not done: none in this list.

## 10. Shader Graph window

- Present: node canvas, default shaders open, Save As, Filter, Delete, `.ukshader` files.
- Missing: undo and redo, node search palette, preview window, live compile errors per node, duplicate node, group nodes.
- Done (this pass), selftest section 10b (360 passed, 0 failed in the full run):
  - Undo and redo: snapshots of nodes, connections and groups. Edits within one second share one step. Edit menu, Ctrl+Z and Ctrl+Y (or Ctrl+Shift+Z).
  - Duplicate Node (Ctrl+D): copies the selected nodes with their parameters, offset by 40 px.
  - Group Selection (Ctrl+G) and Ungroup Selection (Ctrl+Shift+G). Groups are saved in the .ukshader file.
  - Node search: Space while the canvas has focus, or Edit > Search Nodes. Filters by name, id or category. Enter or double-click adds the node at the mouse position.
  - Preview dock: renders the graph on a sphere, through the same path as the Project panel thumbnails. Updates 400 ms after an edit, and only while the dock is visible.
  - Per-node compile errors: a glslang error marks the node that wrote the failing line, and the Errors tab lists it. Activating an entry selects the node. Errors clear on the next graph edit.
- Limits:
  - The node error path is tested by marking a node directly and by checking the line-to-node map. A real glslang error from a graph is not produced in the test, because typed parameters always compile.
  - Ungroup is tested through a selected group frame. Selecting nodes inside a group did not reliably report as selected in the test, so that path is not covered.
  - Group nodes are plain frames for now. Renaming a group uses the default name "Group".

## 11. Material Inspector

- Present: exposed property editors for a `.ukmat`, thumbnails.
- Done (selftest section 11n, 364 passed, 0 failed in the full run):
  - Shader switch: a Shader list of the project's .ukshader files. Choosing one rewrites the material's shader. Values with the same name are kept.
  - Reset per property (enabled only when the material overrides the default), and Reset All.
  - Copy Values and Paste Values through the clipboard as JSON. Paste applies only the properties the material's shader exposes.
- Done: texture slot previews (selftest section 11t, 503 passed, 0 failed in the full run).
  - Shader Graph: a Sample Texture 2D node with an Exposed name becomes a texture slot. Its image path is the slot's default. A name cannot be used twice, and it cannot be both a value and a texture.
  - Files: `.ukshader` lists the texture slots (name, binding, default path). `.ukmat` has a `textures` map from slot name to image path. A path inside the assets folder is stored relative to it.
  - Renderer: a `GraphMaterial` binds each material's own textures, decoded with stb_image and owned by the material. A slot with no image uses the white fallback. Thumbnails rebind the cached shader material for every material they draw.
  - Inspector: one row per texture slot, with a thumbnail, the file in use, and Choose and Clear. Clear is enabled only for an override, and a missing file is marked "(missing)".
  - Reset All clears every value and every texture override. Copy Values and Paste Values still cover values only.
  - Refresh: a material's thumbnail is redrawn when its shader or any of its images changes. The live render was keyed on the `.ukmat`'s own time alone, so a shader edit or an image edit left the old thumbnail. The selftest checks both edits, and fails when the fix is removed.
  - The viewport draws a material's textures too. The selftest assigns the default and the red override to the scene's mesh and counts the red and blue pixels in the frame.
- Limits:
  - A shader has at most 8 texture slots (the sampler limit of the graph).

## 12. Project Settings

- Done (selftest sections 11o and 11p, 399 passed, 0 failed in the full run). Settings are kept in `ProjectSettings/ProjectSettings.json`:
  - Input: a table of actions and keys, with Add and Remove. Keys use portable key names, and combinations like Ctrl+Q are rejected. A script reads an action with `ukiyo.actionDown(name)`. Key presses in the Game view during play reach the script.
  - Tags and Layers: a user tag list (built-in tags are fixed) and 32 layer slots. Slots 0 to 7 are built in and fixed. Slots 8 to 31 take unique names.
  - Time: fixed timestep (1/120 to 1/20 s) and time scale (0 to 4). The fixed step drives physics. The time scale multiplies game dt, so 0 freezes play.
  - Physics: gravity X, Y, Z, and Jolt's velocity and position solver steps (1 to 32).
  - Build: product name, a scene list in build order (Add, Remove, Up, Down), and Write Build Manifest, which writes `Build/build.json`. The manifest does not package a playable executable.
  - Reset Project Settings also clears these values.
- Limits: time, physics, and input values apply when play starts. The key path is tested through the world and the settings controls. The Game-view key filter is not tested with a live play session.

- Present: settings pages (Quality, Graphics, Color Grading, Bloom, Lighting, Lens) and Reset Project Settings.
- Missing (before this pass): Input bindings, Tags and Layers, Time settings, Physics world settings (gravity, solver), Build settings. All five are now done, see below.

## 13. Prefab mode

- Present: edit prefab, add prefab to the open prefab, save, exit, apply and revert overrides, drag and drop.
- Missing (before this pass): add or delete models in prefab mode, variant prefabs, breadcrumb showing the nested prefab chain.
- Done (selftest section 11q, 410 passed, 0 failed in the full run):
  - Add Model in prefab mode: the model becomes a new child of the prefab root, placed at the root's position. It is saved with the prefab. Its id is m1, m2, and so on.
  - Delete in prefab mode: removes the object and everything under it. Save removes the node from the file. The prefab root cannot be deleted, and objects from a nested prefab are deleted in that prefab.
  - Breadcrumb in the status bar: "Outer > Inner > c1" for the selected object, from the cached flattening. "Prefab: name" when nothing is selected.
- Done, variant prefabs (selftest section 11r, 420 passed, 0 failed in the full run):
  - Prefab > Create Variant...: writes a new .ukprefab whose root v0 has one reference child, base, to the chosen base prefab at zero pose, then opens it in prefab mode.
  - Format: the reference node carries "overrides" (relative id path to {translation, yawDeg}) and "removed" (relative id paths). Flatten applies them to a copy of the referenced root before walking it. A path through a nested reference is handed down to that reference.
  - Save: moving a base object writes an override in the variant's reference. Deleting a base object adds it to the reference's "removed" list. The base prefab file is not changed.
  - Delete of a nested object is allowed when it comes through a reference. Otherwise it is refused, as before.
  - Limit: a nested object that cannot be reached through a reference is still saved only in its own prefab.
- Limits: each add or delete reloads every object in the stage, so a large prefab takes a moment. Duplicate and Create Empty Object are still refused in prefab mode.

## 14. Systems panels (six new engine systems)

Physics, Navigation, Audio, Animation, Script and Particles sections exist in the Inspector. Done in this pass (selftest section 11s, 448 passed, 0 failed in the full run):

- Physics: Done. Shape is Sphere or Box, and a box has a half size per axis, saved to the scene file. "Show colliders in Scene view" draws each body's collider as a wireframe (three great circles for a sphere, twelve edges for a box). It is a view setting and is not saved.
  - Limit: boxes are not rotated. A body's collider follows its position only.
- Navigation: Done. Bake Navmesh runs on the render thread with a progress bar that counts the bake stages. Preview Path finds a path on the navmesh from the object, 6 m ahead, and draws it in the Scene view with its corner count and length. A point off the mesh is refused with the reason.
- Audio: Done. Each sound has a Volume. Preview Sound plays the file once through the editor's audio device, outside play mode, and says why when it cannot play. The Master volume is the mix level for every sound. Mute Audio still silences everything.
  - Limit: there is one master level and a per-sound volume, not a bus mixer.
- Animation state: already present. The Inspector's Animation section has the node-graph state machine with states, transitions and parameters (selftest section 11i). The earlier "missing" note was out of date.
- Script (Lua 5.4): Done. The picker lists the .lua files under Assets. Check and Reload Script compiles the file without running it, and lists errors with the file and line. If Play is running, it restarts so the new script runs.
  - Limit: errors raised while the script runs are reported in the play warnings, not in this list.
- VFX (Particles): Done. A live emitter preview shows the live particles of the running play session from the side, refreshed five times a second. A size-over-life curve has two handles, for the size at birth and the size at death, and it follows the size fields.
  - Limit: only size is curved. Colour over life is still a start and end pair.

## 15. Status bar

- Present: Paused or Playing state, render time in ms, viewport resolution.
- Done: frame rate (always shown), memory use of the editor process (read from /proc on Linux, hidden on other systems), and the scene name, with "(unsaved)" when there are unsaved changes.

## 16. Dialogs and Preferences

- Present: Preferences dialog, with a Shortcuts page where each command can be rebound (the earlier "missing" note was out of date). Unsaved-changes prompt (Save, Don't Save, Cancel), Compare tonemappers dialog, Edit Transform dialog.
- Done: Edit > Find and Replace (Ctrl+H). It searches object names in the Hierarchy, with a Match case option. Find Next selects the next match, Replace changes the selected match, and Replace All renames every match as one undo step. It is refused in prefab mode.
  - Limit: the scope is object names only. It does not search prefab contents, scripts or asset names.

## 17. Animation controller: blend trees (Phases 2 and 3)

Done (selftest sections 11j2 and 11t, 503 passed, 0 failed in the full run).

- Phase 2, 1D blend: a state mixes several clips along one float parameter. The weights interpolate linearly between the two nearest thresholds. Outside the range the end motion takes all the weight.
- Phase 3, 2D blends:
  - Simple directional: each motion is a direction. The angle of the input mixes the two nearest directions. The input's length moves the mix toward the motion at (0, 0), which is the idle pose.
  - Freeform cartesian: each motion is a point. Weights fall off with inverse squared distance and are exact at each motion.
  - Freeform directional: each motion is a direction and a length. Several motions may share a direction. The distance from the input to a motion is the angle between them, scaled by the input's length, plus the difference in length. The motion at (0, 0) is at the input's length, so a short input leans toward idle. Weight falls off with the inverse squared distance and is exact at each motion. The file name is "freeformdir2d".
- Runtime: a blend state crossfades in when it is entered. While it stays current, its weights follow the parameters without restarting the clips. The player poses each member clip on its own clock. Bones are mixed by weight: rotations by a sign-aligned weighted sum, then normalised, and translations by a weighted average.
- Editor: the Animation graph side panel has a Motion type combo, Parameter X and Y lists (float parameters only), and a motion table with clip, X and Y columns and Add and Remove buttons. Blend nodes show their motion count. The test pane logs the mix when it changes.
- Scene file: blend states save their type, parameters and motions. Clip states write no blend fields, so existing files read as before.
- Validation flags a missing or non-float parameter, a blend with no motions, and a motion whose clip is not in the library.

Interpretation: Phase 2 is the 1D blend and Phase 3 is the three 2D blends listed above. The freeform directional weighting is our own design. It was not checked against Unity's own formula.

Limits:
- Interruption sources and layers or sub-state machines were not part of this pass.
- Blend states ignore the start-offset option, which applies to clip states only. A blend's exit time uses the mean length of its motions.
- The selftest covers the weights, the state machine's blend calls, the pose-mix helper, the file name, the validator and the graph panel. The file round trip for the new kind rests on that name check, since the file writer uses the same name function for every kind. It does not check a blended pose on screen in play mode.

## Build note

- Run the selftest under a display: `xvfb-run -a ./build/ukiyo_editor --selftest`. With no DISPLAY, glfwInit blocks at startup and the run prints nothing.
- Jolt's GPU compute shaders need both `glslc` and `dxc`, found in the same folder. CMake now checks for both and builds Jolt CPU-only when either is missing, with a status line. Passing `-DJPH_USE_VK=ON` still overrides this.
- main_window.cpp is built at -O1 (set in CMakeLists.txt). At -O3 it ran the build machine out of memory, and at -O2 it did too. The rest of the editor keeps the Release flags.

## Proposed order to work through

1. Edit menu and undo/redo (affects every panel, so it goes first). **Done:** undo, redo, cut, copy, paste, duplicate, delete for model instances; 186/0 selftest including the Edit tests (section 11c). Undo keeps the current view and selected model. Undo also restores scene settings from its snapshot, so a settings change that was not recorded as an edit can be reverted.
2. Hierarchy operations (create, rename, duplicate, delete, context menu).
3. Scene view gizmos and snapping. **Done:** move and rotate gizmos with snapping, 230/0 selftest (section 11e).
4. Project browser file operations. **Done:** new folder, rename with reference updates, delete to trash, import (drop and dialog), show in file manager, uses and used-by lines. 255/0 selftest (section 11f).
5. Animation timeline and state graph.
6. Console filtering. **Done:** collapse, copy, open source (level filter and search were already present). 266/0 selftest (section 11g).
7. Systems panels (Navigation bake, Audio preview, Script reload, VFX preview).
8. Remaining settings pages and status bar.
