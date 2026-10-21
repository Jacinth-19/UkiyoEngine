# What Ukiyo does not have (yet)

An audit of `src/` and `editor/` against what a team needs to *ship* a game with this engine, written by reading the
tree rather than the README. Every claim below is tied to a file, a symbol or a count that can be checked in a minute.
Where something is missing, the text says what is there instead — an absence is only interesting next to the shape it
leaves.

**Commit audited:** `13dde59b` (PR #3 head) — 71 043 lines across `src/`, `editor/`, `tests/`, `tools/`, `shaders/`.
**Shape of the code:** 13 370 of those lines are `editor/src/main_window.cpp`; the engine proper is 1.3 MB of `src/`,
the editor 21 MB (mostly fonts and brand art).

---

## 1. The one-paragraph verdict

Ukiyo is a **renderer and an animation tool with a game loop attached**, not yet a game engine. Its strongest assets are
unusual and real: a two-backend RHI, a deep NPR/toon pipeline (10 tonemappers, 15 stylize looks, TAA, SSR, SSAO,
multi-bounce AO, contact shadows, traced bounce GI, virtual geometry, terrain, grass), motion matching with foot locking,
an animation state machine with blend trees, a shader graph, a Unity-grade input system, and CI that runs 13 test
binaries on two platforms. What is missing is **everything that turns rendered frames into a shipped product**: a way to
build one, a way to draw a HUD, a scene graph with more than one rotation axis, a character controller, save games, an
import pipeline, and the workflow scaffolding (profiling, hot reload, autosave, localisation) a team of more than one
person needs.

None of that is architectural rot. It is scope that has not been taken on yet, and most of it has a clear home in the
existing code.

---

## 2. Scorecard

Ratings are judgement, not measurement: 5 = production-ready, 3 = works with known limits, 1 = minimal or absent.

| Area | Score | What exists | The hole that matters |
| --- | --- | --- | --- |
| NPR / toon rendering | **4** | mtoon/VRM, outlines, 10 tonemaps, stylize, LUT, SSR/SSAO/GI/contact shadows, bloom, TAA | Single light; no IBL or lightmaps (reflections are a sky gradient); no compute; no GPU timings |
| RHI | **3** | Vulkan + OpenGL behind one interface, MRT, mip chains, two backends in CI | No compute, indirect, storage buffers, queries, cube/array textures, BC/ASTC |
| Terrain / grass / foliage | **4** | Heightmap tiles, 8 layers, brushes, erosion, foliage density, grass bands, per-chunk trimesh collision | No streaming or world partition; GI reads only the nearest tile |
| Virtual geometry | **3** | 128-triangle cluster tree, screen-space cut, disk cache | CPU cut + per-cluster draws: no compute/indirect path (the doc says so) |
| Animation | **4** | State machine, blend trees, timeline/curve editor, motion matching, 2-bone + FABRIK IK, face SDF | No root motion (discarded by design), no bone masks or additive layers, no runtime retarget UI |
| Physics | **2** | Jolt; static box/mesh, dynamic sphere/box, one raycast, contact events, pair filter, merged trimesh world | No capsule/character controller, no joints, no triggers, no layer masks, no shape casts, no heightfield |
| Scene model | **2** | Flat instance list, prefabs + variants + overrides, empty objects | Position + yaw only, no scale/pitch/roll, no parenting, one camera, one sun |
| Audio | **2** | miniaudio; load/play/stop/volume/loop/spatial/listener, stereo pump | No buses, filters, reverb, snapshots, voice limit, streaming music |
| Networking | **2** | UDP, reliability layer, byte IO, client/server, stats | Everything above transport: no snapshots, RPC, prediction, lobby — though README claims snapshots |
| Scripting | **2** | Lua 5.4 sandbox, 23 `ukiyo.*` functions, entity handles, scene switching | One script per scene; no hot reload, debugger, script editor, modules, spawn/despawn or save API |
| Navigation | **3** | Recast/Detour, 8-stage bake, pathfinding, dtCrowd agents | Terrain is not baked into the navmesh; settings are hard-coded; no off-mesh links or area costs |
| Asset pipeline | **1** | Path strings, JSON references, rename rewrite, uses/used-by scan | No GUIDs, `.meta`, import settings, texture compression, asset database, reimport |
| Game UI / text | **0** | — | No font atlas, no 2D renderer, no HUD, no menus, no text anywhere in the runtime |
| Build / export | **0** | `Build/build.json` manifest, `ukiyo_pak` | No player executable, no platform packaging, no signing |
| Editor workflow | **3** | Hierarchy, inspector, multi-select, prefabs, terrain tools, shader graph, play mode, layouts | No prefab undo, autosave, profiler, build dialog, VCS, script editor, localisation |
| Testing / CI | **3** | 21 test sources, 13 in CI on Linux+Windows, Android NDK core, 5 figure renders | No GPU job, editor self-test (676 checks) not run, no sanitizers |
| Docs | **3** | 12 long-form docs with measured numbers | No renderer, physics, audio, network, scripting, build docs; README drifts |

---

## 3. Blockers: five things that stop a game shipping

### 3.1 There is no player build

The Build page writes a manifest and says so itself:

```cpp
// editor/src/main_window.cpp:13346
buildStatus->setText(QStringLiteral("Wrote Build/build.json with %1 scene(s). Packaging a playable executable is not included yet."));
```

`src/app/main.cpp` is a **demo viewer** (`--model`, `--shot`, `--video`), not a game runtime. `CMakeLists.txt` has no
player target. A game therefore runs only inside the Qt editor, with the editor's window, camera, docks and debug
overlays. `ukiyo_pak` and `Vfs` exist, so the packaging half of the problem is already solved; the missing half is a
`ukiyo_player` that boots `build.json`, reads input, and presents a frame.

**First step:** a `src/player/` target that links `ukiyo_core` + `ukiyo_systems`, mounts the pak, loads the first build
scene, and runs the same `World` the editor's Game view runs.

### 3.2 There is no way to draw a HUD

`grep -riE "font|sprite|hud|canvas" src/engine src/systems src/rhi shaders` returns nothing but comments. The renderer
draws 3D only: no font atlas, no textured quad batch, no 2D camera, no widget tree, no text shaping. A game cannot show a
score, a menu, a dialogue box or a health bar. This is the single hardest gap to work around, because everything else can
be faked with 3D tricks and a HUD cannot.

**First step:** a screen-space sprite batch in the RHI's existing fullscreen pipeline plus an SDF or bitmap font atlas;
then a minimal retained canvas (`Panel`, `Label`, `Image`, `Button`) fed by the same descriptor sets.

### 3.3 The scene is a flat list with one rotation axis

`SceneEntry` is the whole scene format:

```cpp
// editor/src/scene_file.h:21-33
struct SceneEntry {
    QString path;
    float position[3] = {0,0,0};
    float yawDeg = 0.0f;     // <- the only rotation
    ...
};
```

Consequences, all visible in the editor:

* **No scale.** The Scale gizmo is explicitly disabled:
  `"Scale is not available: model instances have no scale yet"` (`main_window.cpp:6336`).
* **No pitch or roll.** Characters can only face left/right; no slopes, no leaning props, no flight.
* **No parenting.** The Hierarchy is a `QTreeWidget` of top-level instances; the child rows are the *model file's own
  node tree*, generated read-only at `main_window.cpp:3369-3379`. There is no reparent, no group, no transform
  composition.
* **One camera and one light, both fixed rows**: `kItemMainCamera` and `"Directional Light"` (`main_window.cpp:3311,
  3314`), edited through project settings. A scene cannot add a second camera for a cutscene or a second light.

**First step:** `SceneEntry` gains `rotation[3]`, `scale[3]` and `parent`; the worker composes world matrices; the gizmo
gets a real scale mode; the hierarchy gets `InternalMove` drag-drop with cycle checks.

### 3.4 Physics cannot make a character

`src/systems/physics.h` exposes exactly four body constructors and one query:

```cpp
addStaticBox, addStaticMesh,
addDynamicSphere, addDynamicBox,
raycast
```

There is **no capsule**, no convex hull, no cylinder, no character controller, no trigger/sensor, no joint, no shape cast,
no overlap query. Internally Jolt is restricted to two object layers (`NON_MOVING`, `MOVING`; `physics.cpp:38-40`), so
the 32 layers in Project Settings are **names with no behaviour** — nothing reads them but the editor UI
(`project_settings.cpp` and the tests). The README's "Jolt rigid bodies and character controllers" is not true of this
tree: `grep -rniE "CharacterVirtual|JPH::Character"` returns nothing.

**First step:** `addCharacter(position, radius, halfHeight)` on `JPH::CharacterVirtual`, a `BodyComponent.kind` enum, and
layers/masks wired into `ObjectLayerPairFilter`.

### 3.5 There is no save system

`grep -rniE "savegame|save_game|playerprefs|persistent" src/` returns nothing. A game cannot persist progress, settings or
world state. Scene files are authoring data, not runtime saves.

**First step:** a small versioned binary/JSON blob written through `Vfs` (which already knows how to mount a pak), plus
`ukiyo.save`/`ukiyo.load` in Lua.

---

## 4. Engine: what is missing, area by area

### 4.1 Lighting

| | |
| --- | --- |
| Present | One directional sun (`RenderSettings::sunDir`, `sunIntensity`), 3 cascades (8/25/80 m) into a 2×2×2048 atlas, PCF; hemisphere ambient (`mtoon.frag:69-73`); two **hard-coded** coloured fill points in `renderer.cpp:657, 694-696`. |
| Missing | Point/spot lights as scene objects; shadows for anything but the sun; image-based lighting (`irradiance` = 0 hits, `reflection probe|light probe|spherical harmonic` = 0); light probes; lightmaps; emissive GI; per-light layers. |

The only reflection in the tree is a procedural trick, and its own comment says why: *"RealToon reflection (procedural
sky, no cubemap): the sky/ground gradient in the reflection direction, weighted by a Fresnel term and the gloss mask"*
(`shaders/toon_extras.glsl:36-45`). So a chrome prop reflects a two-colour gradient, not the room it stands in.

