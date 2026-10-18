# Per-object motion vectors (step 6b): implemented

Status: implemented and built (Vulkan and GL). Verified as described under "Verification" below.

## As built

- `shaders/velocity.vert` / `velocity.frag`: draw the opaque primitives again against the scene depth
  (`clearDepth = false`, depth test on, depth write off). The clip position is `frame.viewProj * world`,
  the same jittered transform as the main pass, so depths match. The previous clip position uses last
  frame's unjittered `prevViewProj` and last frame's transform. Output is `(cur - prev) * 0.5` in UV units.
- Previous transforms are a per-draw UBO in a new set type `SetType::Motion` (set 3, binding 10):
  `MotionUBO { mat4 joints[128]; mat4 prevViewProj; }`. Rigid draws use `joints[0]` as the previous model.
  Skinned draws use the previous joint palette, which the renderer computes on the CPU as
  `nodes[joint].world * inverseBind` (the same formula as `model.cpp`). Push constants are unchanged.
- Slots are keyed by (model, node) for rigid draws and (model, skin) for skinned draws, in `Renderer::motion_`.
  Each frame: prepare uploads, draw, then copy `current` to `last`. The history is cleared when TAA resets.
- `Post` set binding 5 carries the velocity texture into `taa.frag` (`uVel`). Where the velocity is
  non-zero it gives `prevUV = uv - vel`; elsewhere it uses the camera reprojection, as before.
- Vulkan: `SetType::Motion` appended (index 5); Post layout gains binding 5.

## Verification

- Debug render (temporary, reverted): the velocity buffer is zero on the background and non-zero on the
  character. Non-uniform colour across the character shows per-part motion (gun, sleeve, cord).
- Headless 640x640, aa 4, 16 frames, Vulkan: pre-change TAA RMSE vs 2x SSAA 9.784; with velocity 9.738.
  The idle pose moves little in 16 frames, so this test is weak. Only about 1,900 pixels changed.
- GL vs Vulkan with the velocity build: RMSE 2.42 (backends agree).
- Not yet tested: a fast-moving character (no CLI control of the animation speed); mask/alpha-cut
  primitives are drawn without the discard (their velocity can appear in holes); graph materials
  (ShaderMaterial) have no velocity.

## Original design notes (superseded in places)

## Current state (verified in source)

- Push constants: `PushConstants` = model (64 B) + p0 (16 B) + p1 (16 B) = 96 B. The Vulkan
  budget is 128 B (`kPushConstantBytes`, `gpu_types.h`). A second mat4 for the previous model does
  not fit.
- Rigid draws use `r.model->nodes[r.node].world` as the model matrix (`renderer.cpp` drawWith).
- Skinned draws use an identity model and the skin's joint UBO (`Skin::ubo`, `Skin::set`,
  `model.h`). The previous frame's joint palette is not kept anywhere.
- TAA (`shaders/taa.frag`) reprojects with a camera-only matrix (`pc.model`). It has no per-pixel
  velocity input, so moving characters ghost.

## Proposed design

1. Velocity target: RG16F, same size as the HDR target, written by a new velocity pass (or a
   second colour output of the opaque pass).
2. Previous transforms in a table, not push constants. Store each draw's previous model matrix
   in a per-frame array (UBO or SSBO), indexed by a draw id passed in `p1.x`. The velocity pass
   reads the current model from `pc.model` and the previous one from the table.
3. Previous skin palettes: keep a CPU copy of each skin's joint matrices from the last frame, and
   upload them into a second joint UBO per skin. The velocity shader reads the previous palette
   for skinned vertices and the current palette for the current position.
4. Static geometry with no movement: previous equals current, so velocity is zero and TAA falls
   back to the camera reprojection.
5. TAA: sample velocity at the pixel, reproject with `uv - velocity`, keep the current clamp and
   rejection. Use camera reprojection only where velocity is unavailable.
6. Backends: the velocity pass needs GL and Vulkan shaders (`tools/shaderc.py` builds both).

## Test plan

- Self-test: a static scene has near-zero velocity everywhere. A rotating or moving skinned
  model has velocity on its moving pixels that matches a finite-difference of the projected
  positions (within 0.05 px).
- Headless: render a moving character with TAA on and off. Compare the TAA error on moving
  pixels against no-AA, and check for a ghost trail (mean history difference on moving pixels).
- Both backends (`--backend gl` and default Vulkan) must agree within 1 level.

## Cost and risk

Multi-file change: renderer, model (previous palette), gpu types, shaders (velocity, TAA), both
RHI backends. Not started in this pass. The table approach avoids the push constant limit but needs
a size limit on the draw count, so the table must grow or be chunked.
