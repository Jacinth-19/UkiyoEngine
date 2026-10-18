# NPR character shader survey (personal test source, not for shipping)

Source: the NPR clone's `Assets/charactors/*/shaders/`. Only two character folders have shaders:
`zhuangfy` (Endfield) and `nahida`. Personal use only. Keep derived shaders and textures out of commits.

## Done
- **zhuangfy: EndfieldHybrid / EndfieldDebug.** Ported as our own GLSL (`tools/endfield/`). Packed to
  8 texture slots (`pack_endfield.py`). Outline pass added (graph outline pipeline).

## Nahida
| Shader | What it does | Ported? | Gap |
|---|---|---|---|
| `BodyAndHair.shader` | NPR ramp with a row chosen per pixel by the ILM alpha (five material types); ILM green sets shadow hardness; five outline colours chosen by ILM alpha; PBR/NPR blend; sphere and metal maps; normal map | Partly: ramp, PBR blend, matcap, rim and outline are in the Hybrid | No per-pixel ramp-row select (needs the ILM alpha, which the packed layout doesn't have a free channel for); no five-colour outline; the ILM lightmap isn't used. The Nahida test uses a white default ramp, so banding is flat. |
| `Face.shader` | SDF face shadow turned by a per-frame forward and right vector (set by a script); ShadowTex modulates SDF; outline needs alpha 1 | Yes, through the Hybrid's face SDF, using the engine's `frame.toonD` as the head direction | Not wired to Nahida's face in the test (the face set has no FaceLightmap). Face outline colour not split from body. |
| `SplitScreen.shader` | Full-screen post pass: a split line between an NPR and a PBR side, with tints | Partly: the Hybrid has per-material split props | A real split needs a post pass in the renderer; not done. |

## Next, if wanted
1. Per-pixel ramp row from the ILM alpha (needs one more texture or a packed channel).
2. Face shader on Nahida's FaceLightmap (SDF and ShadowTex).
3. Screen-space split as a post pass.
