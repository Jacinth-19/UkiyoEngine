# Multi-bounce ambient occlusion (item 28)

Single-bounce AO asks "how much of the sky can this point see?" and multiplies the ambient light by the answer. In a
corner the answer is 0.25, so the corner goes to a quarter of the light — which is wrong. The occlusion it measures is
*itself* lit, and some of that light reaches the corner after one more bounce. The corner is dark, but never that dark,
and never grey.

Godot 4.8 shipped the industry's cheap answer to this (GH-115426), from *Practical Real-Time Strategies for Accurate
Indirect Occlusion* (Activision, SIGGRAPH 2016): replace the linear `ao` with a cubic in `ao` whose coefficients come
from the surface albedo. Ukiyo had only the linear term; this item adds the cubic, the albedo it needs, and the test and
figure that show what it does.

![Multi-bounce AO against single-bounce AO and a two-bounce tracer](ao-preview.png)

*A corner of three planes — a red wall, a blue wall, a grey floor — shaded four ways with the same camera and the same
visibility estimate. **Top left:** ambient with occlusion off. **Top right:** what the engine did before this item:
`albedo · sky · ao`, which turns both walls a muddy near-black and the floor a flat grey. **Bottom left:** the
multi-bounce fit: the corner keeps more light and keeps its colour. **Bottom right:** a two-bounce path trace, the ground
truth both are approximating. Luminance RMSE against the tracer, per pixel: **0.241 single-bounce, 0.189 multi-bounce**,
a 21 % reduction in error. Made by `tools/ao_preview.cpp`, which is the engine's `src/engine/ao.h` for the middle
panels and a small path tracer for the right one.*

## 1. The term

```
a = 2.0404 · albedo − 0.3324
b = −4.7951 · albedo + 0.6417
c = 2.7552 · albedo + 0.6903
visibility = max(ao, ((ao·a + b)·ao + c)·ao)
```

Per channel: `albedo` is a `vec3`, so the three coefficients and the result are too. That is where the colour comes
from — a red wall restores red light, a blue wall restores blue, and the grey seam between them stops looking like a
hole.

`src/engine/ao.h` is the definition: `multiBounceVisibility(ao, albedo)`, the three coefficient functions, and the
constants named so a test can cite them. `shaders/effects.frag` evaluates the same arithmetic; `tests/ao_test.cpp`
pins the properties.

## 2. Where it runs

```
src/engine/ao.h          the curve, on the CPU
shaders/normal.vert      now also passes UV through
shaders/normal.frag      now writes TWO targets: view-space normals, and the linear base colour
shaders/effects.frag     reads the albedo at Post binding 6 and applies the cubic to the AO term
src/engine/renderer.cpp  an albedo G-buffer target, a 1x1 transparent fallback, and one option
editor/src/main_window.cpp  "Ambient occlusion: multi-bounce (albedo-weighted)" in Screen Effects
```

The engine's AO is not applied in the material shaders; it is applied to the composed frame in `effects.frag`, because
that is where the SSAO estimate is made. So the term has to be read from a texture, and the texture has to have the
surface's albedo. The normal pre-pass is the pass that already draws every opaque surface with its material bound, so
it grew a second colour attachment and now writes base colour alongside normals. One extra `RGBA8` target and one extra
texture fetch, no extra geometry pass, and no change to any material shader.

**The clamp, and why it is a deliberate difference from Godot.** Godot applies the fit inside the forward pass, to
`ambient_light`, where a factor above the albedo's polynomial value is harmless: the surface also has direct sun, and
the ambient is a separate additive term. This engine multiplies the *composed frame* — sun and ambient together — so a
factor above 1 there would brighten the direct light as well. The result is therefore clamped into `[ao, 1]`:

* the lower bound is Godot's own `max(ao, …)` — the fit may never darken more than single-bounce AO asked for;
* the upper bound is this engine's — the approximation may restore light the linear term removed, never add light the
  frame did not have.

In practice only the upper clamp fires, and only for bright albedos: the polynomial peaks above 1 between `ao = 0` and
`ao = 1` for anything with an albedo above roughly 0.14. Clamping there is conservative and visible only as "the
corner is no longer black" rather than "the corner is now brighter than open sky", which is the right way to be wrong in
a post pass.

**Where there is no albedo.** Terrain, foliage, grass and Shader Graph materials are drawn outside the normal pre-pass.
Binding 6 is a 1x1 transparent texture there, and `effects.frag` falls back to single-bounce AO for those pixels, which
is what they had before this item. That is a limitation of where the albedo can come from, not of the fit, and it is
listed in §5.

## 3. What the tests check

`tests/ao_test.cpp`, 38 checks, no device:

| Group | What it pins |
| --- | --- |
| 1 | the coefficients are the paper's, and the fit passes through 1 at `ao = 1` |
| 2 | endpoints: unoccluded is unoccluded for every albedo; fully occluded is zero; a dark surface falls back to the linear term |
| 3 | over a 21 × 101 sweep: finite, non-decreasing in `ao`, never darker than single-bounce, never brighter than unoccluded, never negative |
| 4 | the feature itself: a brighter albedo restores more light, and the restoration is per channel (a warm wall returns warm light) |
| 5 | out-of-range input is clamped rather than propagated |
| 6 | one worked value the docs quote, so the numbers cannot drift silently |

The sweep in group 3 is the important one: it is the statement that the function is a *visibility*, not just a formula
that happens to produce plausible numbers.

## 4. Cost

The renderer gains one full-resolution `RGBA8` target (4 bytes a pixel, 8.3 MB at 1080p), one texture fetch per pixel
in the effects pass, and one extra output write per pixel in the normal pre-pass. There is no extra pass and no extra
geometry. The normal pre-pass only runs when a screen-space effect needs it, so a scene with AO off pays nothing.

The figure is 1600 × 900 with 128 visibility rays a pixel and 4× that for the tracer; it takes about a minute on two
cores, which is why CI renders it at 96 rays.

## 5. Limits

* **The albedo is a 2D buffer with no depth awareness.** A pixel whose G-buffer sample lands on a different surface (an
  edge) takes its albedo from the wrong material for a pixel or two. The AO estimate itself has the same edge problem,
  and both are small next to the effect.
* **Single-scatter albedo, no transmission.** The fit assumes a Lambertian surface; a leaf or a glass panel restores
  light it would not.
* **Not applied to terrain, foliage, grass or graph materials**, which are not in the normal pre-pass (§2).
* **It does not replace GI.** The real-time traced bounce (`docs/PATHTRACE.md`) computes actual incoming radiance; this
  is a curve that makes the *estimate* of occlusion less wrong. They compose: GI adds directional bounce, this fixes
  the ambient term's shape. Turning both on double-counts a little of the sky, which is why `multiBounceAO` is a
  separate switch.
* **The fit is a fit.** It matches a two-bounce reference to about 19 % luminance error in the figure's corner; a scene
  with more bounces or a glossy floor is further from it. The alternative is more traced rays, which is the GI path.