The two point lights are literal constants:

```cpp
f.pointPos[0] = glm::vec4(1.4f, 1.6f, 1.2f, 3.0f);
f.pointColor[0] = glm::vec4(0.9f, 0.35f, 0.5f, 0.0f) * 0.4f;
```

`mtoon.frag` loops `for (int i = 0; i < 4; ++i)` over a fixed array, so the shader capacity exists but nothing fills it
from the scene. A neon street or a row of lanterns cannot be lit.

### 4.2 RHI capability

`src/rhi/rhi.h` is a deliberately small surface, and the smallness is now the ceiling on the renderer:

| Missing | Consequence |
| --- | --- |
| `Format`: only RGBA8, RGBA8_SRGB, RGBA16F, D32F, Backbuffer | No BC1/BC7/ASTC: `ktx2`, `astc`, `bc[1-7]`, `dds` = 0 hits anywhere in `src/`, so every texture ships as uncompressed RGBA — 4–8× the VRAM and bandwidth |
| No cube maps, no texture arrays, no 3D textures | No reflection probes, no IBL, no volume textures |
| `BufferUsage`: Vertex, Index, Uniform only | No storage buffers, no GPU-side instance lists |
| No compute pipeline | GI, particles, vgeom, grass are all fragment/vertex passes; virtual geometry is cut on the CPU |
| No indirect draw / multi-draw | One draw per cluster. The renderer says so itself: *"no compute or indirect draw"* (`vgeom.h:8`), *"RHI has no compute stage, no storage buffers, and no indirect draw … a GPU-built indirect list"* (`grass.h:6-9`) |
| No timestamp or occlusion queries | No GPU profiler is possible, so frame cost cannot be attributed |
| Fixed `SetType` enum (Frame, Material, Skin, Post, ShaderMaterial, Motion, Particles) | Every new feature edits the RHI, its two backends and the binding audit |

