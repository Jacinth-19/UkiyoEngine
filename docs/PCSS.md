# Percentage-closer soft shadows

A shadow map answers "is this point lit?" with one depth per texel, so every shadow it casts has an edge of about one
texel — whatever the light is. A fluorescent strip, a window, a two-degree sun and a point light all shadow the same,
and every caster, whether it is touching the floor or a metre above it, gets the same hard border.

Flax 1.13 added percentage-closer soft shadows (PCSS) for all three light types. Ukiyo implements the same two-pass
estimate in `src/engine/pcss.h` (the reference the tests and the figure run) and in `shaders/shadow_sample.glsl` (the
renderer's path), and this document measures both.

![PCSS against a hard shadow map and a tuned fixed-radius PCF](pcss.png)

*A slab, a crate, a thin plank and a low block on a floor, under a 1024-texel shadow map over 10.00 m — 9.8 mm per
texel, finer than the effect needs, so the difference between the panels is the filter and not the map. The sun is
3.4° wide, a stylised sun: the real one would put a 1.5 cm penumbra on a 20 cm gap and the effect would be invisible
at this scale. **Top left:** a single tap per pixel, the map's own edge. **Top right:** fixed-radius PCF with the
radius swept over nine values and handed its best one (0.22 m) — an oracle no scene can actually be tuned to, since a
game cannot sweep at run time. **Bottom left:** PCSS, 16 blocker taps and 32 filter taps. **Bottom right:** the same
scene traced with 64 rays over the sun's disc, the errors over the 12 764 pixels the truth calls partly lit, and the
visibility along a floor line crossing the crate's shadow edge, where the truth, PCSS and PCF are drawn against the same
distance. Mean absolute error: **hard 0.266, PCF 0.066, PCSS 0.055**. Made by `tools/pcss_preview.cpp`, which runs
`src/engine/pcss.h` — the code the shader mirrors — and prints every number the caption quotes.*

## 1. The two passes

```
// 1. Blockers: how far away is the thing casting this shadow?
blockerDepth = 0; found = 0
for i in 0..16:
    offset = vogel(i, 16) * searchRadius          // metres across the receiver plane
    d      = shadowMap[uv + offset * uvPerMetre]
    slack  = bias + receiverSlope * |offset|      // this surface's own drop over that offset
    if d > 0 and d < receiverDepth - slack:
        blockerDepth += d; found += 1
if found == 0: return 1.0                         // nothing between this point and the light

// 2. Penumbra: how wide is the shadow that blocker casts here?
gap   = max(receiverDepth - blockerDepth / found, 0)
width = min(2 * tanRadius * gap, maxPenumbra)     // directional light
lit   = 0
for i in 0..32:
    offset = vogel(i, 32) * width
    d      = shadowMap[uv + offset * uvPerMetre]
    if d > 0 and d < receiverDepth - (bias + receiverSlope * |offset|): lit += 1
return 1 - lit / 32
```

The width falls to zero where the receiver touches the blocker and grows with the gap, which is contact hardening —
the reason the technique exists, and the one thing a fixed-radius filter cannot do. For a local light the width is
`lightRadius * gap / blockerDistance` instead: the same disc, seen from a point instead of from infinity.

Both passes read a Vogel spiral, `vogelTap` in the header and `vogelTap` in the shader, so the CPU reference and the
GPU path visit the same texels in the same order and cannot drift apart silently.

## 2. The detail that decides whether it works

Two things had to be right before PCSS beat a plain filter, and both are receiver geometry rather than the algorithm.

**A slanted receiver shadows itself.** A tap a metre across the light's plane lands on the receiver's own plane a
`receiverSlope * |offset|` metre nearer or farther, where `receiverSlope = |L - (L·N)N|` — zero for a surface facing
the light, one for an edge-on one. Without that slack the floor's own neighbours count as blockers, the average blocker
distance collapses onto the receiver, and the penumbra comes out several times too wide. The tests in
`tests/pcss_test.cpp` and the figure both set it per hit normal; the shader derives it from `N·L`.

**A world distance is not a distance on the light's plane.** Along the sun's azimuth a metre of floor foreshortens to
`|N·L|` metres as the light sees it; across the azimuth it does not foreshorten at all. Filtering with the unfiltered
world width over-filters a horizontal floor by `1/|N·L|` — 1.6× at this sun. Before the fixes were made, at equal cost,
PCSS scored 0.115 against 0.089 for a fixed filter at its best radius: the soft shadow was *worse* than the hard one
tuned well. With the foreshortening term, 0.055 against 0.066.

That second one is worth stating plainly because it is easy to reproduce: a PCSS implementation that looks right on a
wall and over-softens on a floor has this bug, and no amount of tap tuning fixes it.

## 3. What the CPU header and the shader share

| | `src/engine/pcss.h` | `shaders/shadow_sample.glsl` |
|---|---|---|
| tap pattern | `vogelTap(i, n)` | `vogelTap(i, n)`, same constants |
| uv per metre | passed in: `1 / cascade world width` | `0.25 * length(shadowMatrix[c][0].xyz)` — half a cascade across the atlas |
| z per metre | metres native | `abs(shadowMatrix[c][2].z)`, an orthographic projection's depth scale |
| slope | `receiverSlope` field, set per hit | `sqrt(1 - (N·L)²)` |
| blocker/filter taps | `blockerSamples` / `pcfSamples` | 16 / 32, fixed |
| off | `mode` 0 / 1 / 2 | `frame.shadowSoft.y` |

The shader takes both scales from the cascade's own matrix rather than from a constant, so a cascade whose box changes
size with the camera keeps the same penumbra in metres. The cost is 48 dependent-not-coherent texel fetches per pixel
near a shadow edge, three more than the 3×3 tent filter's four gathers; the engine leaves PCSS on by default and the
settings below turn it off.

## 4. Numbers

The figure's scene, 756 × 456 pixels, error over the 12 764 pixels whose truth is between 5 % and 95 % lit:

| filter | mean absolute error |
|---|---|
| hard shadow map | 0.2655 |
| fixed-radius PCF, radius swept, best at 0.22 m | 0.0655 |
| **PCSS, 16 + 32 taps** | **0.0550** |

The fixed filter's sweep, which the tool prints as it runs — the shape is the point: too narrow and the shadow never
spreads, too wide and it turns the whole penumbra grey.

| radius (m) | 0.04 | 0.06 | 0.08 | 0.10 | 0.14 | 0.18 | 0.22 | 0.28 | 0.36 |
|---|---|---|---|---|---|---|---|---|---|
| error | 0.2010 | 0.1740 | 0.1511 | 0.1314 | 0.1015 | 0.0794 | **0.0655** | 0.0698 | 0.0950 |

`tests/pcss_test.cpp` runs the same header against a closed-form answer — the fraction of a disc whose ray clears a
straight edge — rather than against another filter. 30 checks, 0 failures: the curve, the contact hardening, both
formulas, the clamp, the off switch, the tap pattern, and — separately — the shader's own algebra, which works in the
shadow matrix's units rather than in metres. That last section builds the orthographic matrix the renderer builds,
derives `uvPerMetre` and `zPerMetre` from its rows exactly as `pcssShadow` does, and runs the shader's arithmetic
against the header's on the same map: if the GPU's scale factor were wrong, no test of the header would notice, and
every penumbra in the engine would be wrong by that factor. On a single gap at 0.5 m, mean error is **PCSS 0.0488, PCF 0.0712,
hard 0.0522**; across three gaps that share one filter, **PCSS 0.0526 against PCF 0.1522**, because one radius cannot
be right for 0.25 m and 1.0 m at the same time. That spread, not the single-scene mean, is the argument for the
technique.

## 5. In the engine

- `src/engine/pcss.h` — the algorithm, pure C++, no device, no GLSL.
- `shaders/shadow_sample.glsl` — `pcssShadow`, called from `shadowFactor` when `frame.shadowSoft.y` is set; without it
  the 3×3 tent filter runs as before, so a project that turns the feature off pays nothing.
- `FrameUBO.shadowSoft` (`src/engine/gpu_types.h`, `shaders/frame.glsl`) — `x` the sun's angular radius as a tangent,
  `y` the switch, `z` the search radius in metres, `w` the penumbra cap. It is a `vec4` at the end of the block, because
  std140 has no packing to save.
- `RenderSettings` (`src/engine/renderer.h`) — `pcss`, `pcssSunTanRadius` (0.02: about 1.1°, three times a real sun and
  four times softer than the figure's), `pcssSearchRadius` (0.25 m), `pcssMaxPenumbra` (0.5 m).
- Scene settings and the command line carry the same names; `--no-pcss` is the switch the demo app takes.

## 6. What it does not do

- **It does not replace the contact shadow pass.** PCSS's penumbra goes to zero at contact, but the shadow map still
  cannot resolve a gap narrower than its texel, so `docs/CONTACT_SHADOWS.md` still earns its place under a crate.
- **It is not a denoiser.** 32 taps over a penumbra 30 cm wide is a coarse estimate; TAA, which is already on, is what
  makes it look smooth in motion. Without TAA the figure's own noise is visible in a still.
- **Local lights are implemented and untested in the renderer.** The penumbra formula supports point and spot lights
  with a radius, but the cascade path only casts the sun's shadow; point-light PCSS needs a cube map first.
