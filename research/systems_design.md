# Game systems: physics, navigation, audio, animation state, scripting, particles

Status: engine-level first version, built and tested headless (`build/systems_test`, 52 checks, all pass).
Editor integration and the GPU draw pass for particles are not built yet.

## Where the code is

| Area | Files | Library |
|---|---|---|
| Physics | `src/systems/physics.{h,cpp}` | Jolt Physics (vendored) |
| Navigation | `src/systems/navigation.{h,cpp}` | Recast, Detour, DetourCrowd (vendored) |
| Audio | `src/systems/audio.{h,cpp}` | miniaudio (vendored) |
| Animation state machine | `src/systems/anim_state.{h,cpp}` | none. Calls `AnimPlayer::play` through a callback |
| Scripting | `src/systems/script.{h,cpp}` | Lua 5.4 (vendored), sandboxed |
| Particles | `src/systems/vfx.{h,cpp}` | none. CPU simulation with an instance list |
| World (ties them together) | `src/systems/world.{h,cpp}` | none |
| Tests | `tests/systems_test.cpp` (target `systems_test`) | none |

## Decisions

- **Physics:** Jolt with two object layers, non-moving (static) and moving. Static bodies do not collide with each other.
- **Navigation:** the navmesh is baked from triangles the caller gives it. Crowd agents steer along it
  and avoid each other. Detour has no "arrived" state, so arrival means a valid target within 0.6 m.
- **Audio:** miniaudio, usable without an output device for tests. A sound with a position is
  attenuated and panned against the listener. The test checks that a sound 20 m away is much quieter than one at 1 m.
- **Scripting:** Lua 5.4 with only base, string, table, math, utf8, and coroutine open. `io`, `os`,
  `package`, `dofile`, and `loadfile` are not available to scripts.
- **Animation state machine:** parameters are floats, bools, or triggers. A trigger stays set until a
  transition consumes it, so a request made during a minimum dwell time is not lost.
- **Particles:** CPU simulation. Each emitter exposes its live particles as `ParticleInstance`
  records (xyz and size, rgba), laid out for an instance buffer.

## Particle GPU pass

- `src/engine/particle_renderer.{h,cpp}`: `ParticleRenderer` draws one instanced call per frame.
  The instance block is a uniform buffer (binding 11, `SetType::Particles`) holding up to 1024 instances (32 KB).
- `shaders/particle.vert/.frag`: camera-facing quads built from `gl_VertexID` and `gl_InstanceID`.
  No vertex buffer. Soft round falloff. Depth-tested, no depth write. Alpha or additive blend.
- RHI: new `Device::drawInstanced(vertexCount, instanceCount)`, in GL and Vulkan. `SetType::Particles` is appended to the enum.
- `INSTANCE_ID` macro in `shaders/common.glsl`.
- Not done: back-to-front sorting, more than 1024 particles per draw (extra particles are dropped), and
  one draw per pass (the block is uploaded once per call).
- Test: `particles_gpu_test` renders three coloured particles and checks pixel positions, the gap,
  and the cap. Passes 14/14 on OpenGL (llvmpipe under xvfb) and on Vulkan (lavapipe).

## Editor

Scene root in the Inspector, below the scene summary:
- **Play**: "Play Systems" and "Stop Systems". Play runs on the render timer, so instances move in the
  viewport. Stop puts every instance back where it was before Play. This is separate from the timeline
  Play, which drives clips.
- **Bake Navmesh**: builds the navmesh from the geometry of every instance. Play also bakes when an agent
  needs it. A scene with no walkable surface (a character alone) reports "No walkable surface found".
- **Instance**: picks the instance to edit. Then one foldout per system: Physics, Navigation, Audio,
  Animation, Script, Particles. Each has an Enabled checkbox.

Rules that apply in play:
- Instances that are not dynamic bodies become static colliders, so a falling body lands on a floor.
- Animation clips drive only the active instance's model. Other instances stay in their rest pose.
- Particles are drawn as an overlay after tone mapping. They are not exposed, graded, or depth-tested.
- One script per scene. Enabling a second script makes Play refuse with a message.
- Scripts address instances as `instance<N>`, where N is the instance index.
- Paths are stored relative to the project. They are absolute in memory.
- Play advances on the editor frame timer. The frame step is clamped to 0.1 s. Physics runs its own 1/60 s sub-steps inside that.

Not in this step: the Hierarchy does not list instances as rows yet (the Instance picker covers this),
and prefab-linked instances keep their components on the instance, not on the prefab.

## Script API (`ukiyo.*`)

`log(msg)`, `time()`, `getPosition(name)`, `setPosition(name, x, y, z)`, `moveTo(name, x, y, z)`,
`impulse(name, x, y, z)`, `setParam(name, param, value)`, `playSound(path [, entityName])`,
`spawnEffect(x, y, z [, count])`, `raycast(ox, oy, oz, dx, dy, dz, maxDist)`.
Scene loading (editor play only; the standalone `ukiyo` player does not load scenes yet):
`loadScene(path)` asks for a switch to a `.ukscene` file, `holdLoad()` returns a token that stops the
switch, `releaseLoad(token)` drops the hold, and `preloadModel(path)` reads a model ahead of time.
A script may define `onStart()`, `onUpdate(dt)`, and `onSceneLoaded()`. `onSceneLoaded` runs once the
scene has started, after `onStart`.
Value types: `Vector3(x, y, z)`, `Quaternion.lookRotation(forward [, up])`, and `Quaternion.angleAxis(degrees, axis)`.
Quaternion * Vector3 rotates, Quaternion * Quaternion composes, and Vector3 adds, subtracts, and scales. Fields
are read-only. A quaternion turns +Z to the direction it looks at, so `angleAxis(yaw, (0,1,0))` matches the
engine's yaw (`(sin yaw, 0, cos yaw)`).