On the credit side, the README is honest about this exact limit: *"the largest is GPU-driven virtual geometry, which needs
compute or multi-draw in the RHI"* (README line 346). This is the one gap the docs do not oversell.

Samplers are `Filter{Nearest, Linear}` × `Wrap{Repeat, Clamp}` (`rhi.h:43-44`), so no anisotropic filtering control
(there is no anisotropy setting anywhere; the only "anisotropic" in the tree is the hair highlight), no mirrored wrap, no
border colour and no comparison samplers. Also absent: MSAA — the only multisample state in `src/` is `VK_SAMPLE_COUNT_1_BIT` (`vk_device.cpp:151, 535-536`), so
antialiasing is SSAA/FXAA/TAA only; pipeline cache; bindless resources; and shader hot reload (built-in shaders are
compiled to SPIR-V at build time by `ukiyo_shaders`; only the shader graph compiles at runtime).

### 4.3 Performance infrastructure

* **No job system.** `grep -rl "std::thread|std::async" src editor` = 2 files (`systems/physics.cpp`,
  `hub/hub_window.cpp`); 6 files use `std::atomic`. There is no worker pool, so model loading, terrain build, GI
  precompute and mesh processing are serial. All off-thread editor work runs on two single-worker threads — the engine
  session (`QThread`) and asset previews, which are pinned to one thread with the reason stated in the code: *"the render
  backbone is not shared"* (`asset_preview.cpp:148-156`), so thumbnails render one at a time.
