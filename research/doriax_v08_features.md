# Doriax v0.8 feature list

Source: https://github.com/doriaxengine/doriax/releases (v0.8 notes, 10 Oct). There is no "v8.0"; the newest release is v0.8.1 (14 Oct), whose fixes are editor and export only. Doriax is MIT-licensed. Our rule: re-implement the ideas, do not copy code.

## Rendering
- Custom post-process passes: a per-scene chain of forked fullscreen shaders with editable uniforms
- Custom uniforms in forked shaders, and a forkable depth shader for shadows and SSAO
- Mesh LOD generated with meshoptimizer (opt-in), per-view instance culling, distance fade and cull distance for instanced meshes
- Optional depth prepass, cheaper shadow PCF filtering, leaf transmission in materials
- Draw call and triangle counters, scene benchmark command in the editor

## Water
- Water component: waves, reflections, refraction, foam, shadows, caustics, underwater view
- Buoyancy for 3D bodies, and exclusion volumes that keep water out of hulls

## Terrain
- Foliage scattered from painted density maps, culled per chunk, distance fade, height limits, shadow casting
- Placement brush that scatters instanced objects
- Detail layers blended by height and slope, paint masks, PBR surface per layer
- Erosion, noise, stamp, terrace and mask brushes

## Physics
- Physics interpolation (on by default)
- Box2D and Jolt can be left out of exported games; ray casts against mesh bounds work without either
- 2D sensor shapes; 3D bodies can change safely from contact callbacks

## Scenes and animation
- Loading scenes that cover scene switches, loading holds, `Engine.onSceneLoaded`
- Background preloading of a scene's textures, sounds and models
- `SceneManager.addChildScene` from code; SceneManager scene pointers in Lua
- Relative and looping keyframe tracks, action start offsets, ping-pong and spinning actions, hover and press scaling for buttons

## Editor
- MCP server for outside agents, multiple named OpenAI-compatible endpoints for the AI chat
- FBX import
- Detach Camera (F8) to look around a running scene; touch device simulation in Play
- Entities created by a running scene listed read-only in the Structure
- Mesh parts of imported models can be reparented
- Export presets, editor and per-platform project settings, C++ standard setting, separate editor VSync
- Per-user editor state and machine-specific build settings moved out of `project.yaml`
- Reworked menus and save flow, About window, loading screen on project open
- Code editor: git change markers, branch in footer, middle-drag column selection, add-script-event menu
- F2 rename, Backspace delete, reveal in file manager, New menu for Lua and C++ files
- Default font covers Cyrillic, Greek and Vietnamese

## Scripting
- Lua constructors that wrap existing entities; writable data members
- `Quaternion * Vector3` and `Quaternion.lookRotation`
- Lua event handlers can return values (for filters such as `preSolve2D`)
- `System.isTouchDevice`, UI layout pivot, camera `depthTest` flag

## Ads, purchases and web portals
- AdMob (consent, banner, interstitial, rewarded, rewarded interstitial, app open ads)
- In-app purchases: Google Play Billing and StoreKit 2
- `WebPortal` service: CrazyGames, Poki, GameDistribution, Yandex Games, YouTube Playables (cloud saves)

## Export and platforms
- Optional `resources.pak` packing assets and Lua scripts
- Source exports include a `doriax.py` build script; CMake iOS and macOS bundles include assets
- Android SDK 36; iOS UIScene life cycle
- Docker and Nix build configurations for the editor

## Examples
- Tappy Plane, Lost Slime, Lost Slime 3D

---

# Plan for Ukiyo (scope: Ukiyo-relevant items, from v0.8)

Checked against the tree on 2026-10-15. "Exists" means a first version is already in Ukiyo.
Not listed here: ads, purchases, web portal SDKs, 2D tilemaps and sprites, example games, Docker and Nix.

