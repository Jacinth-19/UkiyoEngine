# Environment lighting: cubemaps, IBL, and light probes

Batch 6 of the rendering roadmap. Until now a surface's ambient was two colours — `ambientSky` above,
`ambientGround` below — mixed by the normal's height, and RealToon's reflection sampled the same two colours along
the reflected ray (`toonReflection` in `shaders/toon_extras.glsl`). Real places are not two colours: a lantern-lit
street is warm near the lantern and nearly black ten metres away, and a chrome badge should show the street. This
batch adds the three standard pieces, implemented in our own way on our RHI:

* a **cubemap** the renderer can sample — six faces uploaded per face, mip-chained, at Frame binding 8,
* **image-based lighting** — the ambient comes from a projected spherical-harmonic probe, the specular from a
  **prefiltered** cubemap (the split-sum idea cut down to what a toon material needs),
* **light probes** — lattices of SH probes in world-space boxes, trilinearly interpolated like Unity's light-probe
  volumes.

`src/engine/environment.h` is the module; it is pure CPU maths (no GPU, no window), so the whole chain runs in
`tests/environment_test.cpp` on every machine, CI included.

## The chain, once per scene load

```
procedural sky (a CPU twin of shaders/sky.frag)   or   equirectangular panorama PNG
        |                                                      |
        +---------------------->  Cubemap  <-------------------+
                                  (6 float faces)
                       |                            |
        irradianceFromCubemap                  prefilteredChain
        (SH L2, band-scaled)                   (GGX lobes per mip)
                |                                        |
     bakeVolume(probe lattices                  packMipChain -> RGBA8
     + the scene's lights)                      face-major mip chain -> RHI
                |                                        |
        FrameUBO.envSH[9] + envParams            Frame set, binding 8 (uEnvCube)
```

Everything is baked in `Renderer::setEnvironment` when the scene loads, on the CPU, in tens of milliseconds. There is
no GPU bake pass and no load-time hitch: the phone runs the same code as the desktop.

## Conventions (both tested from both sides)

* **Cubemap layout** is the GPU's: face order +X, −X, +Y, −Y, +Z, −Z, `u` right and `v` down, the standard
  `sc/tc/ma` mapping. A CPU bake needs no swizzle on the way to `samplerCube`.
* **SH basis** is the standard real basis (Y00 = 0.282095, L1 = 0.488603, L2 = 1.092548/0.315392/0.546274), 9
  coefficients per channel in Unity's documented order (L00, L1−1(y), L10(z), L11(x), L2−2(xy), L2−1(yz), L20(zz),
  L21(xz), L22(xx−yy)). `FrameUBO.envSH[9]` carries one coefficient per `vec4` slot as (r, g, b).
* **Coefficients hold irradiance in colour units**: a uniform white environment evaluates to (1, 1, 1) at every
  direction. The radiance→irradiance conversion is Ramamoorthi's band scales over π, (1, 2/3, 1/4). This is what
  makes switching the ambient on brightness-neutral: a scene that says nothing renders exactly as it did before.
* **GI equalisation is MToon's** (VRMC_materials_mtoon 1.0, our own implementation):
  `gi(n) = lerp(rawGi(n), uniformedGi, f)` with `uniformedGi = (rawGi(up) + rawGi(down)) / 2` and `f` the material's
  existing `giEqualization` (default 0.9). Toon stays toon even under a fully directional ambient; the C++ twin is
  `env::equalizedGi`.

## Ambient: one probe per frame, at the camera's focus

`shaders/mtoon.frag` replaces the hemisphere mix with the probe when `envParams.x > 0`; otherwise the old path runs
untouched. The probe is interpolated from the volumes at the **camera's focus point** — the same decision the scene
light ranking makes (`engine/lights.h`): per-object SH would need a per-draw uniform the push-constant draw path does
not have, and the real-time GI pass adds the screen-space variation on top of the probe's baseline. In practice a
camera looking at a lantern gets the lantern's warm probe, exactly as it gets the lantern's light.

Probes are baked from the environment **plus the scene's lights**, using the same windowed inverse-square the toon
shader draws (`window² / (1 + d²)`, the reach clipping to nothing) — so baked ambient and direct light agree at the
same spot by construction. Directional lights project their own irradiance function, `max(0, n·l)`.

## Specular: prefiltered mips instead of a BRDF LUT

`toonReflection` samples `uEnvCube` along the reflected ray when the environment is present. The mip is picked by the
gloss mask (`G = 1 − roughness`) squared across the chain — the split-sum prefilter's job, minus the BRDF
integration: a toon material does not want a physically-integrated specular, it wants a sharp mirror on a clean
surface and a soft sheen on a rough one, with the Fresnel and strength staying stylistic. `environment.
reflectionStrength` overrides the project default per scene (`< 0` keeps `reflection`).

The prefilter itself is Karis' importance-sampled GGX convolution (Halton sequences, `N·L` weights), normalised so a
uniform environment prefilters to itself at any roughness — energy-conserving where the raw filter is not.

## Scene format

```json
"environment": {
    "enabled": true,
    "mode": "sky",                  // or "panorama" with a project-relative RGBA8 PNG, width 2*height
    "intensity": 1.0,
    "reflectionStrength": 0.5,
    "cubeSize": 64,
    "volumes": [
        { "center": [0, 1.4, 0], "size": [24, 3.2, 24], "divisions": [5, 2, 5] }
    ]
}
```

Absent, or `"enabled": false`, means everything above is skipped. Volumes clamp at their box like Unity's; where two
boxes overlap the later one wins. `Afterglow` ships one 5×2×5 volume over the village, so the stone lanterns warm the
probes around them and the ambient between pools of light falls to the night sky.

## RHI additions

`TextureDesc.faces == 6` makes a texture a cubemap. OpenGL creates a `GL_TEXTURE_CUBE_MAP` and uploads faces as
z-layers; Vulkan adds `VK_IMAGE_CREATE_CUBE_COMPATIBLE`, six array layers and a `VK_IMAGE_VIEW_TYPE_CUBE`. The mip
chain for a cube is **face-major** (face 0's whole chain, then face 1's — `packMipChain`'s output). The Frame
descriptor layout grew binding 8 (a combined image sampler); a 1×1 white cube is bound there whenever a scene has no
environment, so every pipeline's descriptors are satisfied and sampling it is harmless.

## Tests

`environment_test` (47 checks): the face mapping round-trips and covers 4π steradians; panorama bands decode to the
right faces; a white cubemap evaluates white everywhere (the colour-unit contract); a unit sun's irradiance has the
analytic DC term and dies around the sphere; GI equalisation matches the MToon formula at both ends; a point light's
reach clips to nothing; probe lattices interpolate exactly at probes and clamp outside; the prefilter is
energy-holding on white and mixes hemispheres only as roughness grows; the mip chain has the RHI's exact level count;
the frame packing writes the slots `shaders/frame.glsl` reads.
