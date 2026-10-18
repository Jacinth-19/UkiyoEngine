# Terrain, foliage and brushes: design for check-in

Status: decisions made on 2026-10-16 (see "Decisions" below). Step 1 is built (see the status note under Build order). Later steps wait for the user's go-ahead. Items 18 to 21 of the Doriax v0.8 list are assumed to be this
batch (the Terrain and foliage section of `doriax_v08_features.md`). The user confirmed the mapping on 2026-10-16.

Our rule holds: Doriax is MIT, so we re-implement the ideas from the public descriptions, and we copy no code.

## What exists today

- No terrain code. `grep` finds no heightfield, no terrain component, no foliage, and no brush code.
- Models draw one call per placed object (`Renderer::render`). There is no GPU instancing for models. The particle
  renderer (`src/engine/particle_renderer.h`) does instanced quads, so it is the pattern to copy in our own code.
- Per-view culling exists for static model draws (`src/engine/cull.h`, item 9). The same test can cull chunks.
- Physics is Jolt (`src/systems/physics.cpp`). Jolt's height-field shape is in core Jolt, and we do not use it yet.
- The editor has an Inspector, a Hierarchy, and undo through `sceneDocument()` snapshots (`beginEdit`). Brushes need a
  different undo: a stroke saves the height or mask values it changes.
- The render graph has custom post passes, but no PBR material path for surfaces. Our look is toon. Per-layer PBR is a
  new shader path, not an existing one.

## Scope (what the feature list asks for)

1. Foliage from painted density maps, culled per chunk, with distance fade, height limits and shadow casting.
2. A placement brush that scatters instanced objects.
3. Detail layers blended by height and slope, with paint masks and a PBR surface per layer.
4. Erosion, noise, stamp, terrace and mask brushes.

## Proposed design

### Data
- `Terrain` component on a scene instance. Fields: heightmap file (16-bit greyscale PNG or raw `.u16`), size in
  metres (X and Z), height scale, chunk size in cells (default 64), and the list of layers.
- Heightmap grid: up to 1025 by 1025 samples per terrain in v1. Larger needs streaming, which is out of scope.
- Layer masks: one 8-bit RGBA mask per four layers, at heightmap resolution, stored beside the heightmap.
- Density maps: one 8-bit mask per foliage type, same grid. Density scales the scatter probability.
- Foliage type: a model path, a density mask, height min and max, slope max, scale range, a seed, and a distance
  fade. Scatter uses a fixed seed, so the same map gives the same placement on every load.
- Scene file: a `terrain` key on the instance, with the paths above, written the same way as `lod` (optional, omitted
  when empty).

### Rendering
- Chunks: each chunk is a grid of `chunkSize` by `chunkSize` quads with its own vertex and index buffers. Normals come
  from the heightmap by central differences.
- Chunk LOD: two levels per chunk, full and half resolution, chosen by distance. This reuses the simplification path
  of item 15 if we decide to, or a plain half-step grid. The half-step grid is simpler and is the first choice.
- Culling: a chunk is skipped when its AABB is outside the frustum (`boxOutsideFrustum`) or past the cull distance.
- Layers: the surface shader blends up to 8 layers. Weights come from the layer masks and from height and slope
  rules. A layer has a diffuse, a normal, and a roughness texture, plus a tiling scale. This is a new shader.
- Foliage: a new instanced path. Each chunk has an instance buffer of matrices. One instanced draw per foliage type
  per chunk, with the density and slope already applied on the CPU at scatter time. The shader uses the same toon
  lighting as the models where it can.
- Shadows: chunks cast shadows through the existing shadow pass. Foliage casts only if the type asks for it,
  because each instance adds shadow cost.

### Physics
- One Jolt mesh shape per chunk (`PhysicsWorld::addStaticMesh`), built from the chunk's own triangles. Rebuilt for a
  chunk when a brush edits it. The first plan was Jolt's height-field shape, but it needs the sample count divided by
  the block size to be a power of two. A 64-cell chunk has 65 samples per side, which does not fit, so the height field
  is not used.
