# Review: the PP.zip shader set (RavenEngine)

Source: `uploads/PP.zip`, extracted to `uploads/PP/PP/`. 15 files: `scene.vert/frag` (PBR with a cel option),
`shadow.vert/frag`, `sky.vert/frag`, `common.glsl`, compute passes `temporal`, `bloom`, `exposure`, `post`,
`composite`, and three 32-cube `.cube` LUTs (Eclipse, Neutral, Porcelain).

Licence: the `.cube` headers say "RavenEngine original color transform; MIT license". The shader files
have no licence notice. Treat the shaders as reference. Re-implement the ideas, and do not copy code
until the licence is confirmed.

Note: `uploads/image-1.png` is a screenshot of a different program (a "World Editor" with a match-3 gem
game). It is not part of this shader set.

## What we already have
- TAA (`taa.frag`), velocity buffer, bloom, tonemap with vignette, grain and a 256x16 LUT strip.
- Sky (`sky.frag`), grid, outlines, and the Stylize pass (`stylize.frag`).

## What is worth taking
| Idea (file) | What it does | What it needs here | Effort | Note |
|---|---|---|---|---|
| `.cube` LUT loader | 32-cube 3D grading LUT, three looks | A `.cube` parser and a 3D texture. We have a strip loader only. | Low | Three ready looks. Check MIT before use. |
| Depth fog with sun in-scatter (`composite.comp`) | Height-based density and sky-coloured fog | Depth and `invViewProj`, both already in the frame | Low | Would help the Stylize horror look and atmosphere generally |
| Chromatic aberration (`post.comp`) | Offsets R and B by a radial amount | One extra tonemap tap | Low | Also a Spider-Verse-style option |
| Sharpen (`post.comp`) | Unsharp mask from four neighbours | Neighbour taps in tonemap | Low | |
| Lens flare ghosts (`post.comp`) | Sun halo, streak and ghosts | Sun screen position and occlusion | Low | Cosmetic |
| Cel band function (`scene.frag`, `brdf`) | Three-step ramp on N.L with soft edges | Ramp variant in `mtoon`/`mmdtoon` | Low | Easy to test against the current ramp |
| Motion blur (`post.comp`, `resolve`) | Gathers along the velocity buffer | Velocity buffer, already present | Low to medium | |
| Auto exposure (`exposure.comp`) | Log-average luminance, clamped 0.15 to 5, adapted over time | A reduction pass (64x64 mip or compute) and a 1-pixel state texture | Medium | Our exposure is manual |
| Sky clouds, stars, aurora, moon (`sky.frag`) | Two cloud decks, stars, moon disc, aurora | Compare with our sky first | Medium | |
| Normal-based effects (`composite.comp`) | SSAO, SSR, and depth-normal Sobel outline | A world normal buffer in the scene pass, as an extra render target | Medium to high | The DeNorSob outline in the RealToon list needs this |

## Recommended order
1. `.cube` loader and LUTs (test the three looks in the Graphics page).
2. Depth fog, chromatic aberration and sharpen (all in `tonemap.frag` or a new post pass).
3. Cel band as a toon ramp option, tested against the current ramp.
4. Auto exposure.
5. Normal buffer, then the depth-normal outline, SSAO and SSR (the largest change; it adds a render target).

## Not recommended
- Their compute-based post chain. Our post passes are fragment passes. Porting is more work than re-implementing the individual effects.
- Their depth-of-field gather as written. It costs more than the look is worth at our resolutions.

## Implementation status (re-implemented, not copied)

The seven recommended items were implemented from the ideas in this review. No code was copied from the
RavenEngine PP files, which have no licence notice. Treat the licence as unconfirmed until the author says otherwise.

| # | Item | Where | Default | Selftest |
|---|------|-------|---------|----------|
| 1 | `.cube` LUT loader (N*N x N strip, any N), editor picks `.cube` and `.png` | `renderer.cpp` `readCubeStrip`, `tonemap.frag` `gradingLut` reads N from the texture; `assets/LUTs/{Eclipse,Neutral,Porcelain}.cube` | off (identity) | 11v LUT |
| 2 | Depth fog with sun in-scatter (view distance, no height term); lens flare (occluded by depth at the sun's screen position) | `shaders/effects.frag`, pass 2c | off | 11v fog, flare |
| 2 | Chromatic aberration, sharpen (unsharp mask) | `tonemap.frag` via grade `model[2].y/z` | 0 | 11v CA, sharpen |
| 3 | Cel band (three flat steps) | `mtoon.frag` and `mmdtoon.frag` via `frame.toonC.x` | off | 11v cel |
| 4 | Auto exposure: 16x16 log-average reduce, per-frame adapt (speed 0.2 per frame, not per second), 1x1 RGBA8 state with `v = (log2 m + 2)/4` | `expo_reduce.frag`, pass 2d | off | 11v auto exposure |
| 5 | Normal buffer (view-space RGBA8): depth-normal outline, SSAO (12 taps, hemisphere, range check), SSR (16-step march in the depth buffer) | `normal.vert/frag` (pass 2a'), `effects.frag` | off | 11v SSAO, SSR, outline |
| 6 | Motion blur (6 taps along `velocity_`), runs BEFORE TAA | `effects.frag`; the velocity pass runs when `taaOn || motionBlur > 0` | 0 | 11v still-camera check |

Disclosures:
- Motion blur runs before TAA, so with TAA on it also smears the TAA history. Treat it as a stylistic option.
- The first-frame velocity fallback is the current view matrix, so a still camera does not blur (found and fixed during 11v).
- SSR is a short screen-space march. It misses anything off-screen and has no roughness. The strength is scaled by 0.3.
- Auto exposure starts at multiplier 1 and needs several frames to converge. A single headless frame shows only part of the adaptation.
- Cel band in the MMD path uses the same curve over the hard light test. It is subtle on the Seed-san model.
