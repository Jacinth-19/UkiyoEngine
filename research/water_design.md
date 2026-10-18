# Water (item 16) design

Status: design only, for check-in. No code written. Items 16 and 17 are the Hard water pair. Own implementation
throughout. Gerstner waves are a public formula (Tessendorf / GPU Gems ch. 1), implemented from the maths only. No
third-party water shader is copied.

## Scope proposed for v1

In: Gerstner waves (sum of 4 waves), Fresnel blend of refracted scene and sky reflection, Beer-style absorption with
depth, edge and crest foam, sun shadow on the surface, underwater post effect (tint and fog when the camera is below).
Out of v1: planar reflections (a second scene pass), screen-space reflections, caustics on geometry, exclusion volumes,
velocity output for TAA, water in the standalone player (it has no scene loading yet).

## Data

- `WaterVolume` in `src/engine/water.h`: centre (x, z), half extent (x, z), level (y), `WaveParams` (4 waves: direction,
  wavelength, amplitude, speed, steepness), colours (shallow, deep), absorption, foam width, refraction strength.
- Authored in the scene file as a `water` entry. The editor gets an Inspector section. Zero or one volume per scene in v1.
- The wave height and normal are a CPU function `waterHeight(x, z, t)` and `waterNormal(x, z, t)` that matches the shader
  exactly. Buoyancy (item 17) reads the same function, so the physics and the picture agree.

## Mesh

- A grid, 128 × 128 quads at most, sized to the volume. Vertices are displaced in `water.vert` with the Gerstner sum.
  The grid is static. Only the displacement changes with time.

## Render order

1. Depth prepass and opaque (as now).
2. Copy the scene colour (`hdr_`) to a half-size refraction texture, only when a volume is visible.
3. Water pass (`water.frag`): depth-tested against the opaque depth, no depth write. It samples the refraction texture
   and the depth buffer. Foam is where the scene depth under the surface is less than `foamWidth`.
4. Blended materials, then the underwater effect when the camera is below `level`.

## Risks to check first

- Sampling `depth_` while it is the depth target of the same pass. Vulkan needs a read-only depth layout for this.
  If the RHI cannot do it, copy depth to a second texture (one more copy pass).
- GL backend parity for the same sampling.
- Frame cost without a GPU. Only shader source counts can be checked here, as with the stylize work.

## Item 17 (buoyancy) preview

- For each dynamic body inside a volume, take 4 sample points on its bounds. At each point with `p.y < waterHeight`,
  call Jolt's `BodyInterface::ApplyBuoyancyImpulse` with the surface position, the wave normal, and a buoyancy factor
  from submerged depth. Drag comes from the same call. Only dynamic bodies are affected.

## Questions for the check-in

1. Is the v1 scope above right? (Planar or SSR reflections and caustics are the first additions if wanted.)
2. Water shape: an authored rectangle (proposed) or an infinite plane?
3. Exclusion volumes ("keep water out of hulls"): defer (proposed), or do them in v1?

## Implementation status (items 16 and 17)

Built: the CPU wave model (`src/engine/water.{h,cpp}`), the scene `water` object, the render passes, the Inspector
section, and buoyancy in `PhysicsWorld`. Checks: `systems_test` 140 of 140 (wave and buoyancy checks), the editor
self-test section 15 (scene round trip, clamping), and `water_render` (Vulkan, headless, five checks: the frames render, have detail, and differ under the surface and without water).

Deviations from this design, and why:

- **Depth is not sampled in the water pass.** The copy pass (`shaders/water_copy.frag`) reads the opaque depth and
  writes view distance into the alpha of the refraction target. The water pass reads only that target. So the depth
  buffer is never read while it is being written, and no extra depth copy is needed.
- **Refraction target is half size, RGBA16F.** Its alpha holds view distance in half precision. That is coarse past
  about 50 m, so edge foam is soft at long range. A 32-bit target would fix it, but the RHI has no RGBA32F format.
- **Underwater effect is a blended fog pass after the blended draws.** It takes its depth from the copy, so blended
  surfaces get the fog of the scene behind them, not their own depth.
- **Buoyancy is one Jolt call per body, not four.** Jolt takes one surface plane and computes the submerged volume of
  the whole shape. Four calls would count that volume four times. The four footprint samples only set the plane:
  mean wave height and mean wave normal. The buoyancy factor is water density over body density, with body density
  = mass / shape volume, clamped to [0, 4]. Water density is `density` in the volume (1000 kg/m^3 default).
- **Wave clock.** The water clock is the render time. The editor passes it to the play world each frame, so buoyancy and
  the drawn waves use the same phase.
- **Editor.** A `water` object in the `.ukscene` file. The scene page has a Water section with: volume on/off, level,
  half X, half Z, foam width and absorption. Waves, colours, density and refraction are file-only in v1. Water edits
  go through the document (saved, reopened, reset when a scene without water opens), but are not separate undo steps.
- **Scene switching does not change the water.** A switched scene keeps the editor scene's volume (not done in v1).
- **Standalone player:** no water (it has no scene loading, as before).

Known limits, found in the first render check:

- The editor's infinite grid sits at y = 0, the water level in the test. Where waves dip below it, the grid showed
  through. Fixed in a water scene: the grid tests depth but does not write it (`pGridWater_`), so the water drawn after
  it wins. Scenes without water keep the grid's depth write (`pGrid_`). The first version wrote no depth in every
  scene, and that changed the Impasto stylize look in the editor self-test (it became indistinguishable from Off). The
  self-test is 587 passed, 0 failed again. Depth-based effects still see grid pixels with no depth in water scenes.
- Foam was too heavy in the first render. Crest foam is now limited to the top 18% of the height range.
- Not checked on a real GPU. The render check ran on llvmpipe (software Vulkan), which confirms the passes run and
  that the output is not blank, not the look.

Files: `src/engine/water.{h,cpp}`, `shaders/water.vert`, `shaders/water.frag`, `shaders/water_copy.frag`,
`shaders/underwater.frag`, `src/engine/renderer.{h,cpp}` (the `setWater` path and the split scene pass),
`src/engine/gpu_types.h` and `shaders/frame.glsl` (the water fields at the end of the frame block),
`src/systems/physics.{h,cpp}` (`setWater`, `setWaterTime`, buoyancy), `editor/src/scene_file.{h,cpp}`,
`editor/src/engine_session.{h,cpp}`, `editor/src/render_worker.h`, `editor/src/main_window.{h,cpp}`,
`tests/systems_test.cpp`, `tests/water_render.cpp`.
