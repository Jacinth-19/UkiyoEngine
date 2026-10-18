# RealToon extras: plan and status

Source: Unity Asset Store listing "RealToon (Pro Anime/Toon Shader)" (fetched 2026-10-13). This is a
Unity-only commercial shader. We build our own versions of the features, with no code from the package.

## What the engine already has
- `shaders/mtoon.frag`: MToon-style shade ramp (shading shift, toony), shadow receive, hemisphere ambient,
  point lights banded the same way, parametric rim with a lighting mix, emission, and an outline.
- `shaders/mmdtoon.frag`: MMD toon shading (ported from a reference effect).
- Material parameters come from VRM/MToon data in `src/engine/model.cpp`. Shader Graph materials are `.ukmat`.

## Feature list and status
| Feature (listing) | Status | Notes |
|---|---|---|
| SDF face shadow | done, material side (`--face-sdf`, `--face-material`) | Nine-angle SDF atlas from `tools/make_face_sdf.py`, binding 2, flag `toon2.z`, head forward in `toonD`. Tested with uniform atlases and on `backpack_nm`; the boundary sweep is not visible on the project characters (see note). |
| Anisotropic hair highlight ("angel ring") | done, stage 1 | Kajiya-Kay band on a strand tangent from UV derivatives. Shift and sharpness are settings. Both toon shaders. |
| Gloss (toon highlight) | done, stage 1, texture stage 3 | Procedural gloss from the half-vector. Gloss texture (binding 7) = 1 - G of the glTF metallic-roughness texture, multiplies the gloss. No project asset has that texture, so it is not checked visually. |
| Self shadow (threshold, hardness, no texture) | done, stage 1 | Soft threshold on N.L. Both toon shaders. |
| Smooth object normal | done, stage 3 | Load-time: normals of vertices sharing a position are averaged per primitive. Project default `smoothNormals` (`--smooth-normals`); takes effect when a model loads. Also closes the inverted-hull outline. |
| Normal map (detail or override) | done, stage 3 | Uses a material's glTF normal texture (binding 3). Tangent frame from UV derivatives, no tangent attribute. Project default `normalMap` strength (0 = off). Under the soft toon ramp the change is small; it shows mainly at the shade boundary. Override mode not done. |
| Reflection (not matcap) | done, procedural sky, MToon only | `--reflect` strength, project default `toonC.w`. No reflection texture slot yet. |
| Outline: colour, width, noise (sketch) | done, stage 3 | Colour and width exist. Noise on the depth-normal outline (`outlineNoise`) and on the inverted-hull outline (`outlineMatNoise`, project default, 3 px dashes). |
| Screen-space rim light | done, stage 2 (scene-wide) | Post pass in `effects.frag` from the normal buffer, cool edge on surfaces turned from the camera (`screenRim`). Scene-wide, not per material. |
| Depth-normal Sobel outline | done, stage 2 (scene-wide) | Normal buffer from `normal.vert/frag`, depth and normal edges in `effects.frag` (`depthNormalOutline`). Shared with the PP work. Per-material toggle not done. |
| Smear effect | done, cel edge stretch along motion | `--smear S` (project default `toonD.w`). Per-model hips displacement per render call, in `pc.p1.xyz`. Smear shift = S × clamp(speed / 0.02 m per frame) × N·dir, added to N·L in `mtoon.frag`. MToon only. Headless: `--sway 0.3 --frames 3` changes 1,709 px (max 75); static model 0 px. |
| Perspective adjustment (3D to 2D) | not started | Vertex-side projection blend. |
| Refraction (glass, ice) | not started | Needs a scene colour copy. |
| Shadow texture (gradient or flat) | partly | `uShadeTex` exists. |
| Graph "Toon Master" node | done | `ToonMaster` master in `sg_nodes.cpp`; codegen branch in `sg_codegen.cpp` (two-tone sun ramp + hemisphere ambient). Sample `assets/Shaders/Toon.ukshader` and `Materials/Toon.ukmat`. Rendered with `ukiyo_shader_tool sphere`. |