| Batch | Items | Status in Ukiyo |
|---|---|---|
| 1. Small editor and runtime | Detach camera (F8); touch simulation in Play; touch device check in Lua; F2 rename and Backspace delete; scene benchmark command; draw call and triangle counters | Draw call text exists in code; no detach camera, no touch simulation, no benchmark command |
| 2. Scenes and animation | Scene preloading (textures, sounds, models); loading holds and `onSceneLoaded`; relative and looping keyframe tracks; action start offsets; ping-pong and spin actions; hover and press scaling | Scene loading, keyframes, and loop exist in part; preloading and ping-pong missing |
| 3. Rendering | Depth prepass; cheaper PCF; leaf transmission; per-view instance culling; distance fade and cull distance for instances; mesh LOD | Instancing and culling exist in part; prepass, PCF cost, leaf transmission, and distance fade missing; mesh LOD built (opt-in, see item 15) |
| 4. Physics | Physics interpolation; ray casts against mesh bounds; 2D sensors (skip: Ukiyo is 3D); safe body changes in contact callbacks | Jolt is used; interpolation status unchecked |
| 5. Water | Water component (waves, reflection, refraction, foam, shadows, caustics, underwater view); buoyancy; exclusion volumes | Not present |
| 6. Terrain and foliage | Foliage from density maps; chunk culling; placement brush; detail layers by height and slope; erosion, noise, stamp, terrace, mask brushes | Terrain is missing (one unrelated hit only) |
| 7. Import and export | FBX import; `resources.pak`; source export build script; Android SDK 36 target | FBX missing |
| 8. Scripting | Lua constructors wrapping entities; writable data members; `Quaternion * Vector3` and `lookRotation`; handlers returning values | Lua 5.4 exists with a `ukiyo.*` API (corrected) |
| 9. Editor extras | MCP server for outside agents; several AI endpoints; per-user editor state; export presets | Partly (AI and MCP hits exist; unchecked) |

Notes:
- CORRECTION: Ukiyo has Lua 5.4 (`third_party/lua`, `ukiyo.*` API, `ScriptComponent`). The scripting items apply and are not blocked.
- Water and terrain are each a full system. Each needs its own design before code.

---

# Batch 1 status (2026-10-15)

Done:
- Status line shows draw calls and triangles per frame (`renderer.cpp` counts all three mesh draw paths; `frameReady` carries triangles).
- Tools > Benchmark Scene: 10 warm-up frames, then 120 timed frames. Reports average, min and max render ms, draw calls and triangles per frame (log and status bar).
- Backspace deletes in the Resources browser, like Delete. F2 rename already worked for model rows and the browser.
- Self-test block 12 (564 passed, 0 failed). It checks the triangle counter and the benchmark result on the test scene, not on the character yet.

Not applicable yet:
- Detach camera (F8): Ukiyo has no scene camera component. Play mode uses the one editor orbit camera, so there is nothing to detach from. Revisit when scene cameras exist.
- Touch simulation in Play, and the touch device check in Lua: the engine has no touch input and no Lua.

Not done: Backspace delete in the hierarchy tree (it would conflict with keyframe and graph deletion; needs a focus-aware handler).

---

# Batch 2 status (2026-10-15)

User choice: "foundations" (scene switching with loading holds and preloading, then actions). Hover scaling is a separate job (no UI toolkit yet).

Done:
- Spin and ping-pong actions (engine). `ActionComponent` in `components.h` (per instance: spin speed in deg/s; ping-pong axis, amplitude, speed in cycles/s, linear or ease). `World::setAction` and `updateActions` in `world.cpp`. Play start attaches it in `engine_session.cpp`. Scene file key `action` in `systems_json.cpp`.
- Entities with a physics body are skipped by actions (physics owns them).
- Tests: systems_test 60 passed, 0 failed (new `testActions`). Editor self-test 564 passed, 0 failed.

Not done:
- Inspector UI for the action. It can only be set by editing the scene file for now.
- Runtime scene switching, loading holds and `onSceneLoaded`, scene preloading. The `.ukscene` reader and writer (`sceneDocument`, `openScene`) live in editor code, so they must move to the engine first.
- Hover and press scaling. Needs a UI toolkit; none exists.
- Ukiyo's spin uses the instance's current yaw as its start. The scene file's placement yaw is not checked yet.

## Batch 2 update (same day)

Done since the last status:
- Inspector "Actions" section (Kind, Speed, Axis X/Y/Z, Amplitude, Ease, plus the Add Component entry and the remove button). Undo compares the action fields (`sameAction`). Widget names: actionKind, actionSpeed, actionAxis0-2, actionAmplitude, actionEase.
- The systems round-trip test now saves and reopens an action (edited scene field by field). Editor self-test: 564 passed, 0 failed.

