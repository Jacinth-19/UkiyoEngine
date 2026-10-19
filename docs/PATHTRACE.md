# Path tracing that runs on an iGPU (item 22)

The reference is [ColinLeung-NiloCat/NiloPathTracing](https://github.com/ColinLeung-NiloCat/NiloPathTracing), a Unity URP
path tracer built on hardware ray tracing (DXR / Vulkan ray queries) with DLSS Ray Reconstruction and native NiloToon
support. On an RTX card it is a real-time answer to "what would this frame look like if it were path traced". The
project's own requirements make the wall explicit: Windows/DirectX 12, Unity 6, and **all NVIDIA RTX GPUs with 16 GB
of VRAM or more** — and its point is to replace the raster lighting stack outright: no lightmaps or probe volumes, no
SSAO/GTAO, no SSR or reflection probes, no shadow maps, no PCSS. Every one of those jobs is done by traced rays.

An integrated GPU has no RT cores and no BVH traversal units; DLSS Ray Reconstruction needs tensor cores and an NVIDIA
driver. There is nothing to port. The same program on an iGPU is either a compute shader marching a software BVH at
single-digit frames, or it is not path tracing. This item is therefore answered in two halves:

| | What it is | Where it runs |
| --- | --- | --- |
| **Ground truth** | `tools/pathtrace_preview.cpp` — a CPU path tracer of the Terrain Lab scene | any CPU; the GPU is not involved |
| **What the engine draws** | cascaded sun shadows, SSAO, terrain layer blending and grass, at frame rate | every iGPU the editor already runs on |

The tracer is the reference the real-time renderer is judged against. It is deliberately not an engine feature:
nothing in it touches the RHI, so it cannot regress a build, and it produces the same image on a laptop with an Intel
UHD and on a workstation.

## 1. The tracer

`tools/pathtrace_preview.cpp` renders the engine's own terrain, not a stand-in scene: it loads the same heightmap
(`tests/data/terrain_hills_257.u16`), the same three vendored ground textures, and it reproduces the template's layer
bands. What it adds is light transport.

- **Heightfield intersection.** A heightmap is a height function, so there is no acceleration structure to build and
  no triangles to store. Rays march adaptively — the step grows with the height above the ground — and 26 bisection
  steps land the hit. A ray that climbs above the tile's highest point stops immediately.
- **Sun.** The direct term is a shadow ray marched against the same heightfield; the sun is a direction, not a light
  with a mesh, so the penumbra is the heightfield's own, not a shadow-map texel.
- **One diffuse bounce.** Cosine-weighted, 85 % continuation, terminated against the sky or the terrain. That single
  bounce is what fills a shadow with the colour of the ground around it; two bounces on a heightfield change very
  little at this scale.
- **Sky and grade.** The gradient, the cirrus deck and the sun glow are authored in *display* space and converted
  back through the inverse of the tone curve, so the sky is the colour it was designed to be rather than whatever
  ACES does to a guessed radiance. Indirect light uses the converted radiance, so it stays physically consistent.
  The frame ends in the same ACES curve the engine's 0 ACES operator uses.
- **Aerial perspective and detail fade.** A view-distance haze pulls distant ridges towards the sky, and the textures
  fade to their band's mean colour past ~210 m, which is the CPU equivalent of a mip chain.
- **AA.** Jittered primary rays, one per sample; rows are handed to a thread pool.

CLI: `pathtrace_preview <out.png> <root> [width height samples]`. Defaults are 1280×720 at 64 samples.

## 2. The image

![The Terrain Lab tile, path traced](pathtrace-terrain.png)

*1600×900 at 96 samples. Sun azimuth 238°, elevation 24°; camera on the ridge above the basin looking north-east
across the valley. The grass is not geometry here — the tracer shades the ground with the grass layer's albedo, so
the tufts visible in `terrain-hero.png` are absent. This is the light reference, not the draw reference.*

## 3. Cost

Measured on the two cores of the CI container, 1600×900:

| Samples | Time |
| --- | --- |
| 36 | ~1 min |
| 96 | ~4 min |

That is one to two orders of magnitude away from real time, which is the point: this is the branch you run when you
want to know what the right answer looks like, not when you want a frame.

## 4. What the real-time renderer does instead

On an iGPU the editor draws the same scene with rasterisation, and the differences are the whole story of this item:

| Effect | Ground truth | Real-time |
| --- | --- | --- |
| Sun visibility | exact ray march, soft where the ridge is | 3-cascade shadow atlas with a PCF kernel |
| Ambient | sky dome sampled over the hemisphere, plus one bounce | hemispherical ambient in the toon shader and SSAO in `shaders/effects.frag` |
| Interreflection | one cosine bounce | not simulated; the shadow tint and the SSAO darkening stand in |
| Grass | albedo only | 262 144 tufts a tile, scattered on the CPU, wind and distance bands in the vertex shader |
| AA | jittered supersampling | FXAA, SSAA 2×, or TAA |

The honest gap is interreflection and contact occlusion. SSAO covers the second at a fraction of a millisecond and the
one-bounce colour bleed is what a future screen-space GI pass would add; both are raster-cheap, which is why they are
the iGPU-suitable parts of the reference's look.

## 5. Reproducing

```sh
cmake --build build --target pathtrace_preview
./build/pathtrace_preview docs/pathtrace-terrain.png . 1600 900 96
```

`pathtrace_preview` is built by the "Build Everything" CI step, so the tool cannot rot even though no test runs it.
There is no reference image comparison: the tracer is the reference.
