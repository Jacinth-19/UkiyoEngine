# Multi-channel signed distance fields for text

A font may be drawn at any size; a glyph page is rasterised at one. The engine's HUD, its debug overlay and the player's
readout all share a single page, and until now that page held coverage bitmaps at one pixel size: sharp at that size, and
a magnified bitmap everywhere else. Flax 1.12 shipped the fix that this engine now has — a page of signed distance
fields, generated once, sharp at every size.

![The same text from a coverage page and from a distance field](msdf.png)

*Two pages from the same font, laid out by the shipping `HudCanvas` and rasterised with `shaders/hud.frag`'s own rule.
**Left:** a coverage page rasterised at 20 px, drawn at 12, 20 and 40 px. Sharp at 20, because that is where it was made;
soft at 40, where the page's pixels are magnified; thin at 12, where they are dropped. **Right:** a distance field
generated at 40 px with a 6-texel range, drawn at the same sizes: the same edge at all three. The Japanese line is the
case that matters for this engine — six dense strokes at 64 px out of a page rasterised at 20 — and the 96 px letter is
where a magnified bitmap stops being an opinion. Bottom left: what the field actually stores, red, green and blue
carrying the distance to different subsets of the outline's edges and their median the outline itself. Bottom right: the
measurements this run made. Made by `tools/msdf_preview.cpp`.*

## 1. Why three channels

A signed distance field stores, at every texel, the distance to the nearest point of the glyph's outline: 0.5 at the
outline, above it inside the glyph, below it outside. Recovering the shape is then a threshold, and because a linear
function interpolates to a linear function, the threshold lands where the outline actually is at any magnification. The
edge is one screen pixel wide however large the glyph is drawn.

A **single** channel is wrong at a corner, and text is mostly corners. Where two edges meet, the distance to the outline
is the minimum of the two edges' distances, and interpolating that minimum across the corner rounds it: the iso-line at
0.5 cuts the corner off. The multi-channel variant stores three fields, each the distance to a *different subset* of the
outline's edges, and the renderer takes the **median** of the three at each sample. Near a corner two channels belong to
the two edges that meet there, so the median keeps whichever is sharper and the corner stays a corner; along a single
edge all three channels agree, so the median is that edge.

The colouring rule is the one thing the generator has to get right: walk the outline, and where it turns by more than
3° start a new run with the next of the three channel pairs (rg, gb, rb). A flattened curve turns by fractions of a
degree between segments, so it stays in one run and behaves as the single curve it came from.

## 2. The generator

`src/engine/msdf.cpp`, ~300 lines, no dependency beyond the stb_truetype the text atlas already used.

```
outline     = flatten the glyph's contours to 0.03 px
colour      = split each contour at corners > 3 degrees, cycling rg / gb / rb
cell        = the outline's box grown by `range` on each side
per texel   = for each channel: the distance to the nearest edge of that channel's edges
              encoded as 0.5 + signedDistance / (2 * range), so inside reads above 0.5
alpha       = the median channel, so old readers of the page still draw text
```

Two decisions in that loop are worth their cost. An edge farther than `range` from a texel cannot change the encoded
value — the encoding clamps — so those edges are skipped, which is what makes the generator fast enough to run at load
time (0.23 ms per glyph at 40 px, ASCII in 21 ms). And the cell's frame is stb_truetype's own: a glyph bitmap's row *r*
is font-space `y = yoff + r`, so the outline is kept in that frame, the bounding box comes from the flattened segments
rather than from stb's separate box, and the offsets the atlas hands the HUD are the ones every other glyph consumer in
the engine already expects. Getting that frame wrong cost a day; it is written down here so it costs no one else one.

## 3. The shader

`shaders/hud.frag` grows one branch, selected by a uniform so a coverage page costs nothing:

```glsl
float median  = max(min(r, g), min(max(r, g), b));          // the same rule as msdf::median
float sig     = median - 0.5;                                // 0 at the outline, positive inside
vec2  unitRange     = vec2(pc.p0.z) / vec2(textureSize(uAtlas, 0));   // the range in uv units
vec2  texelPerPixel = fwidth(vUV);                           // and the same, per screen pixel
float screenPxRange = max(dot(unitRange, 1.0 / texelPerPixel), 1.0);
alpha = clamp(sig * screenPxRange + 0.5, 0.0, 1.0);
```

`fwidth(vUV)` is the uv change across one screen pixel, so `dot(unitRange, 1.0 / texelPerPixel)` is the field's range in
screen pixels: `range * size / generationSize`, and 6 texels of a 40 px page drawn at 13 px is 1.95 px. The shader needs
to know nothing about the glyph's size, only the page's range. `pc.p0.z` is that range, set by `HudRenderer::syncAtlas`
from `FontAtlas::msdfRange()`; 0 selects the coverage path. The reserved white texel at (0,0) still works for solid
fills: its RGB is 1, so its median is above the outline and a fill comes out opaque through the same expression.

### The ramp is one pixel wide, not two

Every published MSDF shader — `msdfgen`'s own included — writes `0.5 * dot(unitRange, screenTexSize)`. That factor
makes the alpha ramp span **two** screen pixels: half a pixel of distance on either side of the outline is encoded, and
`alpha = 0.5 + 0.5 * d` means a pixel half a pixel *outside* the edge still gets a quarter of the ink. On a 40 px heading
that is nothing. On 13 px text, where a stem is barely one pixel wide, it is a third of the glyph, and the text renders
visibly bold and blobby — `orbit` closes into `orblt`.