* **No streaming.** No async asset loading, no texture mip streaming (it is still on the Godot-4.8 shortlist), no
  world partition. A scene is loaded whole; terrain is one tile per instance.
* **No occlusion culling.** Culling is a frustum test plus a distance cut (`RenderSettings::frustumCull`, `renderer.h:31`,
  and `cull.h`); every `occlusion` hit in `shaders/` is an ambient-occlusion term (SSAO, terrain, the AO target) or the
  contact-shadow march. Nothing tests whether a draw is hidden behind another. Interiors cannot hide their neighbours.
* **No memory budget or asset LRU**: every instance owns its own `Model`
  (`std::unique_ptr<Model> model` in `engine_session.h:273`), so a prop placed twenty times uploads its geometry and
  textures twenty times.

### 4.4 Physics

Beyond the character controller above: no collision matrix, no friction/restitution/material per body, no CCD, no
continuous shape casts, no triggers (contacts are reported after the fact), no joints or ragdolls, no vehicles.

Collision *geometry* does exist, but it is built the expensive way: at "Play Systems" every static model is merged into
one triangle mesh and every terrain chunk is converted into its own trimesh collider
(`editor/src/engine_session.cpp:1222-1244`). There is no Jolt heightfield, so a 2×2 km terrain becomes hundreds of
thousands of triangles in the physics world before the first frame, and it is rebuilt on every play. Bodies are also not
serialisable as arbitrary shapes — only sphere/box, radius/half-extents/mass.

### 4.5 Audio

`src/systems/audio.h` is 52 lines: load, play, stop, volume, loop, spatial flag, listener, master volume, `pump()`.
Missing: mix buses and sends, filters, reverb, compression, ducking, snapshots, voice limiting/priorities, streaming for
long music, per-sound attenuation curves, Doppler, device change handling. Everything plays at once with one master gain.

### 4.6 Networking

The transport is solid and tested (sequence/ack bitfield, reliable ordered channel, fragmentation at the datagram level),
but **nothing above it exists**. There is no snapshot/delta encoding, no interpolation buffer, no prediction/reconciliation,
no RPC registry, no interest management, no lobby/matchmaking, no NAT traversal, no encryption or authentication.

This matters twice because the README advertises *"networking over UDP with snapshots and interpolation"*. The only
interpolation in the tree is `PhysicsWorld::setInterpolation`, which blends two fixed steps for rendering
(`physics.cpp:167-315`) — nothing to do with the network. The docs overstate the engine here.

### 4.7 Scripting

Lua 5.4 with a strict sandbox (base/string/table/math/utf8/coroutine) and **23 functions**:

```
log time actionDown joinPlayer playerCount leavePlayer playerActionDown playerActionValue
playerActionVector2 getPosition setPosition moveTo impulse setParam playSound spawnEffect
raycast loadScene holdLoad releaseLoad preloadModel setContactFilter Entity
```

There is also a hard limit in play mode: **only one instance in a scene may run a script**. `engine_session.cpp:1200-1214`
counts script components and refuses to start with `"Only one instance can run a script in play mode. Disable the
others."` So a scene cannot have a player script and an enemy script at the same time.

What a gameplay programmer will miss: hot reload, a debugger/console, `require`/modules, signals or an event bus, timers,
coroutine scheduling on `dt`, spawning a *visible* model at runtime (`Entity(name)` only names a simulation entity;
`spawnEffect` spawns particles), destroying entities, reading/writing components generically, save/load, and any in-editor
script editing or completion. There is no C# or visual scripting alternative.

### 4.8 Animation