Still open in batch 2:
- Runtime scene switching. Plan: move the `.ukscene` entry parsing out of `applySceneDocument` (main_window.cpp) into a shared module, then add `EngineSession::loadSceneFile` on top of `clearInstances` and `addModel`, and restart play after the switch. Not started.
- Loading holds and `onSceneLoaded`. Plan: `ukiyo.loadScene`, `ukiyo.holdLoad` and `ukiyo.releaseLoad` in the Lua API, and the switch waits while holds are open. Not started.
- Preloading. Only sounds are cached (`World::soundFor`). Models have no cache, so preloading needs a model cache first. Not started.
- Hover and press scaling. Needs a UI overlay (no UI toolkit exists). Not started.

## Batch 2 steps 1 to 3 (same day)

Done:
- Scene reader moved out of the editor window into `editor/src/scene_file.{h,cpp}`. Open Scene and the runtime
  switch both use it. `applySceneDocument` keeps the render settings and the scene setup, and no longer parses instances.
- Lua calls: `ukiyo.loadScene(path)`, `ukiyo.holdLoad()`, `ukiyo.releaseLoad(token)`, `ukiyo.preloadModel(path)`,
  and the `onSceneLoaded()` hook after `onStart`.
- Runtime switch in play (`EngineSession::switchScene`). It waits while any hold is open, then runs between frames.
  The first switch keeps the editor's scene, and Stop puts it back without reloading models.
- Preload cache of up to 8 models. A staged model is used once, and is dropped if its file or the smooth-normals setting changed.
- Save is refused while a switched scene is in play.

Tests:
- `systems_test`: 76 passed, 0 failed (16 new scene checks: holds, replace, preload queue, Lua calls, onSceneLoaded).
- Editor self-test: 578 passed, 0 failed. New section 13 covers the switch with the script, the Hierarchy rows,
  the save refusal, Stop restoring the editor scene, a second switch, opening a scene during a switched play, and preloading.

Limits:
- A preload reads on the render thread, so it stalls one frame. Background loading is not built.
- The standalone `ukiyo` player does not load scenes, so switching works in editor play only.
- If the new scene fails to start, play stops and the editor's scene comes back.
- Changes made to a switched scene during play are dropped at Stop. Opening or creating a scene ends play first.
- The self-test benchmark waits 240 s, not 120 s. Software Vulkan renders about one 960x540 frame a second here,
  and the benchmark needs 130 frames. Measured: 123 frames in 120 s. The check still needs a finished report.

Still open in batch 2: hover and press scaling (needs a UI overlay, no toolkit yet).
Batch 3 not started.

## Easy items from the remaining list (same day, after batch 2)

- Item 1, Backspace in the Hierarchy: done. It deletes the selected model, like Edit > Delete. It is a key filter on the
  tree widget only, so rename editors and the animation panel keep their own keys. A scene keeps at least one model.
- Item 2, physics interpolation: done, on by default (`PhysicsWorld::setInterpolation`). Dynamic bodies are drawn between
  their last two fixed steps. `setPosition` jumps with no blend. Physics results are unchanged.
- Item 3, Lua vector and quaternion helpers: done. `Vector3(x, y, z)`, `Quaternion.lookRotation(forward [, up])`,
  `Quaternion.angleAxis(deg, axis)`, `Quaternion * Vector3`, `Quaternion * Quaternion`, Vector3 `+ - *`, and `tostring`.
  Fields are read-only. Writable data members and entity constructors are still open (item 11).
- Item 4, per-user editor state: nothing to move. Per-user state already lives in `QSettings` (`editor::prefs`).
  `ProjectSettings/ProjectSettings.json` holds only project-wide values, and no machine-specific build settings exist.
- Item 5, keyframe tracks and action start offsets: partly done.
  - Done: action start offset. The entity waits at rest for `startOffset` seconds (inspector "Start after (s)", scene key
    `action.startOffset`).
  - Already there: clip looping. Clips wrap at their duration (`sampleClip`).
  - Not done: relative keyframe tracks. This needs a per-track flag, sampling against the rest pose, the clip file, and the
    animation panel. It is medium work and is the next piece of item 5.

Tests: `systems_test` 91 passed, 0 failed (new: physics interpolation, Lua value types, action start offset).
Editor self-test 582 passed, 0 failed (new: Backspace in the Hierarchy, start offset in the scene file).

## Medium items 6 to 14 (same day, after the easy items)

Built in the agreed order. Status per item:

