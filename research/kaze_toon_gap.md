# Kaze UnityGenshinToonShader: gap analysis

Source: https://github.com/kaze-mio/UnityGenshinToonShader (MIT). Files read: `ToonInput.hlsl`, `ToonForwardPass.hlsl`, `ToonOutlinePass.hlsl`, `URPGenshinToon.shader`, `MaterialUpdater.cs`.

## Already in the engine

| Kaze feature | Engine equivalent |
| --- | --- |
| Light map (R/B spec, G AO, A material type) | ILM map, five material types (`NahidaHybrid`, `BodyAndHair`) |
| Shadow ramp | Ramp rows per material type (`REGION_RAMP`) |
| Day and night ramp | Day/night rows chosen from the light's Y (`isDay`). Kaze uses a manual toggle. |
| Emission | `emission` texture and `P_emissionStrength` |
| Normal map | Normal map input |
| Face shadow from an SDF light map | `NahidaFace` SDF shadow, `Face.shader` logic |
| Face direction from the head bone (`MaterialUpdater`) | `frame.toonD`, or the material's forward vector |
| Metallic and non-metallic specular | Spec ramp, matcap, `F0` specular |
| Outline, back-face hull | Inverted-hull outline, five colours by material type |
| Double-sided rendering | `doubleSided` material flag (same UV on back faces) |
| Rim light, fresnel | Facing-based fresnel rim (`P_rimPower`, `P_rimIntensity`) and a screen-space facing rim in the effects pass |

## Gaps

| # | Kaze feature | What is missing | Cost |
| --- | --- | --- | --- |
| 1 | Face blush (`_FaceBlushColor`, `_FaceBlushStrength`, base alpha as mask) | No blush in `NahidaFace` | Small: a material parameter and one lerp |
| 2 | Distance-scaled outline width (`_OutlineWidthParams`, fov-corrected) | Outline width is fixed in world units | Medium: vertex shader and two params |
| 3 | Outline Z offset and screen offset | Not present | Small |
| 4 | Manual day/night toggle (`_IsDay`) | Day/night is automatic from the light | Small: one param |
| 5 | Shadow colour, shadow offset, shadow smoothness | Shadow ramp has no adjustable colour or offset | Small to medium |
| 6 | Light direction multiplier, custom material type override | Not present | Small |
| 7 | Depth-edge rim (screen-depth discontinuity, times fresnel) | The engine rim is facing-based only | Medium: depth sampling in the effects pass |
| 8 | Vertex-colour AO (`input.color.r`) and vertex-alpha outline width | Graphics vertex format has no colour attribute | Large: loader and vertex-layout change |
| 9 | Back-face UV set (`TEXCOORD1`) for double-sided materials | Graphics vertex format has no second UV set | Large: loader and vertex-layout change |
| 10 | Kaze's shadow formula (half-Lambert × AO, with a step at AO 0.9) | Engine uses ILM ramp selection | Judgement call, not a clear gap |

## Status (gaps 1–9, implemented)