Mostly strong, but with two real limits: **root motion is discarded by design** (`motion_match.h:322`; `model.cpp:411`
leaves a placeholder for "future root-motion lean"), so a cutscene or a root-driven attack cannot be authored without
code; and the player has exactly **two layers**, used as a crossfade pair (`anim.h:178-211`): whole-clip blending, no bone
masks, no additive layers, no per-layer time scaling exposed to the state machine. Retargeting is
programmatic (`rig.h`), not an editor workflow.

### 4.9 Terrain, grass, vgeom

These are the best-developed parts of the engine. Their gaps are the RHI's: vgeom cuts clusters on the CPU and issues one
draw per surviving cluster; grass is described as "indirect-friendly" but draws instanced, not indirect; terrain chunks
are uploaded once and never streamed; the real-time GI estimator sees only the *nearest* terrain heightfield
(`engine_session.cpp:988-1000` picks one), and it skips rotated tiles because it assumes an axis-aligned height function.

### 4.10 Navigation

Recast + Detour with an eight-stage bake, path queries and `dtCrowd` agents (`navigation.h:20-58`). Two limits remain:

* **One setting set.** The bake always uses `NavMesh::Settings()` defaults (`engine_session.cpp:1107`), and those defaults
  (0.3 m radius, 1.8 m height, 45° slope, 0.4 m climb, `navigation.h:20-24`) are not exposed anywhere in the editor: the
  Navigation section has only "Max speed (m/s)" for the selected agent (`main_window.cpp:3984-3993`).
* **No off-mesh links, area costs, multiple navmeshes, or incremental rebuild.** Play re-bakes only when `navBaked_` is
  false (`engine_session.cpp:1215`), so editing the level mid-session leaves the crowd on the old mesh.

*Fixed since this audit:* the bake now takes terrain tiles as well as models. `bakeNavMesh` used to call
`worldTriangles` on each instance's *model* only, so a scene whose ground was a tile baked the floor underneath it and
an agent on sculpted ground walked off the mesh. `appendTerrainTriangles` (`src/engine/terrain.cpp:77`) appends every
chunk through the same `buildChunk` the collider uses, moved to the tile's origin, and the editor self-test bakes a
scene of hills over a flat floor and checks the input reaches above 5 m. See [TERRAIN.md](TERRAIN.md) §8.

### 4.11 Platforms and devices

The input system is the best-engineered part of `src/`, and it supports exactly three device kinds:

```cpp
// src/input/input.h:64
enum class DeviceKind { Keyboard, Mouse, Gamepad };
```

Importing an action map that names anything else fails with an explicit message — `"is not supported (no touch or XR
devices)"` (`src/input/input_asset.cpp:169`). So: **no touch, no pen/stylus, no XR, no gyro**, and haptics are a comment
only (`input_users.h:135` mentions a rumble but no API sets one).

Platform-wise, the tree builds desktop only. `ANDROID` short-circuits CMake to two utility tests (§6), there is no
`ANativeActivity` entry point, no APK target, and no iOS target. That makes the Android line in CI a compile check rather
than a platform the engine runs on.

---

## 5. Editor: what is missing

### 5.1 Workflow scaffolding

| Missing | Evidence / what happens now |
| --- | --- |
| **Undo in prefab mode** | `undoEdit()` returns early: `"Undo is not available in prefab mode."` (`main_window.cpp:6113-6117`) |
| **Autosave / crash recovery** | `grep -riE "autosave|auto-save|recover|crash"` = 0. A crash loses every unsaved change |
| **Undo model** | 100 **whole-scene JSON snapshots** (`kUndoLimit = 100`, `main_window.cpp:6049`), each a full `sceneDocument()`; undo re-applies the document and rebuilds the scene. Memory scales with scene size × 100, and there are no per-field transactions outside the 800 ms key coalescing |
| **Profiler** | FPS/draw-calls/triangles text and a "Benchmark Scene" button only (`main_window.cpp:4978-4980`, `5079`). No timeline, no per-pass breakdown, and no GPU timing is possible through the RHI |
| **Build dialog** | The Build page writes `build.json` and says packaging is not included (§3.1) |
| **Version control** | No git status/diff/per-file history anywhere |
| **Script editing** | No code editor, no syntax highlighting, no completion, no breakpoints; `QPlainTextEdit` appears only in the shader graph's generated-source view |
| **Asset import settings** | No per-asset settings UI because there is no import step (§5.2) |
| **Localisation** | `QTranslator` = 0. Every editor string is an English literal; the brand typeface supports Japanese, but the UI does not |
| **Command palette / quick open** | Not present; navigation is the hierarchy, the browser and menus |
| **Docking layout import/export** | Custom layout names live in `editor::prefs` (QSettings) as one newline-separated string (`main_window.cpp:5759-5762`); they cannot be exported, shared or stored per project |
| **Multi-window** | Only Shader Graph opens its own window |

