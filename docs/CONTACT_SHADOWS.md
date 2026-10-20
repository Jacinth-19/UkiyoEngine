# Screen-space contact shadows (item 29)

A cascaded shadow map answers "is this point lit?" at its own resolution. Where two surfaces meet — a crate on a floor, a
prop on a shelf, a foot on the ground — the map's texels are much wider than the gap, so the contact is either smeared
into a step or lost entirely and the object reads as floating. Screen-space contact shadows fix exactly that scale: from
the visible surface, march a few centimetres towards the sun through the depth buffer that is already bound, and darken
the pixels where geometry sits just in front of the ray.

Godot 4.8 added this as GH-118045, an option on `Environment` and on `DirectionalLight3D`, using the Bend Studio
approach (*Inside Bend: Screen Space Shadows*). Ukiyo implements the same depth test in `src/engine/contact_shadow.h`
and `shaders/effects.frag`; it does not need a per-light render target, a separate dispatch or a blur pass, because the
effects pass already holds the depth and view-space normals.

![Contact shadows against a shadow map that cannot resolve the contact](contact-shadows.png)

*A crate and three props on a floor under a 36° sun. **Top left:** a 32 × 32 shadow map over a 4.4 m frustum — one texel
is 13.8 cm, which forces 20 cm of depth bias; the props cast square blobs and the crate's shadow pulls away from its
base. **Top right:** the contact pass alone: the depth-buffer march draws a dark contact under every caster, with no cast
shadow at all. **Bottom left:** both, which is a frame. **Bottom right:** what the figure shows. Each panel carries a 2×
crop of the same contact. In the frame the pass darkens **86 % of the 14 034-pixel contact band** and touches **0.43 %**
of the lit pixels (darkened by more than 10 % when the sun actually reaches them). Made by `tools/contact_shadow_preview.cpp`,
which runs the engine's own `contactShadowVisibility`; the shadow map and the ground truth are ray-cast.*

## 1. The march

```
steps       = 24                       // depth fetches per pixel
stepLength  = length / steps            // length = 0.35 m
jitter      = interleavedGradientNoise(gl_FragCoord.xy)
travelled   = stepLength * (0.5 + jitter)
for i in 0..steps:
    ray        = P + L * travelled      // L is towards the sun, in view space
    uv         = project(ray)
    sceneDepth = depth buffer at uv
    gap        = rayDepth - sceneDepth  // positive: the buffer is in front of the ray
    if gap > bias(travelled) and gap < thickness: occluded += 1
    travelled += stepLength
hit = sqrt(clamp(occluded / steps * 4, 0, 1))
visibility = 1 - clamp(strength * hit * edgeFade, 0, 1)
```

`bias` starts at 1.5 cm and grows with distance; `thickness` is 8 cm by default. Both are metres, and both matter:

- **Bias** keeps the pixel's own surface out of the count. The ray starts half a step out and lifts a little, but a
  depth buffer is quantised, and without the bias a flat floor shadows itself in stripes.
- **Thickness** is what makes this a *contact* shadow rather than a second shadow map. A sample only counts when the
  occluder is within 8 cm of the ray, so a wall 2 m away does not cast; the pass cannot and should not reproduce a
  cascade. Thick occluders — a crate, a wall — need it raised (the figure uses 0.30 m so the crate's face counts), thin
  ones — cloth, foliage, a rail — keep it low.

One hit in 24 is a ray that grazed something, so the raw count is shaped by `sqrt(x * 4)`: the near edge of a contact is
soft and the deep contact goes solid, which is what a penumbra looks like at this scale. The per-pixel jitter trades
banding for noise, and TAA resolves it across frames exactly as it does for SSAO.

## 2. Why per pixel, and not a compute pass

Godot's version writes a per-light R8 target from a compute shader, organises 64-thread workgroups along epipolar lines
in light space, and blurs the result. That is the right shape for a renderer that must produce a shadow *texture* for
the light's own sampling. This engine's effects pass already has the depth buffer and the view-space normals bound, and
applies its screen-space terms to the composed frame in one place. The same depth test therefore costs one loop in the
pixel shader:

```
length: 24 depth fetches, only on pixels whose normal faces the sun (the dot product early-outs the rest)
bandwidth: one fetch per step, no render target, no resolve, no blur
```

The trade is that the pass runs per pixel rather than per shadow-map texel, so it is not free at 4K; the step count is
the knob (12 is enough for a still at 1080p, 48 removes the last banding on a grazing surface).

### Reversed-Z

The engine's depth buffer is reverse-Z (`0` at the far plane, `1` at the near plane), and `eyeDist` in `effects.frag`
converts a sampled depth to metres before the comparison. The CPU header takes eye depth in metres directly, which is
why `tests/contact_shadow_test.cpp` can feed it a ray-cast depth buffer and check the arithmetic without a device.

## 3. Where it runs

```
src/engine/contact_shadow.h   the parameters and the march, on the CPU
shaders/effects.frag          the same march in GLSL, applied after the ambient terms and before SSR
src/engine/renderer.cpp       turns the normal pre-pass on when the option is above zero; uploads four numbers
src/rhi/vk_device.cpp         one uniform buffer at the effects pass's set, binding 7
editor/src/main_window.cpp    "Contact shadows" plus length, thickness and steps in Screen Effects
tests/contact_shadow_test.cpp the properties, on an analytic scene
```

The four numbers do not fit in the effects pass's push block, which is already full, and the pass binds only its own set,
so they ride in a 16-byte uniform block at **binding 7**. Textures stop at binding 6 on purpose: a pass that carries both
the Frame and Post sets binds the Frame shadow atlas on GL unit 1 in the GL backend, which is where Post texture 1 lives.

## 4. What the test pins

`tests/contact_shadow_test.cpp` builds a scene that can be solved on paper — a floor at `y = 0` and a unit cube at the
origin, seen by a perspective camera, with the depth buffer produced by ray casting — and checks, in 26 assertions:

1. the floor just outside the cube on the shadow side is darkened;
2. a lit floor point beyond the march is left alone, and a genuinely shadowed point 1 m from the caster is left alone
   at the default 0.35 m length;
3. lengthening the ray reaches a shadow that was outside it, monotonically;
4. the strength and the length knobs move the result in the direction they claim;
5. a surface facing away from the sun is never darkened (the early-out);
6. the jitter is finite and in `[0, 1)` for every pixel, including the largest coordinates the shader will see;
7. degenerate settings — zero length, zero thickness, zero steps, zero strength — return exactly 1.

The figure's tool is the second half of the check: it runs the same function over a scene it can also solve exactly, and
counts the pixels the answer disagrees with.

## 5. Limits

- **Occluders off screen do not count.** The march reads the visible depth buffer only, so a caster just outside the
  frame cannot shadow the pixel; the 8 % border fade hides the worst of it.
- **Thin geometry in front of a thick one** needs `thickness` above the thin object's depth, or the sample is rejected
  as "too far behind". The default is tuned for props and set dressing, not for cloth.
- **No sun, no pass.** The march is along one direction (the directional light's). Point and spot lights would each need
  their own march; Godot's version has the same restriction.