- Water and buoyancy (item 17) use the terrain only through the world's collider, so no change is needed there.

### Brushes (editor only)
- Each brush edits either the heightmap, a layer mask, or a density mask. The brush falloff is a smooth curve with
  a radius and a strength.
- Noise, terrace, and stamp brushes change the heights. The mask brush paints a layer or density mask.
- Erosion is a CPU pass with a fixed number of iterations (particle-based hydraulic erosion, a common public method).
  It runs on the whole heightmap when the user asks, and it is undoable as one step.
- Placement brush: paints foliage instances on the density mask, or places a chosen model at the cursor. Placed models
  become scene instances, so they keep the normal Inspector and undo.
- Undo: a stroke records the old values of the changed cells and chunks. One stroke is one undo step.

## Build order (once the design is agreed)

Step 1 status (2026-10-16): done and tested. `src/engine/terrain.{h,cpp}` (heightmap, chunk meshes that share edges,
raw 16-bit file, physics ray onto a chunk), `src/engine/terrain_gpu.{h,cpp}` with `shaders/terrain.{vert,frag}` (chunks drawn
before the grid, culled per chunk), the `terrain` key in the scene file, Create > Terrain..., and the Inspector's read-only
Terrain section. Physics: one static mesh per chunk, placed at the tile origin at Play. Tests: `systems_test` 161/0;
editor self-test 602/0 (section 17). Not in step 1: layers, foliage, brushes, and any Inspector editing of the tile.

Step 2 status (2026-10-16): layers and the layer shader are built and tested. `src/engine/terrain.h` has `TerrainLayer`
(colour, roughness, height band, slope band, soft edges) and up to 8 layers per tile. `src/engine/terrain_layers.{h,cpp}`
has `layerWeightMap`: each layer's weight is its height band times its slope band times its paint, normalised to one, and
layer 0 takes a sample that nothing matches. The weights reach the GPU per vertex: layers 0 to 3 in the weights slot and
layers 4 to 7 in the joints slot (u16 over 65535), since the terrain pipeline does not skin. The layer colours and
roughness sit in a uniform buffer in the ShaderMaterial set at binding 15. The shader blends them and applies a toon
ramp whose edge width follows the roughness. The scene file holds the layers. Tests: `systems_test` 175/0 (14 layer
checks); editor self-test 604/0 (section 17, with a red-layer frame check). The paint is in memory only (all painted
by default); its file format and the mask brush are step 4.

Step 2b status (2026-10-16): per-layer textures are built. Each layer has optional diffuse, normal and roughness image
files and a tile size in metres. The scene file keys are `diffuseMap`, `normalMap`, `roughnessMap` (relative to the
project) and `tile`. The layer textures are packed into two atlases of 4 by 2 cells of 256 texels (one cell per layer,
so 8 layers fit): the diffuse atlas has the roughness in its alpha, and the normal atlas holds the tangent-space normals.
The two atlases are bindings 0 and 1 of the ShaderMaterial set, and the layer uniform buffer is binding 15. The shader
tiles each cell with explicit gradients, so there are no seams at the repeats. The normal maps use a derivative-based
tangent frame, so the terrain needs no tangents. Limit: the atlases have no mipmaps, because the mips of one cell would
bleed into the next. Distant ground shimmers. Tests: `systems_test` 184/0 (8 atlas checks, including a missing file
being an error); editor self-test 606/0 (a textured layer changes the frame, and a missing texture is refused with the
tile kept). Fixtures are in `tests/data`. The default layers have no maps, so they stay untextured. The Inspector has
no texture fields yet (step 6), so a map is set by editing the scene file.
Test heightmap: `tests/data/terrain_hills_257.u16` (257 samples, 4 by 4 chunks).