### 5.2 Assets

The asset model is **paths in JSON, scanned on demand**:

* `assetops::usesOf` parses the file and collects every string that resolves to a file (`asset_ops.cpp:231`).
* `assetops::usedBy` parses **every reference file in the project** on each call (`asset_ops.cpp:251-268`) — it runs when
  the browser's details pane shows a selection.
* Renaming rewrites those strings across the project.

There are no GUIDs, no `.meta` sidecars (`\.meta\b` = 0), no import settings, no import cache, no texture compression, no
reimport, and no dependency database. Consequences: a rename is a global text rewrite; a moved file outside the editor
breaks references silently; every texture ships at full RGBA size; and there is no place to hang per-asset knobs (wrap,
filter, compression, pivot, animation compression).

### 5.3 Play is two buttons, and only one of them runs the game

Play (Space) sets `playing_`, starts the frame timer and maximises the Game view (`main_window.cpp:4753-4769`). It does
**not** start the simulation: `World` is only created by `EngineSession::startPlay`, which the UI calls from the
inspector's Systems section — a second button labelled "Play Systems" / "Stop Systems" (`main_window.cpp:3906`,
`4313-4317`). To test a scene a designer presses Play, finds the Systems section, then presses Play Systems; stopping is
two steps in the other order. Unity and Godot both have one button.

The good half: play mode *does* snapshot and restore the scene. `stopPlay` calls `restoreEditorScene` and re-applies every
saved placement (`engine_session.cpp:1348-1371`), so playing never permanently moves an object.

### 5.4 Editor code health

`main_window.cpp` is **13 370 lines** — 19 % of the whole codebase in one translation unit, built at `-O0` on Linux
because `-O3` runs out of memory on the build machine (`CMakeLists.txt:522-528`). Every panel, menu, dialog, self-test
and browser behaviour lives there. This does not break anything today; it makes every change riskier and code review
harder, and it is why feature work keeps accreting in one file.

---

## 6. Quality: tests, CI, and the things that are not checked

What CI does today (`.github/workflows/build.yml`):

* **Linux:** builds everything, runs 13 test binaries, renders five documentation figures (trail, contact shadows, alpha
  mips, browser, motion demo), checks the real-time GI estimator against a converged reference, captures hub/splash
  screenshots and commits them.
* **Windows:** MSVC + Ninja, the same 13 tests, hub self-test.
* **Android:** NDK builds for arm64-v8a and x86_64, with ELF/Bionic ABI checks. Worth reading the CMake: under `ANDROID`
  the file builds `search_test` + `browser_zoom_test` and `return()`s (`CMakeLists.txt:35-44`). No renderer, no RHI, no
  Jolt, no editor — the Android job proves two utility files compile for the ABI, nothing more.

What is **not** checked:

| Gap | Detail |
| --- | --- |
| **No GPU test job** | `endfield_gpu_test`, `lod_gpu_test`, `particles_gpu_test`, `water_render`, `rig_smoke`, `endfield_scene_test`, `endfield_shader_test` are built by CMake but never run in CI. There is no `xvfb`/lavapipe job, although the editor's engine session already defaults to lavapipe (`engine_session.cpp:38-41`) |
| **The editor self-test never runs** | 676 `check()` calls in `runSelfTest()` cover undo, prefabs, terrain, inspector, browser, play, graph and more — and no CI job executes `ukiyo_editor --selftest` |
| **No sanitizers** | No ASan/UBSan/TSan job, no valgrind, no fuzzing of the glTF/VRM, `.ukscene`, `.ukprefab`, `.ukanim` or network parsers |
| **No static analysis** | No clang-tidy, no cppcheck, no `-Werror` (warnings are allowed to accumulate) |
| **No coverage** | No gcov/lcov, so "which of the 71 k lines are exercised" is unknown |
| **No perf regression** | The benchmark is manual; frame times are not tracked between commits |
| **No macOS CI** | README line 345: "the editor and hub run on Windows, Linux and macOS" — nothing builds macOS |
| **Android is built, never run** | No emulator job, no APK target, no `ANativeActivity` entry point, no input touch path (§4.10). Building is not running |

