# Terrain, production pass (item 20)

Item 18 built the tile: a heightmap, chunk meshes, up to eight layers blended by height, slope and paint, a layer
atlas, masking and the brush. Item 19 added grass. This pass turns the pair into something a team can ship a level on,
and it starts with the ground itself.

![The Terrain Lab tile with the vendored ground textures and grass](terrain-hero.png)

*The Terrain Lab tile — the sample heightmap with the moss, heather and mud from the reference project, the detail
overlay, occlusion and grass at the template's settings — rendered offline by `tools/terrain_preview.cpp`. It is a
preview of the scene the template writes, not an engine frame: this sandbox has no GL or Vulkan (see
[Limits](#limits-that-remain)).*

## 1. The assets are in the engine now

The reference project's ground textures were not in Ukiyo before this pass — the terrain used flat layer colours, or
whatever a project supplied. They are now vendored at **512 × 512** in `editor/res/terrain/`:

```
grass_moss_albedo.png   grass_moss_normal.png   grass_moss_ao.png
heather_albedo.png      heather_normal.png      heather_ao.png
muddy_albedo.png        muddy_normal.png        muddy_ao.png
plant_detail.png
```

They come from [`Jacinth-19/UnityGrassIndirectRenderingExample`](https://github.com/Jacinth-19/UnityGrassIndirectRenderingExample),
MIT, © 2024 EricHu. `editor/res/terrain/NOTICE.md` carries the licence and the exact transforms. The import is a
script, not a manual step: `tools/import_reference_terrain.py` reads the 2048² TIFFs, downscales, pulls ambient
occlusion out of the HDRP mask map, and writes the ten PNGs in about a second. The full TIFF set is ~1.3 GB of
uncompressed data; the vendored set is 4.3 MB and is exactly what the shader consumes.

## 2. Three shading additions that stop a field looking tiled

| Added | Where | Why |
| --- | --- | --- |
| **Occlusion** from the mask map, in the alpha of the normal atlas | `terrain_layers.cpp`, `terrain.frag` | Darkens only the sky light, so a clump of moss keeps its shape without the whole ground going brown in shadow. |
| **Detail overlay**, `plant_detail.png` tiled at 1.7 m and faded out by 45 m | `terrain.frag`, `TerrainDetail` | The near ground has structure at a scale a 4 m layer repeat cannot give. This is what the camera sees most of. |
| **Macro variation**, the same texture at 0.013 and 0.047 repeats, luminance only | `terrain.frag` | Breaks up the flat colour of one layer across a valley. Together with the detail overlay it is the standard answer to visible tiling, and it costs three texture reads. |

The atlas cell also grew from 256 to **512** texels (4 × 2 cells, 2048 × 1024 RGBA8). At the layer repeat the template
uses (3.5–5.5 m) that is roughly 100 texels per metre, which is what makes the near ground hold up at eye height.

## 3. Terrain now casts, and grass now receives

Before this pass the shadow atlas held only meshes, so a hillside had no shadow of its own and grass under a ridge was
lit as if the ridge were not there.

* **Terrain is a shadow caster.** `shaders/terrain_shadow.vert` draws the tile depth-only into each cascade, from
  `Renderer`'s shadow pass, with the same per-chunk frustum cull as the colour pass.
* **Terrain and grass read the atlas.** Both include `shadow_sample.glsl`; the sun term of the toon ramp is multiplied by
  `shadowFactor`, so a blade behind a hill is lit by the sky alone.
* **The receiver bias is slope scaled.** A height field lit at a grazing angle shadows itself in stripes, because the
  depth error across one shadow texel grows as `1 / N·L`. `shadow_sample.glsl` now recovers the cascade's world texel
  size from its matrix (`2 / (|row0| · 2048)`) and pushes the sample along the light by `2 · texel / max(N·L, 0.25)`.
  This is the difference between "shadows are on" and "shadows are usable" on rolling ground.

## 4. Grass at meadow density

The catalogue of what the reference does, what Ukiyo's RHI cannot do, and how each stage was re-cut is
[GRASS.md](GRASS.md). Two numbers changed in this pass, because the first cut was tuned for a test rather than a meadow:

| | Before | Now | Why |
| --- | --- | --- | --- |
| `kGrassMaxInstances` | 98 304 | 262 144 | A 256 m tile at 4 tufts/m² asks for exactly 262 144; the old cap silently halved 2.5/m² by stride. |
| Template meadow | 0.9/m², 4 blades, 0.45 m | 4/m², 8 blades, 0.44 m | Finer, shorter blades, more of them: four tall wide blades read as reeds, not as a sward. |
| Grass layers | layer 0 only | layers 0 and 3 | The template's heather band carries grass too, so the meadow climbs the slope instead of stopping at the valley floor. |

The tuft itself is a clump: roots on a disc of `0.55 × height`, blades on the golden angle leaning further out as
they rise, and two per-blade hashes for lean and height, so no two blades in a tuft are the same length. Eight blades
of four quads is 64 triangles a tuft; the bands keep a real view near three million, while a whole tile held at once
would be 16.8 M. A project can lower `density`, `blades`, `segments` or `cullDistance` per tile to trade it back.

## 5. What a project gets

Creating a **Terrain Lab** project writes a scene with:

* four layers — grass moss, rock (mud), pale rock, heather — with the vendored albedo, normal and occlusion maps
  *linked* from the engine folder (relative paths, like the character asset, so the project stays small),
* the detail overlay at 1.7 m, strength 0.5, macro 0.28,
* grass on at 4/m² over the moss and heather, bands 26 / 55 / 110 m,
* fog at 0.0035 per metre with sun in-scatter 0.6, and a sun low enough to model the ground.

Everything is editable: layer bands and maps in `Scenes/Main.ukscene` (`layers[]`, `detail`, `grass`), grass density
and shape in **Terrain → Grass**, the look in the scene's settings.

## 6. Showing it without a GPU

This sandbox has no `/usr/include/GL`, no `libEGL`, `libGL`, `libOSMesa` or `libvulkan`, and `gl_device.cpp` needs a
GLFW context even when headless. CI's hub self-test does render — it writes `renders/terrain_grass.png` — but the
artifact is on Azure blob storage, which this environment cannot reach.

So the showcase is produced by **`tools/terrain_preview.cpp`**: a small software rasteriser that links the engine's own
`terrain.cpp`, `terrain_layers.cpp` and `grass.cpp`, so the heightmap, the layer weights, the scatter, the tuft mesh
and the default settings are the engine's, and only the shading is re-implemented (z-buffer, trilinear mips, a sun
shadow map with the same slope-scaled bias, the ported sky, fog, Custom Anime tonemap and grade).

```sh
c++ -O2 -std=c++17 -I src -I third_party tools/terrain_preview.cpp \
    src/engine/terrain.cpp src/engine/terrain_layers.cpp src/engine/grass.cpp -o build/terrain_preview
./build/terrain_preview docs/terrain-hero.png . 1600 900 2
```

Framing, sun, grade and the grass shape can be set without a rebuild: `UKIYO_EYE`, `UKIYO_EYE_H`, `UKIYO_TARGET`,
`UKIYO_TARGET_H`, `UKIYO_SUN_AZ`, `UKIYO_SUN_ELEV`, `UKIYO_FOG`, `UKIYO_EXPOSURE`, and `UKIYO_GRASS_DENSITY`,
`UKIYO_GRASS_HEIGHT`, `UKIYO_GRASS_WIDTH`, `UKIYO_GRASS_BLADES` for a what-if on the meadow. The image at the top was
made with `UKIYO_SUN_AZ=320 UKIYO_SUN_ELEV=22 UKIYO_FOG=0.0028 UKIYO_EXPOSURE=0.88 UKIYO_EYE=196,196
UKIYO_EYE_H=0.85 UKIYO_TARGET=178,178 UKIYO_TARGET_H=0.30`; the close-up in [GRASS.md](GRASS.md) is the same light from
`UKIYO_EYE=190,194 UKIYO_EYE_H=1.05 UKIYO_TARGET=174,182 UKIYO_TARGET_H=0.35` at 1000 × 620.

The tool is also a check: it is the only place where the scatter, the mesh and the settings are exercised together
without a device, and it is compiled by the normal CMake build so it cannot rot.

## 7. Limits that remain

* **No terrain LOD or clipmap.** A tile is drawn at full resolution always; 257² is cheap, 1025² is 2 M triangles. The
  next real step is a chunked LOD (skirts, or a geometry clipmap).
* **One tile per scene object.** There is no streaming and no neighbour stitching; a world of tiles is several objects.
* **No compute, storage buffers or indirect draw.** The grass scatter is CPU work per tile upload, and the instance cap
  exists because of it. GRASS.md lists exactly what an RHI change would buy back.
* **The layer atlas has no mips** (a cell's mips would bleed into its neighbours), so minification at a distance relies
  on the shader's explicit gradients and the detail overlay's fade.
* **Textures are RGBA8 UNORM**, sampled without an sRGB decode, so the albedo is treated as linear. The vendored maps
  are authored sRGB; the anime grade happens to like the result, but a colour-managed pipeline would use `SRGB8_ALPHA8`
  for albedo and keep normals/occlusion linear.
* **Grass does not cast shadows.** It receives them. Adding it to the-shadow pass means a second instanced draw with the
  same batch data and no colour output; it is a small renderer change, deliberately left out of this pass.
* **Water** sits over the tile as a volume (the template enables it), but the preview tool does not draw it, so the ponds
  in the sample heightmap are not in the figure above.