Step 3 status (2026-10-16): foliage is built. A `FoliageType` (in `terrain.h`) has a name, a shape (`tuft` or `cone`),
a density per metre cell, an optional density image (red channel), a normalised height band, a slope limit, a scale
range, a seed, fade distances, and a tint. The scene file key is `foliage`, a list of up to 4 types. Its key is written
only when the list is not empty. Shapes are built in code: a crossed-quad tuft (1 m) and an eight-sided cone (2 m).
Model files are not used yet, so the static-model source is still open. `scatterFoliage` draws one candidate per cell
from fixed hash draws. A candidate is kept when its draw is below density times the density field, and the height and
slope limits pass. A lower density therefore keeps a subset of a higher one's instances. The instances are grouped by
chunk and uploaded in batches of 256. Each batch is 3 vec4 rows (12 KB) at binding 15 of the ShaderMaterial set, which is
now visible to the vertex stage. A new `Device::drawIndexedInstanced` draws each batch, in both backends. The renderer
skips a batch when its chunk is outside the view or past its fade distance. The vertex shader shrinks an instance towards
its base between the fade distances, measured from `frame.camPos`. Foliage has no shadows and no textures yet.
Tests: `systems_test` 206/0 (22 foliage checks: counts, subset, seed, height and slope limits, density field, chunk
grouping, batch rows, meshes, density map load). Editor self-test 611/0 (section 17: trees are drawn, a missing density
map is refused and keeps the trees, and the trees survive a save and reopen). Frame: `renders/terrain_step3.png`.
Fixed on the way: `slopeDegAt` halved the slope at tile edges (it divided by two cells where only one exists), which
now matches `Heightmap::normalAt`. Not checked yet: the tuft shape and the distance fade in a frame. Both are verified
only by the CPU tests and the shader compile. The Inspector has no foliage fields yet (step 6).


1. Heightmap data, the chunk mesh, the Jolt height field, and the scene key. Tests: height sampling, chunk bounds,
   and that the physics ray hits the surface.
2. Layer masks and the layer shader. Test: weights sum to one and follow the height and slope rules.
3. Foliage scatter from density and rules. Instanced draw and per-chunk culling. Test: the same seed gives the same
   count, and the count drops with the density.
4. Editor brushes: noise, terrace, stamp, mask, then erosion. Each with an undo test on the heightmap.
5. Placement brush. Test: a painted density gives instances only where the mask is set.
6. Inspector and Hierarchy entries for terrain, layers, and foliage types.

Each step gets a `systems_test` or GPU test, and the editor self-test gets a section, as in earlier batches.

## Decisions (2026-10-16)

1. Look: PBR layers with the toon lighting ramp (user chose the recommendation).
2. Foliage: static models only in v1. No billboards.
3. Erosion: editor bake only, saved into the heightmap.
4. World size: several terrain tiles, each 1025 by 1025 samples, placed side by side. Only nearby tiles are loaded
   or drawn. The user chose tiles over one streamed or one large in-memory terrain. The target world size is not
   given yet, so the number of tiles is open.
5. Shadows for foliage: still open. Recommendation is off by default and on per type.

## Tiles (replaces the one-terrain limit above)

- A scene has any number of tiles. Each tile is a `Terrain` instance with its own heightmap, masks, and foliage
  density. Tiles have a grid position (column and row), so they line up at their edges.
- Neighbouring tiles share their edge samples, so there is no crack at the seam. The height of an edge sample comes
  from the tile that owns it.
- Physics uses one mesh shape per chunk, as above. Brushes that cross a tile edge update both tiles, and undo
  covers both as one step.
- Only tiles within a set radius of the camera are drawn, and their chunks are culled as above. Tiles beyond the
  radius are not drawn. Loading is per scene load, not streamed while the camera moves, in v1.
- Memory grows with the number of tiles. The limit to check with the user is how many tiles per scene.

## Decisions still needed before code

1. Target world size, in tiles or metres (for example 4 km by 4 km). This sets the tile count and the load radius.
2. Go-ahead to build, in the order of the build list above.

## Known limits (expected for v1)

- No streaming in v1. Every tile in the scene is in memory when the scene loads.
- Edge seams between tiles are checked by the test, not by eye.
- Foliage has no wind animation in v1.
- Not checked on a GPU. The sandbox renders on software Vulkan, so visual checks will be coarse.