- 7, cheaper shadow PCF: `shadowFactor` takes four `textureGather` taps at ±0.5 texel, a tent 3×3 filter (weights
  1-2-1 by 1-2-1, over 16) in place of nine taps. Shader compiles. Not checked against a reference image (no GPU here).
- 10, leaf transmission: `leaf` vec4 in the material UBO (x strength, y view falloff power). Set per material from the
  glTF material `extras` keys `leafTransmission` (0 to 1) and `leafPower` (at least 1). `mtoon.frag` adds
  albedo × sun × pow(V·−L, power) × max(−N·L, 0) × strength. Limits: the transmitted term ignores sun shadows, and
  `mmdtoon.frag` does not have it. Not checked visually.
- 6, depth prepass: `depth.frag` runs the four discards of the colour shaders in the same order (dither fade, distance
  fade, character dissolve, alpha cutout), so the depth matches. In-view opaque toon draws write depth first (pipelines
  `pDepth_`). The scene pass no longer clears depth. Setting `RenderSettings::depthPrepass` (default on). Sky and grid
  need no change: sky has no depth test, and the grid still loses to objects in front of it.
- 8, distance fade and cull: `Model::cullDistance` and `Model::fadeDistance` (0 = off). A model past its cull distance is
  skipped. Between the fade distance and the cull distance it is dithered out (push constant `p2.x`, 112 bytes of 128).
  `fade_discard.glsl` gives every colour and shadow fragment shader the same discard, including outlines. Graph
  (custom) materials do not fade yet.
- 9, per-view culling: `src/engine/cull.h`. `boxOutsideFrustum` tests the model's union bounds, moved by each draw's
  node matrix and grown by 0.25 m (for morph targets), against the unjittered main-camera view-projection. Static draws
  only. Skinned draws are never culled. The shadow pass keeps every caster. `RenderSettings::frustumCull` (default on).
  Limit: a culled draw does not update its motion-vector slot, so it can show one frame of velocity error when it
  comes back into view.
- 12, contact callbacks: a Jolt `ContactListener` queues added and removed contacts under a mutex. `World` runs
  `onContact` on the main thread after each step. Body changes made in the handler are deferred to the next step
  (`PhysicsWorld::setDeferChanges`). Persisting contacts are not reported.
- 11, Lua entities and filters: `ukiyo.Entity(name)` handles with writable `position` and `yaw`, read-only `name` and
  `body`, and methods `impulse`, `setParam`, `moveTo`. `ukiyo.setContactFilter(a, b, enabled)` is a native pair filter
  in Jolt's validate callback. The user chose this design, because the solver cannot run Lua. Handler return values
  are not implemented (see `systems_design.md`).
- 14, `resources.pak`: `src/engine/vfs.{h,cpp}` (format `UKPAK1`, uncompressed), `src/engine/vfs_cgltf.h` (cgltf read
  hook), and the packer `tools/ukiyo_pak.cpp`. Model, animation, texture, face SDF, graph texture, LUT, and script reads
  go through the VFS. No runtime entry point mounts a pack yet (`src/app/main.cpp` unchanged).
- 13, hover and press scaling: deferred. The user chose to hold it until a UI-layer design check-in. The runtime has
  no button, no pointer input for scripts, and no entity scale.

Depth prepass benchmark (`Tools > Benchmark Scene`, 120 frames, self-test fixture, 50-triangle scene). This machine has
no GPU, so Vulkan runs in software (lavapipe under Xvfb). Treat these numbers as a sanity check only:

| Run | Render average | Min | Max | Draw calls per frame | Triangles per frame |
|---|---|---|---|---|---|
| Prepass off (`UKIYO_NO_PREPASS`, a temporary toggle, removed) | 737.26 ms | 628.73 | 942.62 | 20 | 40 |
| Prepass on | 721.39 ms | 628.94 | 830.31 | 25 | 50 |
| Prepass on (earlier run, same build) | 731.97 ms | 632.73 | 1120.00 | 25 | 50 |
| Prepass on (final build, after the fade fix) | 765.25 ms | 650.28 | 887.94 | 25 | 50 |

The gap is inside the run-to-run spread. The fixture is too small to show overdraw, and a software renderer does not
reflect GPU fragment cost. A real gain needs a GPU and a dense scene. The extra 5 draws and 10 triangles per frame are
the prepass over the opaque draws. The later fade fix (outline, mmd edge, and shadow) does not change the prepass path.