Dropping the 0.5 makes the ramp one pixel wide, which is what a rasteriser's own antialiasing does: coverage is zero
half a pixel from the edge, not one. Measured against stb_truetype's rasterisation of the same outlines, summing alpha
over the glyph:

| glyph, drawn at | ink, textbook scale | ink, one-pixel scale |
| --- | --- | --- |
| `H` at 12 px | 1.210 | 0.961 |
| `H` at 13 px | 1.226 | 1.000 |
| `i` at 12 px | 1.642 | 1.140 |
| `i` at 13 px | 1.459 | 1.108 |
| `o` at 13 px | 1.289 | 1.035 |
| `o` at 40 px, the generation size | 1.058 | 1.067 |
| `H` at 64 px | 0.969 | 1.000 |

The same change takes the mean signed per-pixel error from +0.061 to −0.0001 at 13 px, and the mean absolute error
against the rasteriser over the whole test from 0.09 to 0.02. It is the single largest correctness fix in the feature
and it is four characters shorter than the textbook version.

## 4. Numbers

From `tools/msdf_preview.cpp` on the project's IBM Plex Sans JP, this run:

| measurement | value |
| --- | --- |
| field against the exact outline (`msdf::signedDistance` over the same flattened segments) | **0.014 texels mean**, 0.024 worst, over 577 texels |
| field against stb_truetype's rasterisation at 12 px | **0.022** |
| at 20 px | 0.013 |
| at 40 px (the generation size) | 0.006 |
| the same glyph a quarter pixel lower, at 12 / 20 / 40 px | 0.124 / 0.105 / 0.059 |
| coverage page drawn at 20 px | 0.000 — it *is* that rasterisation |
| coverage page drawn at 12 px | 0.057 |
| coverage page drawn at 40 px | 0.134 |

The field is within a quarter pixel of the shape it describes — three to twenty times closer than the rasteriser is to
itself after a quarter-pixel shift, at every size tested. The coverage page is exact at its own size and a third of full
coverage wrong at twice that size, which is the sum of what this feature fixes.

`tests/msdf_test.cpp` asserts those properties rather than those numbers: 48 checks, 0 failures.

- the field is a distance (0.5 at the outline, monotone inside, clamped outside) and within a third of a texel of the
  exact outline everywhere it is not clamped;
- the median tracks the shape at corner texels, where the channels disagree by up to 0.27 of the encoding;
- a page drawn at seven sizes from 10 to 80 px is as close to a rasterisation of that size as the rasteriser is to
  itself a quarter pixel away — 0.029 / 0.024 / 0.023 / 0.016 / 0.012 / 0.010 / 0.011 against floors of 0.11 / 0.23 /
  0.12 / 0.17 / 0.066 / 0.089 / 0.070. The bar calibrates itself per size instead of imposing an arbitrary tolerance;
- and it carries the rasteriser's ink to within 15% at every one of those sizes, which is the check that fails if the
  alpha ramp goes back to the textbook two pixels;
- the atlas: a field page reports its range, packs glyphs, fills solid quads from its reserved texel, refuses nothing it
  should not, and a zero range gives back the old coverage page;
- the same glyph generated twice is the same bytes.

The whole test runs against a 22 KB subset of the project font committed beside it (`tests/fixtures/fonts/`, OFL licence
beside it), so it needs no assets and no GPU.

## 5. What changed in the engine

- `src/engine/msdf.{h,cpp}` — the generator, the reconstruction the shader mirrors (`coverageFromField`), the flattened
  outline and the exact signed distance that everything else is measured against.
- `src/engine/text_atlas.{h,cpp}` — `loadMsdf` / `loadMsdfFromFile`, `msdf()` and `msdfRange()`; the same shelf packing,
  atlas growth and dirty-rectangle upload as before, so `HudRenderer::syncAtlas` did not need to change beyond passing
  the range through.
- `shaders/hud.frag` — the median reconstruction, `fwidth`-scaled, behind `pc.p0.z`.
- `src/engine/hud_renderer.{h,cpp}` — carries the page's range in the push block it already sets.
- `src/player/main.cpp` and the build manifest — the player generates the HUD font at 40 px with a 6-texel range, and
  `"msdf": false` in a manifest asks for the old coverage page instead.
- `tools/hud_preview.cpp` — renders `docs/player-hud.png` through the shipping path: it samples the field with
  `msdf::coverageFromField` at each pixel's own `fwidth`, which is the same expression `hud.frag` evaluates on the GPU.
  `--coverage` renders the page the engine used before, and the two are otherwise pixel-for-pixel the same layout.
- `tests/msdf_test.cpp`, `tools/msdf_preview.cpp`, this document and its figure; CI runs the test on Linux and Windows
  and regenerates the figure on every push.

## 6. What it does not do

- **It does not animate weight.** One page is one weight of one font. A bold heading is a second page, and the HUD draws
  one atlas, so a project that needs two weights needs the HUD renderer to bind per batch — a change to the draw call,
  not to this feature.
- **It is not text layout.** Kerning pairs, ligatures and shaping come from the font's own tables and none of them is
  applied yet; the canvas advances by the glyph's own width. Thai, Arabic and Devanagari need a shaper, not a distance
  field.
- **The generator is not the only way to make one.** `msdfgen` produces better corners at large sizes with its own
  edge-colouring rules; this one is here because the engine can then build a page at run time, for whatever font a
  project ships, with no offline step and no binary asset in the repository.