Gap 10 was never in scope (judgement call). Attribution: the feature list and formulas come from the MIT-licensed repo
(copyright 2023 风澪瑟, licence in the clone's `LICENSE`). The GLSL is our own. The shader build script says so in its header.

| # | Where | What landed | Default (no change) |
| --- | --- | --- | --- |
| 1 | `NahidaFace.body.glsl` | `faceBlushColor`, `faceBlushStrength`. Blush mask = `faceSdf.b` (see caveat) | strength 0 |
| 2 | `graph_outline.vert`, `graph_outline_map.vert` | `outlineDistance` (near, far) and `outlineFarWidth`: width lerps to the factor at the far distance | far ≤ near, or factor 0 = off |
| 3 | same two vertex shaders | `outlineZOffset` (0.01 world units, pushes the hull away from the camera), `outlineScreenOffset` (percent of clip position) | 0 |
| 4 | face and ILM (`EndfieldHybrid`, `NahidaHybrid`) | `dayOverride`: -1 follows the sun, 0..1 forces night..day | -1 |
| 5 | face: `faceShadowOffset`, `faceShadowSoftness`. ILM: `shadowOffset`, `shadowSoftness` | Shadow colour already existed (`shadowColor`). Offset moves the threshold, softness widens the edge | 0 / 0 face (hard step); 0 / 1 ILM (the original 0.2–0.4 edge) |
| 6 | face and hybrid | `lightDirMul` (vec3, scales the light before normalising). ILM `customMatType` (-1 = from ILM alpha) | (1,1,1) / -1 |
| 7 | `shaders/effects.frag`, `renderer.cpp`, editor | `depthEdgeRim` setting (0..1) in the pre-pass, `p1.w`. Lights a surface's silhouette where a neighbour 3 px out along the normal is much farther away or is sky | 0 |
| 8 | `rhi::Vertex`, all three backends, glTF loader, `graph.vert`, outline shaders | `color` (COLOR_0, RGBA, default white) and `vertexAOStrength` (multiplies occlusion by `color.r`). Outline width × `color.a` only when `outlineVertexAlpha` = 1 (slot 12). Off by default: `nahida.glb` stores COLOR_0 alpha 0 on every vertex, which would hide her outlines | Strength 0; alpha weight off; white colour = no change |
| 9 | face and hybrid | `useBackUV`: back faces read TEXCOORD_1 for all texture lookups | 0 |

Vertex layout: `Vertex` is 96 bytes (was 56). New attributes at locations 5 (colour), 6 (UV1) and 7 (smooth normal). Loaders fill
them from COLOR_0, TEXCOORD_1 and the load-time smooth normal. Meshes without them get white and the first UV set. Outline controls sit at material slots 8..11
in every outline material. Slot 1 (width) and slot 2 (alpha) are unchanged, and the engine's on/off check still reads them.

### Caveats (not verified)

- **Blush mask.** Superseded by the second pass: the mask is base texture alpha (as Kaze's `baseMap.a`), not `faceSdf.b`. Strength 0 keeps it off. Check on a real face before use.
- **Shadow offset and softness** act only on the ILM path (`NahidaHybrid`). The non-ILM Endfield path has no NoL shadow edge.
- **Editor build.** `editor/src/main_window.cpp` changes (the setting's UI, save and load, reset) are not compiled here: Qt6 is not installed.
- **Visual check.** No display here (no X server), so no screenshots of the new features. Compile checks and the test suites only.
- **Slot-numbered tests.** `endfield_shader_test` checks `NahidaHybrid` ramp rows at slots 15..19 (outline controls occupy 8..14). The test file already has this.
- **Asset check.** Only `assets/personal/nahida/nahida.glb` has COLOR_0 among the four glTF/VRM assets in `assets/`. Its alpha is 0, so the alpha weight must stay off for it.

## Second pass: deviations fixed

Each formula now follows Kaze's source. Where a change alters the existing look, it sits behind a setting that defaults to the old behaviour.

| Item | Now | Default |
| --- | --- | --- |
| Blush mask | Base texture alpha (`NahidaFace.body.glsl`) | strength 0 |
| Outline width | Kaze's law: `fovFactor = 2.414 / P11`, `z = abs(viewZ * fovFactor)`, range `outlineDistance` (o8.xy), factors `outlineWidthFactors` (o9.xy) | range (0,0) = off |
| Outline direction | Kaze's view-plane extrusion, with `outlineViewDirection` (o14.x) | 0 = the original world-normal extrusion |
| Screen offset | Raw clip units, as Kaze (no 0.01 factor) | 0 |
| Z offset | Kaze's `0.01 * zOffset * normalize(positionVS)` in view space | 0 |
| Smooth normal | New vertex attribute (location 7, `Vertex` now 96 bytes). Computed at load by averaging over shared positions. `outlineSmoothNormal` (o13.x) selects it | 0 |
| Vertex-alpha width | `outlineVertexAlpha` (o12.x) | 0 |
| Depth-edge rim | Kaze's `GetRim`: one-pixel offset along the normal's x, threshold 1 m, fresnel^5 | rim strength 0 |
| Shadow (gap 10) | Kaze's `GetShadow` (half-Lambert x AO, forced lit at AO 0.9, edge at 0.5 + offset), plus the shadow tint from `GetShadowColor`, behind `kazeShadow` | 0 |

Deliberate differences that remain:

- **Depth rim is scene-wide.** It runs in the effects pass, so it applies to every surface, not per material as in Kaze's forward pass. Its one-pixel offset and 1 m threshold are fixed Kaze defaults, not settings.
- **Shadow ramp rows** stay the engine's ILM rows. Kaze selects the row by a material index, which the engine does not use.
- **Smooth normal** is averaged at load, in object space. Kaze bakes it in tangent space.
- **Outline base width** stays in world units (`outlineWidth`). Kaze's `0.01 * _OutlineWidth` scaling is not used, but the factor law is.
- **Unity-only code** (`MaterialUpdater.cs`, Unity Volumes) is not ported. The engine already has the face direction through `frame.toonD`.

All of the above is compile-checked and passes the test suites. There's no visual check, and the editor is not compiled.

## Decisions: NahidaRenderProject (Jacinth-19, MIT, same author)

Reviewed the sample's shaders, scripts, materials and volume profile. Its shaders match Kaze's version we already ported.

| Item | Decision | Reason |
| --- | --- | --- |
| Depth rim per material (body, face, MMD face) | **Not done.** Kept the scene-wide depth rim (off) and the facing rim per material (body 0.18, face 0). | A per-material depth rim needs the material pass to read scene depth. That's a renderer ordering change, and the gain is a silhouette rim only. Revisit as a separate task. |
| Normal map on the body | **No change.** `NahidaBody` and `NahidaHair` already set `normalStrength` 1. The template default of 0 only affects materials that don't set it. | Correction to an earlier note, which said the body had no normal map by default. It does. |
| Emission on the body | **No change** (`emissionStrength` 0). | The sample enables the emission keyword but gives no level. Choosing one would be a guess, so this is an art call. |
| Face custom material type | **Kept** the fixed ramp row 5 (`NahidaFace` sets `faceRampRow` 5). | The sample doesn't store the custom type value, so there's nothing to port. The existing row is the closest match. |
| MMD face variant | **Not ported.** | The engine has no Nahida MMD face material in its asset set, and the MMD path is separate from the GLB character. If MMD Nahida is wanted, that's its own task. |
| Face outline Z offset 0.5 | **Done** (default of `NahidaFace`). | From the sample's `Nahida_Face.mat`. |

## Verification and state (2026-10-17)

Checked this turn. Earlier claims of "all nine gaps done" were not reliable. The status below is what has been checked.

| Check | Result |
| --- | --- |
| Full shader set through the build step (`tools/shaderc.py shaders`) | 42 of 42 compile, including `graph_outline.vert`, `graph_outline_map.vert`, `effects.frag`, `graph.vert` |
| `build_endfield.py` after the comment fix | EndfieldHybrid 81 props, NahidaHybrid 91, NahidaFace 30 |
| `endfield_shader_test` | 60 PASS, 0 FAIL |
| `endfield_scene_test` (earlier this session, after the ILM change) | 23 PASS, 0 FAIL |
| `endfield_gpu_test` (earlier this session) | 11 PASS, 0 FAIL |
| `systems_test`, `lod_gpu_test`, `particles_gpu_test` | pass |
| Editor compile | not done (Qt6 not checked here) |
| Visual check | not done for the toon features. The Nahida skirt change was checked against `Assets/Screenshots/front.png` only. |

Gap 10 (Kaze shadow) is now AO from ILM green (`param.g`) for Nahida, not ILM blue. The Nahida ILM is packed from the source lightmap (see `research/npr_pbr_check.md`). `kazeShadow` defaults to 0, so existing materials are unchanged. It has no visual check yet.

Open for the toon port: turn `kazeShadow` on for a Nahida material and check it visually; then decide whether it becomes the default. Editor compile when Qt6 is available.
