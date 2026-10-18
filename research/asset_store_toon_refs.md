# Asset Store reference: ZLZ Anime Shader and Anime Shading Plus

Sources (store pages only, read 2026-10-14):
- ZLZ Anime Shader (ZLZ Studio, v2.1.0, URP only): https://assetstore.unity.com/packages/vfx/shaders/zlz-anime-shader-354900
- Anime Shading Plus (ASP, Eric Hu, v1.6.0, URP only): https://assetstore.unity.com/packages/vfx/shaders/anime-shading-plus-281737

Rule for this note: these are commercial packages. We use the feature ideas and write our own code. We do not copy shaders, scripts, textures, or the sample models. Store pages do not describe algorithms, so every method below is still to be researched on its own. The ASP and ZLZ documentation sites are the next place to look for method detail.

## What the pages say

### ZLZ Anime Shader
- Headline: transparent hair so the face stays readable. Also depth-based contact shadow for hair and character details.
- Shading: base and shadow colour, toon ramp (sharpness and smooth transitions), shadow edge, face shadow ("without UV concerns"), realtime shadow receive, main and additional lights, normal map, specular, metallic, hair highlight, soft light, rim light, emissive, transparency, light sweep.
- Outlines: hull outline and screen-space outline, both with thickness and colour.
- Tone mapping for anime, cartoon, and cinematic looks.
- Runtime FX: outline on selection, dither camera fade, dither occlusion fade, dissolve, target darken, indicator, get-hit, upgrade.
- Tools: character dashboard, shader cost and texture-usage analyser, texture mask packer.
- Performance: features are switchable and disabled features are stripped at compile time. The page says this removes their cost.

### Anime Shading Plus (ASP)
- Three character styles: cel shading, stylised PBR, SDF face.
- Cel: ramp texture plus an extra ramp layer for subsurface-like effects. Three ways to colour received shadow. Flatten received GI and additional light. Specular with falloff colour, stylised BRDF highlight, rim, depth-based rim, separate hair-highlight map, matcap. An option to remove ugly self-shadow on cel characters.
- Shadows: a "Simple AABB cube-proxy" self-shadow remover (recommended), depth-offset shadows, and a legacy character-only shadow map on older Unity versions.
- Dithering for pseudo-transparency. Perspective FOV adjustment.
- Outlines: mesh-based inverted hull, and a screen-space outline with material, albedo, depth, and normal edge detection.
- Eye shader: parallax pupil, matcap, separate highlight map, option to flatten lighting.
- Post: anime tone mapping with Filmic and Khronos PBR Neutral, with options to keep character colours.
- Editor tools: SDF face-shadow map baking, smooth-normal baking for hull outlines, per-character lighting controls, a custom shader GUI.
- Render Graph support. MotionVectors and XR motion-vector passes for TAA, motion blur, and ASW.
- Limits stated: no GPU instancing, no LOD cross-fade, SRP Batcher supported, character shaders only, some effects not with transparent materials.

## Mapping to our threads