Tests: `systems_test` 127 passed, 0 failed (new: frustum and distance rules, contact dispatch and deferral, Lua entity
handles, the contact filter, and the pack round trip including reads through cgltf). Editor self-test on the final build: 582 passed, 0 failed.

## Item 15, mesh LOD with meshoptimizer (opt-in)

- Licence: meshoptimizer is MIT (copyright 2016-2026 Arseny Kapoulkine). Checked from the upstream `LICENSE.md` on
  2026-10-16. Vendored under `third_party/meshoptimizer/` at commit f0a684f11ead064e176775c9b796f82a55910e92 (v1.3),
  with `LICENSE.md` and a `README.txt` that records the commit. Only `meshoptimizer.h`, `simplifier.cpp` and
  `allocator.cpp` are included. Both `.cpp` files are in `ukiyo_core`. No Doriax code is copied; the feature idea
  only.
- `buildModelLods(device, model, switches)` in `src/engine/model.{h,cpp}` builds the levels. Level k targets 0.5^k of
  the indices with `meshopt_SimplifyLockBorder` and a 0.05 target error (a fraction of the mesh extent). Levels are
  built from the full-detail CPU indices. A level is dropped when the simplifier does not shrink the previous level,
  and the renderer uses the last built level beyond that. Calling it with empty switches turns LOD off and clears the
  levels.
- Opt-in. A model has no LOD until its switch distances are set. Nothing is set by default, so the default output is
  unchanged.
- Editor and scene file (wired after the first version). Each object has an optional `lod` array in the `.ukscene`
  file, for example `"lod": [25, 60]`, in metres. It is omitted when empty. The Inspector has a "Level of Detail" row
  with a "Switch at (m)" field, comma separated. Edits are undoable. Opening a scene, undo and redo, and play-mode scene
  switches all rebuild the levels. Values are cleaned on read by `normalizeLodSwitches` (positive, sorted, unique, at
  most three distances, so at most 1/8 of the indices).
- `lodForDistance(distance, switches)` in `src/engine/cull.h`. Distance is from the eye to the model centre. The level
  is the number of switch distances reached (for example, switches {25, 60}: 10 to level 0, 25 to level 1, 60 to
  level 2).
- Static draws only. Skinned draws stay at full detail. The shadow, depth prepass, velocity, normal, colour and outline
  passes all pick the index buffer through one helper (`lodIndices`), so they use the same per-draw level.
- Tests: `systems_test` 144 passed, 0 failed. It covers the level thresholds, and a 40 × 40 grid simplified with the
  border locked (index count drops, indices are in range, the count is a multiple of 3, and the boundary edges are
  unchanged). New target `lod_gpu_test` loads a real model on the headless Vulkan device, builds levels, and checks the
  counts. It found a case where the simplifier stalls at the error limit, which is now handled by dropping that level.
  Editor self-test 587 passed, 0 failed.
- Limits: no LOD cross-fade, so the switch is a visible pop. Prefab-level LOD is not stored on the prefab; it is set
  on each scene instance. Graph (custom) materials use the same index pick. Not checked visually on a GPU.

## Physics check (batch 4, same day)

- Physics interpolation: already in the tree and on by default (`PhysicsWorld::setInterpolation`). Positions and
  rotations blend between the last two fixed steps by the leftover time. `systems_test` checks both the start pose
  and the halfway blend.
- Ray casts: `PhysicsWorld::raycast` is backed by Jolt and exposed to Lua as `ukiyo.raycast`. The "without Box2D or
  Jolt" variant from the release notes is not built, because Ukiyo always ships Jolt.
- Safe body changes in contact callbacks: done earlier (item 12, deferred changes).
- 2D sensors: skipped, Ukiyo is 3D only.

## Terrain, items 18 to 21 (in progress)

- Design: `research/terrain_design.md`. Decisions made 2026-10-16: PBR layers with the toon lighting ramp, static
  models only for foliage, erosion as an editor bake, and several 1025 by 1025 tiles per scene. The user chose the
  small target of about 2 by 2 tiles.
- Step 1, partly done: `src/engine/terrain.{h,cpp}` holds the heightmap, chunk meshes that share their edges, and the
  raw 16-bit file. Physics uses one Jolt mesh shape per chunk, because Jolt's height field needs a power-of-two sample
  count. `systems_test` has 13 terrain checks, all passing (161 in total).
- Not done in step 1: the `terrain` scene key, editor import, and drawing the chunks.
