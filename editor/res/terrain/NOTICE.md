# Terrain textures: origin and licence

The ten images in this folder are **derived** from the terrain sample assets of

<https://github.com/Jacinth-19/UnityGrassIndirectRenderingExample>

Copyright (c) 2024 EricHu, released under the MIT licence (reproduced below).

They are the ground truth of the grass system Ukiyo adapted in item 19: the reference's own grass, heather and mud,
so a Ukiyo project can look like the scene the system was designed against instead of inventing a meadow.

## What each file is

| File | Source | Transform |
| --- | --- | --- |
| `grass_moss_albedo.png` | `Assets/TerrainSampleAssets/Textures/Terrain/Grass_Moss_BaseColor.tif` | 2048 × 2048 sRGB → 512 × 512, no colour change |
| `grass_moss_normal.png` | `…/Grass_Moss_Normal.tif` | 2048 → 512, RGB kept (tangent space, +Y up) |
| `grass_moss_ao.png` | `…/Grass_Moss_MaskMap.tif` | green channel (HDRP ambient occlusion) → 512 × 512 greyscale |
| `heather_*.png` | `Heather_{BaseColor,Normal,MaskMap}.tif` | same three transforms |
| `muddy_*.png` | `Muddy_{BaseColor,Normal,MaskMap}.tif` | same three transforms |
| `plant_detail.png` | `…/Textures/Details/Plant_BaseColor.tif` | 1024 → 512, used as the near-field detail overlay |

`MaskMap.tif` is HDRP's packed mask (R metallic, G occlusion, B detail mask); the engine has no metallic or detail-mask
use for ground, so only the occlusion channel is kept, and it travels in the alpha of the terrain's normal atlas.

The conversion is reproducible: `python3 tools/import_reference_terrain.py [path/to/UnityGrass]` regenerates every
file from a checkout of the reference. Verified on a fresh sparse checkout of the reference (`Assets/TerrainSampleAssets/
Textures` only): the script rewrote all ten files pixel for pixel, so what is committed here is exactly what the
reference's art becomes under the documented transforms. 512 is not arbitrary: the terrain atlas cell is 512 texels, so the engine
resamples once at upload and the files are exactly the size the shader wants.

## Coverage against the reference folder

The reference's `Assets/TerrainSampleAssets/Textures` holds 40 `.tif` files: 31 under `Details/` and 9 under
`Terrain/` (counted with `find … -name '*.tif'` on a checkout of the reference, not from its README). This folder
carries 10 of them, and the split is deliberate.

| Reference set | Files there | Here | What uses it here |
| --- | --- | --- | --- |
| `Terrain/{Grass_Moss, Heather, Muddy}_{BaseColor, Normal, MaskMap}` | 9 | 9 | the terrain's layer atlas, and the path tracer in `tools/pathtrace_preview.cpp` |
| `Details/Plant_{BaseColor, MaskMap, Normal, Thickness}` | 4 | 1 (`Plant_BaseColor`) | the near-field detail overlay in `shaders/terrain.frag` |
| `Details/{BushDry, Bush_A, Fern, GrassDry, Grass_A, Grass_C, Heather_A}_*` | 27 | 0 | nothing yet |

The 30 files not copied are the reference's **billboard plants and their masks**: for each of the seven plant sets a
`BaseColor` and a `MaskMap` (opacity in alpha), and a `Normal` and a `Thickness` map for most of them, drawn as
camera-facing cards with alpha test. Ukiyo's grass is real geometry — eight blades of four quads per tuft, shaded in
`shaders/grass.vert`/`grass.frag` — so it has no card alpha and no transmission term to feed, and the shader would have
to grow an entirely different path before those files meant anything. They are not copied rather than copied and never read; if a future card-based impostor LOD needs them,
`tools/import_reference_terrain.py` is the place to add the conversion, and the licence above already covers them.

`Details/*_Thickness.tif` is the one set with no destination at all: the engine's grass shading has a single
transmission colour, not a per-pixel thickness map.

## MIT licence

```
MIT License

Copyright (c) 2024 EricHu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