---

## 7. Repository hygiene

Small, but the kind of thing a new hire notices first:

* **No `LICENSE`.** `third_party/` has licences; the project itself does not, which makes the repository legally
  ambiguous to use or contribute to.
* **No version.** `project(UkiyoEngine LANGUAGES C CXX)` has no `VERSION`, no changelog, no tags (`git tag` is empty), no
  release artifacts.
* **No contribution scaffolding:** no `CONTRIBUTING.md`, no `CODE_OF_CONDUCT.md`, no issue/PR templates, no
  `SECURITY.md`, no `.clang-format`, no `.editorconfig`.
* **Stray files at the root, all tracked:** `a.out` (a 16 KB compiled binary), `test.cpp`, `frag.spv`,
  `no_water.png`, `water_above.png`, `water_under.png`, and a `renders/` directory of intermediate frames.
* **Docs drift.** Examples found while auditing:
  * README: "Jolt rigid bodies **and character controllers**" — no character controller exists.
  * README: "networking over UDP **with snapshots and interpolation**" — transport only.
  * README line 278: "Windows … runs the **seven** portable tests" — it runs 13.
* **Missing docs** for whole subsystems: renderer/RHI, lighting, materials, physics, audio, networking, scripting, scene
  and prefab formats, asset pipeline, build.

---

## 8. What is fine (so the list above is not misread)

These are commonly assumed missing and are not: undo/redo of scene edits (with terrain records), prefabs with variants,
overrides and nested prefab references, multi-select editing that applies a change to every selected object (`main_window.cpp:9691-9713` checks it), an
animation state machine with blend trees and a live test pane, a timeline with draggable curve keys, two-bone and FABRIK
IK with foot locking, motion matching, a shader graph that compiles to SPIR-V at runtime, material thumbnails rendered
live, terrain sculpt/paint/erosion, foliage density painting, a Unity-compatible input asset with rebinding and
multi-player pairing, fuzzy + glob asset search, dockable layouts, shortcut rebinding, and a project hub with templates.

---

## 9. Suggested order of work

Effort is rough, for one engineer who knows the codebase, including tests and docs. The ordering is chosen so each phase
unblocks the next one rather than so each phase is individually satisfying.

### Phase 0 — make what exists shippable (≈ 6–8 weeks)

1. **Player build** (`ukiyo_player`, pak mount, `build.json`, window + input loop). *4–6 weeks.* Unblocks everything:
   without it, no amount of engine work reaches a player.
2. **CI: software-GPU job** (`xvfb-run` + lavapipe + `ukiyo_editor --selftest` + the existing GPU tests). *1 week.*
   Immediately puts 676 editor checks and the renderer tests under CI, for almost no engine work.
3. **Sanitizer job** (ASan+UBSan on the 13 tests). *2–3 days.* Finds the memory bugs the tests cannot.

### Phase 1 — make it a game engine (≈ 10–14 weeks)

4. **Scene graph and transforms** (rotation xyz, scale, parent, multiple cameras and lights, serialisation, gizmo,
   hierarchy drag-drop, prefab-compatible). *4–6 weeks.* Everything below assumes it.
5. **Game UI and text** (sprite batch, font atlas, canvas, input routing). *3–5 weeks.*
6. **Physics for characters** (capsule, `CharacterVirtual`, layers/masks, triggers, joints, shape casts). *2–3 weeks.*
7. **Save/load** (versioned blob through `Vfs`, Lua API). *1–2 weeks.*

### Phase 2 — production workflow (≈ 12–16 weeks)

8. **Asset pipeline** (GUID/`.meta`, import settings, BC7/ASTC compression, dependency DB, reimport, async import).
   *5–7 weeks.* The largest single quality-of-life change for a team.
9. **Editor production pass** (undo in prefab mode and per-field transactions, autosave/recovery, profiler with GPU
   timings once the RHI has queries, VCS status, script editor with highlighting, command palette). *4–6 weeks.*