| Idea | Where it appears | Our status | Action |
|---|---|---|---|
| Face SDF shadow | ASP face style, bake tool; ZLZ face shadow | Done (face SDF, Toon material) | Compare our bake against ASP's stated approach. Their tool is a baked map, the same family as ours. |
| Smooth-normal bake for hull outline | ASP tool | RealToon list item "smooth object normal" (open) | Confirms the item. Bake at import time. |
| Perspective FOV adjustment | ASP, both | RealToon list item "perspective adjustment" (open, third in order) | Use as a reference for the feature. |
| Depth-based rim | ASP | Screen rim (done, milestone 2) | Check whether our rim has a depth gate. |
| Separate hair highlight map | ASP | Not on our list | Add as a texture slot later. |
| Matcap reflection | ASP eye and cel | Reflection texture item uses a procedural sky (user's decision) | Matcap is a different input. Treat as a separate option, not a replacement. |
| Hair transparency for face clarity | ZLZ headline | Not on our list | Candidate. Needs a decision: cut out or alpha blend. |
| Cube-proxy self-shadow removal | ASP recommended | Not on our list (self shadow is in RealToon's list, not built) | Candidate for the self-shadow item. Method to research. |
| Depth-offset shadow | ASP | Not on our list | Candidate, same family as the self-shadow item. |
| Flatten GI and additional light | ASP | Partly covered by the toon lighting | Check against our Toon path before adding. |
| Filmic and PBR Neutral tone map, keep character colours | ASP | Already in `research/tonemappers.md` | Check the list against that note. |
| Motion vectors pass | ASP | Smear uses hips displacement (open) | A motion-vector buffer would replace our per-object velocity. Worth a decision before smear work. |
| Compile-time feature stripping | ZLZ | Watercolour performance (open, first target) | Use compile-time variants for the expensive watercolour branches. Measure cost first (GPU timing not available here). |
| Shader cost analyser | ZLZ tool | Not on our list | Our `ukiyo_shader_tool` could report texture fetches per shader. |
| Eye shader (parallax pupil) | ASP | Not on our list | Out of scope for now. |
| Dither camera fade | ZLZ (camera and occlusion fade), ASP (dithering) | RealToon listing includes near-fade dithering. Build status is not confirmed in this note. | Check `research/realtoon.md` before planning it. |

## Open questions for the user

1. Hair transparency: do we want it? It changes how hair draws over the face (alpha blend or cutout), which affects the sorting and the smear pass.
2. Motion vectors: switch the smear velocity from hips displacement to a real motion-vector buffer? It is more work, but it works for any moving surface.

## Next research (not started)

- ASP documentation (erichu33.github.io/ASPDocs): face SDF bake, cube-proxy self-shadow, outline details. Method takeaways only.
- ZLZ feature docs (zlz-studio.github.io/features): hair transparency and contact shadow.

## Implementation status (2026-10-14)

Items from the ZLZ feature list, against this engine. "Existing" means it was already in the engine before this pass.

| Item | Status | Where / notes |
|---|---|---|
| Transparent hair for face clarity | Not built | Needs a face anchor per character and a hair flag. Decide dither or alpha. |
| Depth-based contact shadow for hair | Not built | Plan: screen-space contact shadow in `effects.frag` from the depth buffer. |
| Face shadow without UVs | Partial | Face SDF is UV-based (`faceShadowLit`). A world-space projection of the head frame is not built. |
| Base and shadow colour, toon ramp, shadow edge | Existing | `mtoon.frag`, `mmdtoon.frag` |
| Realtime shadow receive | Existing | `shadowFactor` |
| Main and additional lights | Existing | Main light plus 4 point lights, toon-banded |
| Normal map | Existing | Stage 3 |
| Specular | Existing | Toon gloss |
| Metallic | Not built | `mtoon` has no metallic term. |
| Hair highlight | Existing | Anisotropic band |
| Soft light | Built this pass | `toonSoftSweep`, wrap term. Project setting `softLight`. Selftest 11y. |
| Rim light | Existing | Parametric rim plus screen-space rim |
| Emissive | Existing | `mtl.emissive` |
| Transparency | Existing | Alpha modes 0/1/2 |
| Light sweep | Built this pass | `toonSoftSweep`, a band climbs 0 to 2 m. Project setting. Global, not per character. Selftest 11y (weak: 0.003 mean change). |
| Hull outline (colour, thickness) | Existing | Inverted hull per material |
| Screen-space outline (colour, thickness) | Existing | Depth-normal outline, scene-wide |
| Tone mapping (anime, cartoon, cinematic) | Existing (operator level) | ACES, PBR Neutral, AgX, Custom Anime, Hable in `tonemap.frag`. The look presets were not checked in this pass. |
| Outline on selection | Not built | Needs a per-entity ID buffer. |
| Dither camera fade | Built this pass | `toonDitherFade` (4x4 Bayer). Project settings `ditherFadeNear` / `ditherFadeFar`. Selftest 11y. |
| Dither occlusion fade | Not built | Needs occluder detection. |
| Dissolve | Partial | Dissolve Shader Graph presets exist (`tools/shader_graphs/Dissolve_*.json`). No per-character runtime amount. |
| Target darken | Not built | Per-entity. |
| Indicator | Not built | Needs geometry or a sprite. |
| Get hit | Not built | Per-entity flash. Physics has hit events, but no visual path. |
| Upgrade | Not built | Effect. |
| Character dashboard | Not built | Editor UI. |
| Shader cost and texture-usage analyser | Not built | Could extend `tools/ukiyo_shader_tool.cpp`. |
| Texture mask packer | Not built | Tool. |
| Features can be switched off and stripped at compile time | Partial | Runtime flags only. Not compile-time variants. |
| Material presets (hair, skin, special), one-click | Not built | Shader Graph samples exist. No preset manager. |

Per-character runtime effects (dissolve amount, target darken, get hit, indicator, upgrade, outline on selection) all need per-instance GPU data. The push-constant block is full at 128 bytes, so they cannot go there.

## Batch: eye shader and stylised PBR (2026-10-14, v1)

- **Stylised PBR** (`toonPbr`, both toon shaders): a GGX highlight banded into a hard disc, tinted by F0 (0.04, or the albedo for metal). Uses roughness and metallic from the metallic-roughness texture when present. Project setting `pbrStyle`. Selftest 11z: changes the frame (0.027). The look has not been checked by eye.
- **Eye material**: detected by name ("eye" or "iris", not "brow"); flag in `toon2.w`.
  - Parallax pupil (`toonBaseUV`): a UV shift in the tangent plane. No height map, so it approximates depth and assumes the iris is centred in the texture. Setting `eyeParallax`.
  - Flatten lighting (`toonEyeFlatten`): blends toward the albedo at full sun. Setting `eyeFlatten`.
  - Matcap and highlight map (`toonEyeAdd`): wired to material bindings 9 and 10, switched on by `flags2.z` / `flags2.w`. Nothing sets those flags yet, because no matcap or highlight texture is assigned from the file or the editor. So these two are inactive.
- Selftest 11z: the eye terms change the frame (flatten 0.0002, parallax 0.0037 on the test view, so they are small there). Visual check pending: the headless screenshot did not finish in this sandbox.
- Not built from the ASP list: ramp texture, extra subsurface ramp, three shadow-colouring modes, flatten GI and additional light, specular falloff colour, depth-based rim per material, separate hair highlight map, AABB cube-proxy self-shadow removal, depth-offset shadows, material-based dithering, perspective FOV, screen outline material/albedo edges, per-character lighting controls, Render Graph.
- Per-character data decision recorded: material copies per character (not yet used; no per-character effect built in this batch).
