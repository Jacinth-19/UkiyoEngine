# NPR-PBR (Jacinth-19/NPR-PBR) vs the engine: implementation check

Checked 2026-10-17 against `/tmp/npr_src` (HEAD 94fffdc) and the engine's `tools/endfield/*.glsl`, `build_endfield.py`, `build_nahida_sets.py`, `src/engine/renderer.h`, `src/engine/model.cpp`, `editor/src/scene_file.*`. Source of truth for the repo side is its shader files and DEVLOG.md, not the README.

## Status by feature

| Repo feature | Engine status | Notes |
|---|---|---|
| Face SDF shadow, direction | Implemented | `NahidaFace.body.glsl` `mixSdf = mix(sdfRight, sdfLeft, exposeRight)`. Endfield: `sdfDirectionalRG = 1`, matching the repo's default. |
| Face acos clamp | Implemented | `clamp(dot(...), -1, 1)` in both face paths. |
| Face SDF MipMap off | Not verified | Mip setting for the face SDF texture not checked. |
| Face outline (inverted hull, `v.normal`, no smoothing) | Implemented | `NahidaFace` `outlineSmoothNormal` defaults to 0 and uses the raw normal. Z offset 0.5. |
| Body/hair outline with smooth normal | **Partly** | The engine has smooth normals (`smoothVertexNormals`) but `NahidaBody` and `NahidaHair` leave `outlineSmoothNormal` at 0, so they use the raw normal. The repo uses smooth normals for body and hair because raw normals open gaps at hard edges. |
| Smooth normal with area weighting | **Not implemented** | `smoothVertexNormals` averages unweighted. The repo weights by triangle area. |
| 5-colour outline from ILM.a | Implemented | Verified earlier (ramp row and outline colour from the alpha). |
| Edge light | Different choice | The repo's final rim is pure Fresnel (its depth-based rim was dropped after six failed attempts). The engine's facing rim matches that. The depth-edge rim is a separate, scene-wide Kaze-style option, default off. |
| ILM channels R/G/B on Nahida body | **Not implemented (wrong data)** | The Nahida packer maps only ILM alpha (`ilm.png` → `a`). `param` has no source for Nahida, so `param.png` is the Unity default (all 1.0). Result: metal = 0.25 flat, smoothness = 0.18 flat, AO not used, and `highlightHair.png` is all zero, so there is no highlight mask. The repo reads ILM R for metal, G for the shadow tone gate, B for the specular mask. |
| PBR path: NPR dark-side tone blend, shadow floor | Implemented | `pbrBaseBlend`, `shadowFloor`, `rampTone` in `EndfieldHybrid.body.glsl`. Not compared numerically. |
| PBR occlusion from ILM.b | Conflict | The repo found B is the highlight mask, not AO, and reduced its use to strength 0.12. The engine's `occlusion = mix(1, param.b, occlusionStrength)` uses B as AO with strength 0.2. For Nahida the param source is a default anyway, so this matters only for Endfield sets. |
| Realtime shadow jitter fix | Not implemented as settings | The repo changed Unity shadow resolution, normal bias and cascades. The engine has its own shadow path. Not checked. |
| Hair anisotropic highlight, matcap | Implemented | Hair anisotropy and matcap are in the Endfield body. |
| Rain preview | Implemented in shader | Wet props in the Endfield body (droplets, streaks, gloss, cloth mask). `endfield_hybrid_wet_vk.png` exists in `/tmp/endfield_out12`. No editor control, and no scene-file field. |
| Split NPR/PBR preview | Implemented in shader and renderer | Per-material split in the body (`splitLineDirX/Y`, `splitLineOffsetPx`). Renderer post pass `SplitScreen.shader` with tints and line. No editor menu. Not persisted in the scene file. |
| Runtime style switching | Partly | Only through material properties (`pbrBaseBlend`, `faceColorStabilize`) and the split. No editor UI or scene field. |
| Split controller (left-drag rotate, right-drag pan, R reset) | Not implemented | No controller in the editor. |

## Consequences for current work

1. **Toon port, gap 10 (Kaze shadow).** (Done: now reads `param.g`, see "Applied" below.) The earlier planned `kazeAo = param.b * v_color.r` was the wrong channel for Nahida. Kaze uses light map G as AO (`aoFactor = lightMap.g * input.color.r`), and the repo's own shader uses ILM G as the shadow tone gate. Nahida's `param.b` is a default, so the planned patch would read a constant. The patch should read ILM G, which needs the Nahida packer to map ILM to a slot that the shader reads.
2. **Nahida body data.** The packer's Nahida profile drops ILM R, G and B. Fixing it means mapping ILM into `param` for the Nahida profile and deciding how the shader reads it. That changes the look of every Nahida body material.
3. **Body outline.** Turning on `outlineSmoothNormal` for `NahidaBody` and `NahidaHair` matches the repo, but it is a visible look change. Area weighting is a separate change to `smoothVertexNormals`.

## Applied (2026-10-17, after the user chose "all three")

- `tools/endfield/build_nahida_sets.py`: body and hair `param` source = ILM copied as opaque RGBA (`rgb_copy`). Re-packed to `assets/personal/nahida/endfield/{body,hair,face}`. Verified: body `param.png` mean RGBA = ILM values (R 0.197, G 0.315, B 0.097, A 1).
- `tools/endfield/EndfieldHybrid.body.glsl` (inside `NAHIDA_ILM` only): Kaze shadow AO = `param.g * v_color.r` (was `param.b`); specular multiplied by `specMask = param.b` (highlight mask). Endfield path unchanged (`specMask` = 1).
- `tools/endfield/build_endfield.py`: `NAHIDA_PROPS` `occlusionStrength` default 0.12 (repo `_PBROcclusionStrength`).
- `assets/personal/nahida/endfield/NahidaBody.ukmat`, `NahidaHair.ukmat`: `outlineSmoothNormal` 1.0.
- `src/engine/model.cpp`: `areaWeightedOutlineNormals` (new, before `class Loader`). Outline normal only; `N` (lighting) unchanged. Falls back to the plain average where the weighted sum is degenerate.
- Backups before these edits: `/tmp/body_before.glsl`, `/tmp/bef_build.py`, `/tmp/bef_sets.py`, `/tmp/model_before.cpp`.

## Tests after the change

- `endfield_shader_test`: 60 PASS, 0 FAIL.
- `endfield_scene_test` (out `/tmp/endfield_out13`): 23 PASS, 0 FAIL.
- `endfield_gpu_test` (packed dir `assets/personal/nahida/endfield/body`): 11 PASS, 0 FAIL.
- `systems_test`, `lod_gpu_test`, `particles_gpu_test`: pass.

## Visual check

`/tmp/endfield_out13/nahida_endfield_vk.png` compared with `/tmp/endfield_out12/nahida_endfield_vk.png` (before). The skirt moved from green to white with green gem panels. The repo's `Assets/Screenshots/front.png` shows a white skirt with green gem panels; its green petal cape is at the back (`back.png`). The new render matches the reference front view better. The screenshots do not record which render mode they were captured in, so this is a judgement from the front view only. Decision: kept.

## Still open

- Smooth-normal outlines and area weighting have no separate visual check yet. They changed outline geometry only.
- Split and rain have no editor controls, and the scene file does not store them.
- Runtime style switching is only through material properties.