## Steps 4 to 6: brushes, undo, erosion, and the Inspector (built 2026-10-16)

Where the code is:

- `editor/src/engine_session.{h,cpp}`: the in-memory tile store (keyed by heightmap path), `terrainDab`, `terrainErode`,
  `applyTerrainEdit`, `flushTerrainUploads`, `saveTerrainEdits`.
- `src/engine/terrain_edit.{h,cpp}`: the CPU brushes (`heightDab`, `paintDab`, noise, terrace, stamp), erosion
  (`erodeHeightmap`), the raycast, and the merge helpers for undo records.
- `editor/src/main_window_terrain.cpp`: the stroke handlers, undo records, erosion command, the terrain Inspector, and
  self-test section 18. It is a separate file because `main_window.cpp` no longer compiles in this machine's memory.
- `editor/src/inspector_ui.h`: `uiIcon` and `addSection`, shared with the Inspector.

How it behaves:

- A press on a terrain tile starts a stroke when a brush tool is selected. A press off the terrain returns false, so
  selection still works. A stroke is one undo step. Its records are listed under `"terrainUndo"` in the snapshot taken
  before it. Each record is keyed by heightmap path and holds the `before` and `after` values. Undo applies the records
  in reverse order, and redo applies them forwards.
- Dabs merge into the stroke's last record while they change the same thing (one heightmap, or one mask channel).
  Records no undo or redo step refers to are pruned at release.
- The GPU copy follows the stroke at most every 300 ms, and is flushed on release, undo, redo, and erosion.
- Heights: smoothstep falloff, `h + (f - h) * w * strength`, clamped to [0, 1]. Noise, terrace, and stamp use it.
  The Inspector's height per dab is in metres and is divided by the tile's height scale in the session.
- Layer masks: Allow and Forbid. The first use makes a field of 255 (all allowed). Density: Plant and Clear. The first
  use makes a field of 0 (no trees, except where planted). This is deliberately asymmetric, and the labels say so.
  Undoing the first paint removes the field, so the density falls back to uniform.
- Erosion: particle-based, on the whole tile, one undo step. It blocks the editor while it runs. The Inspector's
  default is 80000 droplets.
- Save: heights go to the heightmap file. Layer masks go to `<stem>_layers0.png` and `<stem>_layers1.png`. Density goes
  to `<stem>_density<t>.png`. All are written next to the heightmap. A mask or density field that undo emptied is removed
  from disk, and its scene key is cleared, so a reopen does not bring back undone paint.
- Scene open drops the in-memory edits, because the files are the truth at that point.
- Paths: terrain paths are cleaned when a scene is read, so a heightmap outside the project keeps one store key.
  Each held mask records the image it came from. When the settings name a different image, it is read again, and the
  edit is refused if the image is missing. An empty mask list (an older undo snapshot) keeps the held mask.
- The placement brush paints density only. Placing models is deferred, so no model is placed by a brush.
- The Hierarchy shows a terrain's layer and foliage counts under its row. These rows cannot be selected. The Inspector
  edits them.

Known limits (report these with the step):

- Dabs are clipped to the tile under the first hit point. A stroke across a tile edge does not change the neighbouring
  tile, so a seam can show at the edge. This differs from the tile section above, which asked for both tiles to update.
  Fixing it needs the dab to be applied to every tile it overlaps, and undo to cover all of them.
- No mipmaps on the layer atlases, so distant terrain shimmers.
- Erosion blocks the editor for the whole run.
- Foliage in v1 uses procedural tuft and cone meshes only. Model files are deferred.
- Foliage fade is not checked in a frame. GL and Vulkan are built but only the Vulkan path is run.
- The 300 ms upload throttle is not timed on a 1025 tile. The test uses 257 samples. Measure before relying on it.
- Self-test 638 of 638 (about 9 minutes). Section 18 is on temp copies only, and tests/data is checked by hash.
