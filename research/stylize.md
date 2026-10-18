# Stylize: plan, status and notes

## Scope and source
Goal: a toon-post look with 15 styles, distance-adaptive ink outlines, a rim light, and a preset manager.
The reference is the feature list of "AA Toon Studio Pro" (Unity Asset Store, $49.99, Built-in and URP only;
store page fetched 2026-10-13). This is our own implementation. No code, shader or asset from that package is used.
The feature list was the only input from it.

## Architecture
- One optional post pass: `shaders/stylize.frag`, run after tonemap and before FXAA
  (`render()`: tonemap → stylize → FXAA → present). Stylize reads `ldr0_` and writes `ldr1_`.
- Inputs: tex 1 = LDR frame (sRGB), tex 4 = scene depth (D32F). Eye depth = P32 / (d + P22).
- Push block: `p0` = (style 1..15, strength 0..1, outline 0..2), `p1` = (internal width, height, time), and
  `model[0]` = (P22, P32).
- Settings (`RenderSettings`): `stylize` (0 off, 1..15), `stylizeStrength` (1.0), `stylizeOutline` (1.0).
- Editor: Graphics page, "Stylize" combo, strength and outline sliders, and the "Stylize preset" combo.
- CLI: `--stylize N`, `--stylize-strength S`, `--stylize-outline O`.
- Single-pass cost: not measured in this sandbox. The store page's figure (about 0.07 ms at 1080p) is the vendor's
  claim and has not been checked here.

## Outlines
- Ink comes from depth and colour edges (`inkEdge`). Lines are 0.75 px near and grow to 2.5 px far. They fade
  between 20 m and 40 m, so distant foliage does not fill with lines.
- Each style has its own ink amount (`styleInk`). The global outline slider scales it.

## Rim light (approximated)
The stylize pass has no normal data, so its "surface-adaptive" rim is estimated from depth and colour edges.
This is an approximation. The true normal-based rim now exists as a scene-wide screen-space effect in
`effects.frag` (RealToon milestone 2), but Stylize does not read it.

## The 15 styles
Status column: "checked" means the style differs visibly from Off in the headless contact sheet
(`renders/stylize_sheet.png`) and passes selftest 11s. "Weak" and "Noisy" are open issues.

| # | Style | Look (from the shader) | Status |
|---|-------|------------------------|--------|
| 1 | Genshin Anime Cel | four stepped tones, indigo shadows, cyan rim on depth edges | checked |
| 2 | Ghibli Soft Pastel | four soft tones, desaturated, lifted shadows, warm highlights | checked |
| 3 | RealToon 3D Anime | three-step ramp, richer colour, hard specular band on the top tone | checked |
| 4 | Comic Pop Halftone | 45-degree screen, dot size follows darkness | checked |
| 5 | Watercolor Paper Bleed | wet-edge warp, pigment pooling at edges, paper tooth | **Weak**: little change from Off |
| 6 | Hand-Drawn Sketch | graphite hatching in three darkness bands, lines re-jittered at 12 Hz | checked |
| 7 | Retro 8-Bit Arcade | 4x4 pixel blocks, 16 levels with Bayer dither, 8-colour palette, scanlines | checked |
| 8 | Arcane Painterly | four-quadrant Kuwahara filter, warm light and cool shade | checked |
| 9 | Borderlands Graphic PBR | colours kept and a little richer, diagonal ink shadow hatching | checked |
| 10 | Pixar Cinematic | vibrance, soft highlight glow, warm mid-tones, film rim | checked |
| 11 | Spider-Verse Comic | red and blue fringe toward the edges, per-channel halftone, action hatching | checked |
| 12 | Cyberpunk Neon Cel | three-step high-contrast cel, violet shadows, neon rim, faint tech grid | **Weak**: subtle |
| 13 | Dark Horror Stylized | desaturated, cool shadows, depth fog with noise, grain, vignette | checked |
| 14 | Vector Pop | sixteen flat tones on luminance, soft edges, saturation pop | checked |
| 15 | Impasto Heavy Oil | canvas height from stretched noise, lit for relief, glossy highlights | **Noisy** on the sky |

## Preset manager
- "Stylize preset" combo on the Graphics page. A preset sets style, strength and outline together.
- Built-in presets: Anime Print (Genshin), Soft Pastel (Ghibli), Graphite Sketch, Vector Pop.
- Saved presets: "Save current..." stores the current look, strength and outline in `stylize_presets.json` at the
  project root. The file is at the root, not under `assets/`, so the icon audit does not need a preview for it.
  "Delete saved" removes a saved preset.
- Selftest 11w checks the file round trip with a temporary project root.

## Testing
- Headless renders of styles 0 to 15 (`/tmp/sty`, copied to `renders/stylize_sheet.png`).
- Selftest 11s: every style changes the frame, every style leaves a visible frame (not black, not blown out),
  strength 0 gives the plain frame, outline changes the frame, and Stylize works with FXAA on. All pass.
- Selftest 11w: the preset file round trip.

## Pass 2: Watercolor, Cyberpunk, Impasto
- Watercolor (5): Kuwahara-style flat wash, edge pooling at wash boundaries, paper granulation, blotchy wet
  blooms, highlights lifted to paper white. Frame diff vs. plain 13.2 (was 10.2). Reads as a painted wash.
  Still weakish on the flat ground; the wash edges show as dark lines there.
- Cyberpunk (12): split tone (violet, magenta, cyan) in four hard bands, colour split on edges, neon outlines
  (pink to cyan across the frame), grid, scanlines, glow on highlights. Frame diff vs. plain 22.7 (was 11.4).
  Clearly distinct now.
- Impasto (15): relief now only where the image has paint (colour change, or depth under 60 m). The sky and far
  ground are smooth: sky diff 0.12 (was 9.4). The effect is mild: character diff 7.3 (was 9.5). The height
  field gives too little relief, and raising the normal scale 1.2 to 3.0 changed little. Next step: a height
  field from the image's own colour (stroke-aligned), not just noise.
- Headless renders: `/tmp/sty2/after5.png`, `after12.png`, `after15c.png`.

## Open issues (not addressed)
- Impasto relief is mild (see pass 2). Watercolor ground still has dark wash edges.
- The rim and the outline-adaptive look are approximations (see above).
- Performance has not been measured. This sandbox has only llvmpipe (software Vulkan), so a frame time here
  would not reflect a GPU. Counted per pixel from the shader source: watercolor makes five flat-wash calls, each
  36 texture fetches, so about 180 fetches. That is the costliest of the three, and it is the first thing to
  optimise (fewer samples or a separable pass). Cyberpunk adds 2 fetches. Impasto adds 4 fetches and 3 fbm calls.
  Real GPU timing is still needed.
- Not in scope for this pass: per-material stylize, and Stylize on the MMD character path specifically (it is a
  post pass, so it applies to all materials).
