// Multi-channel signed distance fields for text (Flax 1.12's MSDF fonts).
//
// A glyph atlas rasterised at one size is only sharp at that size: draw it at twice the size and every edge is a
// two-pixel ramp; draw it at half and the thin strokes disappear. A distance field replaces the coverage bitmap with the
// distance to the glyph's outline, and the outline is recovered at any scale by asking where that distance crosses
// 0.5 — a smooth interpolation of a linear function stays linear, so an 8x enlargement is as sharp as the original.
//
// A *single* channel gets the distance wrong at corners: the field of two edges meeting at an angle interpolates to the
// average of their distances, and the corner rounds off. The multi-channel variant stores three signals, each the
// distance to a different subset of the outline's edges, and the sampler takes the median. Near a corner two channels
// belong to the two edges that meet there, so the median picks the sharper of the two and the corner stays a corner.
//
// This header is the generator, shared by the font atlas the HUD draws from and by tools/msdf_preview.cpp. It takes a
// TrueType file and a glyph index — the same stb_truetype the coverage rasteriser uses — and returns one glyph's cell.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ukiyo::msdf {

struct Params {
    // Pixels on each side of the outline the field resolves. Wider survives a bigger enlargement; narrower fits more
    // glyphs in the atlas. 6 px at a 40 px em is 0.15 em, which holds up to about 4x the layout size.
    double range = 6.0;
    // A junction that turns by less than this many degrees is smooth and keeps one colour, so flattened curve segments
    // behave as the single curve they came from. Anything sharper is a corner and splits the colour.
    double angleThreshold = 3.0;
    // Samples per texel per axis. The shader reconstructs the edge over a couple of screen pixels anyway, so one
    // sample is enough; the option exists because the figure in docs/MSDF.md measures what a second one buys.
    int supersample = 1;
};

// One glyph's cell: RGBA8, where rgb is the three channels encoded over [-range, +range] around 0.5 and a is the median
// channel, so a renderer that has not been taught about MSDF still draws something sensible.
struct Field {
    int width = 0, height = 0;
    int xoff = 0, yoff = 0;          // cell top-left relative to the pen position, in pixels at the generator's scale
    std::vector<uint8_t> pixels;     // RGBA8, width * height * 4

    bool empty() const { return pixels.empty(); }
};

// One flattened segment of a glyph's outline, in font units scaled to pixels and in stb_truetype's frame (y up, the
// glyph's top at a negative y). A curve is many short segments: the generator flattens to a fraction of a texel, and
// anything that measures the field has to measure the same outline it was built from.
struct Segment {
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
};

// The glyph's outline, flattened. False for a glyph with no outline (a space).
bool outline(const uint8_t* ttf, size_t size, int glyphIndex, double scale, std::vector<Segment>* out);

// The exact signed distance from a point to that outline: positive inside the glyph, negative outside, in pixels at the
// generator's scale. This is the field's own ground truth — what `build` approximates — and it is here, in the shared
// header, so a test can ask how far the field is from the shape rather than from another approximation of it.
double signedDistance(const std::vector<Segment>& outline, double x, double y);

// Builds one glyph's field. `scale` maps font units to pixels (stbtt_ScaleForPixelHeight's own scale, optionally
// multiplied to generate above the layout size). Returns false for an empty glyph (a space), which has no outline and
// needs no cell. Requires an stb_truetype implementation to be linked into the program — text_atlas.cpp provides it.
bool build(const uint8_t* ttf, size_t size, int glyphIndex, double scale, const Params& params, Field* out);

// The sampler's reconstruction: the median of the three channels. 0.5 is the outline, below it is outside the glyph.
inline double median(double r, double g, double b) {
    return std::max(std::min(r, g), std::min(std::max(r, g), b));
}

// What shaders/hud.frag does with one bilinear-filtered texel: the median channel, scaled by the field's width in screen
// pixels, clamped to an edge.
//
//   rangeUv     the field's half-width in uv units -- `FontAtlas::msdfRange() / FontAtlas::width()`, which is the
//               shader's `vec2(pc.p0.z) / vec2(textureSize(uAtlas, 0))`
//   uvPerPixel  the change in uv across one screen pixel, which is the shader's fwidth() and what a CPU rasteriser can
//               solve for from a quad's corners
//
// The ramp spans one screen pixel, not the two the textbook `0.5 * dot(unitRange, screenTexSize)` gives. A two-pixel
// ramp carries half a pixel of ink beyond each side of the outline, which is invisible on a heading and a third of the
// whole glyph on 13 px text: measured against stb_truetype's own rasterisation, the textbook scaling puts 1.21x the ink
// of an 'H' and 1.64x of an 'i' at 12 px, and this one 0.96x and 1.14x, with the mean signed error at 13 px going from
// +0.06 to -0.0001. tests/msdf_test.cpp measures it; docs/MSDF.md quotes it.
//
// Sharing this half of the feature with the shader is what lets the tests and the figure measure the picture the GPU
// draws rather than an impression of it.
inline double coverageFromField(double r, double g, double b, double rangeUv, double uvPerPixelX,
                                double uvPerPixelY) {
    const double sig = median(r, g, b) - 0.5;
    const double pxPerUvX = 1.0 / std::max(uvPerPixelX, 1e-12);
    const double pxPerUvY = 1.0 / std::max(uvPerPixelY, 1e-12);
    const double screenPxRange = std::max(rangeUv * (pxPerUvX + pxPerUvY), 1.0);
    return std::min(std::max(sig * screenPxRange + 0.5, 0.0), 1.0);
}

} // namespace ukiyo::msdf