## Entities, contacts and filters

- `ukiyo.Entity(name)` returns a handle to a named entity (created on first use). Fields: `name` (read-only),
  `position` (Vector3, writable), `yaw` (degrees, writable), `body` (physics handle, read-only). Methods:
  `e:impulse(x, y, z)`, `e:setParam(param, value)`, `e:moveTo(x, y, z)`. Handles compare equal by name.
  An unknown or read-only field is a Lua error.
- `onContact(ev)` runs after each physics step for every contact that began or ended. `ev.kind` is `"added"` or
  `"removed"`, `ev.a` and `ev.b` are entity names (`""` for a body that is not an entity), and `ev.point` and
  `ev.normal` are Vector3 (zero for removed contacts). Jolt raises contacts on its job threads, so they are queued
  and the script runs on the main thread. Body changes made from `onContact` are deferred to the start of the next
  step, because Jolt forbids them inside its callbacks. Persisting contacts are not reported.
- `ukiyo.setContactFilter(a, b, enabled)` turns collisions between two entities off (`enabled = false`) or on. It is
  a native pair filter, checked in Jolt's validate callback, so no Lua runs inside the solver. It blocks new contacts
  only: a pair that already touches separates when it moves apart.
- Not done: Doriax's `preSolve` filter returns a value from a Lua handler. The solver cannot call Lua (see above), so
  handlers that return values are not supported. Lua handler return values are ignored.

## Resource packs

`resources.pak` packs the files of a project folder into one file (`src/engine/vfs.{h,cpp}`). Build it with
`ukiyo_pak <out.pak> <project root> <folder>...`, for example `ukiyo_pak project/resources.pak project assets scripts`.
Format `UKPAK1`: a sorted index of relative paths with offsets and sizes, then uncompressed data. A mounted pack
serves reads of its files before disk does (`vfs::readBytes`). `vfs_cgltf.h` gives cgltf the same read hook, so
glTF and GLB files are parsed from the pack. Hooked: model and animation glTF loads, embedded and external model
textures, face SDF, graph-material textures, the LUT, and scripts. Not hooked: audio, shaders (engine-owned), and the
editor's scene and project files. No runtime entry point mounts the pack yet: `src/app/main.cpp` is unchanged.

## Physics interpolation

Dynamic bodies are drawn between their last two fixed steps (`PhysicsWorld::setInterpolation`, on by default).
`position()` and `rotation()` return the blended pose, so `ukiyo.getPosition` shows it too. `setPosition` jumps
with no blend. Physics itself is not affected.

## Scene switching during play

- A switch runs between frames: after the World ticks, before the frame draws. It waits while any hold is
  open. Holds belong to the World, so a switch drops them.
- The `.ukscene` reader is `editor/src/scene_file.{h,cpp}`. Open Scene and the switch both use it.
- Parse and every model read happen before anything changes. A missing model or a parse error leaves the
  running scene alone and reports the error in the Log.
- The first switch keeps the editor's scene in memory, so Stop puts it back without reloading any model.
  Later switches only replace the switched scene. Save is refused while a switched scene is in play,
  because writing it would put that scene into the open file.
- If the new scene fails to start (for example, two scripts), play stops and the editor's scene comes back.
- Preloads go to a cache of up to 8 models. A staged model is used once, by the next instance that loads
  that path. It is dropped if the file's modification time or the smooth-normals setting changed. The cache
  is released at shutdown.
- A preload is read on the render thread, so it stalls one frame. There is no background loading yet.
- Changes made to a switched scene during play are dropped at Stop.
- Opening or creating a scene ends play first, which puts the editor's scene back before the new one loads.

## Frame order (`World::tick`)

1. Script `onUpdate`
2. Physics step (fixed 1/60 s sub-steps)
3. Crowd update
4. Copy body and agent positions to entities
5. Animation state machines (clip changes go to `onClipChange`)
6. Particle update
7. Audio listener from the entity named `listener`

## Next, in the agreed order

User decision: full editor panels before prefab stages 3 to 5. Remaining order:

1. ~~VFX GPU draw pass~~ Done (see below).
2. ~~Editor panels~~ Done (see "Editor" below).
3. ~~Save and load of these components in the scene file~~ Done, as an optional `components` object per instance.
4. Prefab stages 3 to 5 (Inspector overrides, prefab edit mode, Apply All and Revert All).

## Not built yet

- **Animation state machine asset.** States and transitions are fixed (Idle, Walk, Run). Only the clip
  names and speed thresholds are editable.
- **Prefab-level components.** Components live on the scene instance. They do not yet live on a prefab, so
  they are not shared between instances of one prefab.
- **Prefab stages 3 to 5** (see `research/prefab_system_design.md`). The user asked for these after the six systems.

## Known limits

- Jolt is built without its Vulkan, DirectX, and Metal backends. Those are not needed by the engine.