## Where the settings live
Project defaults, chosen by the user. They are set on the Graphics page under "Toon Extras (RealToon)".
They are written to the scene document only when a value is changed from its default, so older scenes are unchanged.
They reach every toon material through the frame block (`frame.toonA`, `frame.toonB`).

## Stage 1 (done)
- Settings and frame block: `FrameUBO.toonA/toonB`, `shaders/frame.glsl`, `RenderSettings`, Graphics page, CLI flags (`--hair-highlight`, `--hair-shift`, `--hair-exponent`, `--self-shadow`, `--self-threshold`, `--self-hardness`, `--gloss`, `--gloss-power`).
- `shaders/toon_extras.glsl`: shared by `mtoon.frag` and `mmdtoon.frag`. The project characters use the MMD path by default, so both are needed.
- Selftest 11u: hair (diff 1.76), self shadow (1.92), gloss (0.10), and the plain frame after the extras are off again (0.000).
- Visual check: the effects are measurable but subtle on Alicia at 640 px. Stronger settings are needed for a clear look.

## Stage 2 (milestone 2, partly done)
Done:
- Screen-space rim light (scene-wide post, from the normal buffer). Selftest 11w: diff 0.63 headless.
- Depth-normal outline (scene-wide post). Selftest 11v.
- Outline noise on the depth-normal outline (dashes). Selftest 11w: diff 0.31 headless against the plain outline.
- Settings on the Graphics page ("Toon Extras (RealToon)" and "Screen Effects"), save and load, CLI
  (`--screen-rim`, `--outline-noise`, `--outline-dn`).

Not started (stage 3):
- SDF face shadow: the method is re-implemented from the public description (nine-angle SDF, 128 ± 127·d/64). Not checked on a lit head: both project characters mark every material `KHR_materials_unlit`, so the toon light test never runs on their faces.
- Unlit flag (user choice): `KHR_materials_unlit` is ignored on MToon materials (VRM 1.0 `VRMC_materials_mtoon`, VRM 0.x `VRM/MToon` shader). Both project characters now render with toon lighting, which changes their default look. MMD materials are unchanged.
- Reflection (user choice: procedural sky): `toonReflection` in `toon_extras.glsl`, sky/ground gradient by reflection direction × Fresnel × gloss mask. Strength is the project default `toonC.w` (`--reflect`, 0 = off). MToon only. Headless: Seed-san at 0.6 changes 13,349 px (max 96); Alicia 8,559 px.
- Gloss texture and a normal map (new material texture slots).
- Smooth object normals (needs a smoothed-normal bake or a vertex mask).
- Reflection texture slot (a per-material mask), not built; the sky term is in.
- Material-side outline noise and a per-material outline toggle.
- Smear: the displacement is the hips' movement between render calls, not per-part motion, and it is not in MMD materials. Perspective adjustment (3D to 2D) is still open.
- Perspective adjustment (3D to 2D). It changes vertex positions, so every depth pass
  (normal, velocity, main) must share the transform or the post effects will misalign.
- Refraction (needs a copy of the scene colour).
- The Toon Master node is built (see table). Not yet: a graph-to-scene path for the Toon sample (the engine's MToon materials stay the scene path), and the Toon Master has no rim or extras inputs yet.

Limits of the stage 2 work:
- Screen-space rim and the depth-normal outline apply to every surface, not only toon materials.
- Outline noise is a screen-space hash, not the material's own noise pattern.

## Stage 3 (in progress, material side first)
Done in this pass: normal map, gloss texture (code path, unverified visually), material outline noise, smooth object normals.
Selftest 11x: the normal map changes the frame (mean 0.024; the toon ramp is soft, so the change is small).
Material outline noise: headless `ukiyo --outline-mat-noise 1` changes 3302 px of the Seed-san frame (mean 0.28).
It is not yet in the editor selftest: with the editor's view and a thick outline the frame did not change, and the cause is not found.
Smooth normals on Seed-san: 729 px change, no visible artefacts in the front view.

Still not started:
- Per-material outline toggle for the depth-normal outline (needs an object-ID buffer; the inverted hull is already per material).
- SDF face shadow, reflection texture (not matcap), smear, perspective adjustment, refraction, and the Toon Master node in the Shader Graph (done).