10. **Audio mixer** (buses, effects, snapshots, voice limits, streaming music). *2 weeks.*
11. **Localisation** (Qt `QTranslator` + project string tables; Japanese first, since the brand already supports it).
    *1–2 weeks.*

### Phase 3 — depth (≈ 12–18 weeks)

12. **RHI 2** (compute, indirect, storage buffers, timestamps, cube/array textures, BC/ASTC, pipeline cache). *4–6
    weeks.* Unlocks GPU vgeom, GPU particles, occlusion culling, FFT water and any real profiler.
13. **Lighting** (multiple lights with shadows, IBL/probes, lightmaps, light layers). *3–4 weeks.*
14. **Streaming and jobs** (worker pool, async load, mip streaming, terrain/world partition). *6–8 weeks.*
15. **Netcode layer** (snapshots, interpolation, prediction, RPC, lobby). *4–6 weeks* — or delete the README claim and
    keep it as a transport.

### Phase 4 — reach

16. macOS CI and a signed macOS build; Android application target (the NDK core job is already a foothold); touch input.
17. Repo hygiene: licence, versioning, changelog, templates, `.clang-format`, root cleanup, doc drift sweep.

---

## 10. Evidence index

| Claim | Where |
| --- | --- |
| Build page says packaging is not included | `editor/src/main_window.cpp:13346` |
| Scene transform is position + yaw | `editor/src/scene_file.h:21-33` |
| Scale gizmo disabled | `editor/src/main_window.cpp:6334-6336` |
| One camera, one light as fixed hierarchy rows | `editor/src/main_window.cpp:3311, 3314` |
| Hierarchy children are the model's nodes | `editor/src/main_window.cpp:3369-3379` |
| Two hard-coded point lights | `src/engine/renderer.cpp:657, 693-696` |
| Shader loops over 4 point lights | `shaders/mtoon.frag:78-88` |
| 3 cascades, fixed splits | `src/engine/renderer.cpp:31, 650-652` |
| No IBL: `irradiance` 0 hits, no probes/harmonics | `grep -rniE "irradiance" src shaders` = 0; `grep -rniE "reflection probe\|light ?probe\|spherical harmonic" src shaders` = 0; `toon_extras.glsl:36-45` |
| RHI formats/usages/ops | `src/rhi/rhi.h:28-44, 156-193` |
| No compute/indirect draw in the RHI | `grep -riE "dispatch\|compute\|indirect" src/rhi` = 0; stated in `vgeom.h:8`, `grass.h:5-9` |
| Physics API surface | `src/systems/physics.h:36-88` |
| Only box/sphere shapes | `src/systems/physics.cpp:20-22, 227-270` |
| Two physics object layers | `src/systems/physics.cpp:38-40` |
| Layers/tags unused by the engine | `src/engine/project_settings.cpp` + tests only |
| No character controller | `grep -rniE "CharacterVirtual|JPH::Character" src` = 0 |
| Audio API | `src/systems/audio.h:27-45` |
| Network transport only | `src/systems/network.h:201-271` |
| Physics interpolation ≠ netcode | `src/systems/physics.cpp:167-315` |
| 23 Lua functions | `src/systems/world.cpp:502-524` |
| Root motion discarded | `src/engine/motion_match.h:322`; `src/engine/model.cpp:411` |
| No job system | `grep -rl "std::thread\|std::async" src editor` = 2 files; one preview thread, `asset_preview.cpp:148-156` |
| Per-instance Model ownership | `editor/src/engine_session.h:272-289` |
| Uses/used-by scans | `editor/src/asset_ops.cpp:231-268` |
| 100 whole-scene undo snapshots | `editor/src/main_window.cpp:6049, 6104-6106` |
| Undo disabled in prefab mode | `editor/src/main_window.cpp:6113-6117` |
| Editor self-test size | `grep -c "check(" editor/src/main_window.cpp` = 676 |
| GPU tests exist but are not in CI | `CMakeLists.txt:404-450`; `.github/workflows/build.yml` test lists |
| Lavapipe already defaulted | `editor/src/engine_session.cpp:38-41` |
| No license/version/tags | root listing; `project(UkiyoEngine …)`; `git tag` empty |
| README overstatements | `README.md` §Systems vs. the greps above; "seven portable tests" vs. 13 |
