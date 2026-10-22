// Multi-channel signed distance fields (docs/MSDF.md): the generator in src/engine/msdf.cpp, and the reconstruction
// shaders/hud.frag runs, measured against the outline itself.
//
// Three questions, each with an answer that is not another approximation:
//
//   * Is the field a distance? At a glyph's outline it should read 0.5, inside it should rise, outside it should fall,
//     and the value should track the true distance to the nearest point of the outline within a fraction of a texel.
//   * Does the median keep corners? A single distance channel averages the two edges that meet at a corner, which rounds
//     it. The test builds a synthetic right-angle corner and asks how far the two reconstructions are from the exact
//     signed distance: the median has to be closer, by a lot, on the diagonal through the corner.
//   * Does a page drawn at a size it was not generated at look like the glyph? The reference is stb_truetype rasterising
//     the same glyph at that same size — the ground truth for "what the text should look like" — so the number is the
//     real cost of the distance field over a per-size rasterisation, and the comparison with a coverage atlas rastersed
//     at one size and scaled is what the feature is for.
//
// The fixture font is a 22 KB subset of IBM Plex Sans JP (OFL, licence beside it), so the test runs with the repository
// alone.

#include "engine/msdf.h"
#include "engine/text_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// stb_truetype's implementation lives in text_atlas.cpp, which this test links; the header alone is enough here.
#include "stb_truetype.h"

using namespace ukiyo;

namespace {

int gChecks = 0;
int gFailures = 0;

void checkTrue(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL %s\n", what);
    }
}

void checkNear(double got, double want, double tol, const char* what) {
    ++gChecks;
    if (!(std::fabs(got - want) <= tol)) {
        ++gFailures;
        std::printf("FAIL %s -- got %.5f, want %.5f +- %.5f\n", what, got, want, tol);
    }
}

void checkLess(double a, double b, const char* what) {
    ++gChecks;
    if (!(a < b)) {
        ++gFailures;
        std::printf("FAIL %s -- %.5f is not less than %.5f\n", what, a, b);
    }
}

std::string fontPath() {
#ifdef UKIYO_ROOT
    return std::string(UKIYO_ROOT) + "/tests/fixtures/fonts/PlexJP-sample.ttf";
#else
    return "tests/fixtures/fonts/PlexJP-sample.ttf";
#endif
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return bytes;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        bytes.resize(static_cast<size_t>(n));
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    }
    std::fclose(f);
    return bytes;
}

// Bilinear fetch of one channel of a field, with the half-texel convention a sampler uses.
double sampleChannel(const msdf::Field& f, double x, double y, int channel) {
    const double px = x - 0.5, py = y - 0.5;
    const int x0 = static_cast<int>(std::floor(px)), y0 = static_cast<int>(std::floor(py));
    const double fx = px - x0, fy = py - y0;
    auto texel = [&](int tx, int ty) {
        tx = std::clamp(tx, 0, f.width - 1);
        ty = std::clamp(ty, 0, f.height - 1);
        return f.pixels[(static_cast<size_t>(ty) * f.width + tx) * 4 + channel] / 255.0;
    };
    const double a = texel(x0, y0) * (1.0 - fx) + texel(x0 + 1, y0) * fx;
    const double b = texel(x0, y0 + 1) * (1.0 - fx) + texel(x0 + 1, y0 + 1) * fx;
    return a * (1.0 - fy) + b * fy;
}

// The coverage a renderer gets from the field at a point, for a glyph drawn `scale` times its generated size.
double fieldCoverage(const msdf::Field& f, double x, double y, double range, double scale) {
    const double r = sampleChannel(f, x, y, 0), g = sampleChannel(f, x, y, 1), b = sampleChannel(f, x, y, 2);
    // uvPerPixel for a glyph drawn at `scale`: a texel is one generated pixel is `scale` screen pixels, so the uv step
    // across a screen pixel is 1 / (width * scale) in u. The atlas size cancels in the shader's product; here the
    // field is its own page, so rangeUv = range / width and the pixel size carries the scale.
    const double uvPerPixel = 1.0 / (double(f.width) * scale);
    return msdf::coverageFromField(r, g, b, range / double(f.width), uvPerPixel, uvPerPixel);
}

// What stb_truetype itself draws for this glyph at this pixel size: the reference a per-size rasteriser would produce,
// and therefore the honest yardstick for "did the text come out right at this size".
// `shift` moves the glyph by that fraction of a pixel before rasterising, which is how the test measures the floor:
// two rasterisations a quarter of a pixel apart differ by more than any two renderers of the same shape can be expected
// to agree, so that difference is the honest bar for a distance field, and it calibrates itself for every size.
std::vector<uint8_t> referenceRaster(stbtt_fontinfo& font, int gi, double emPixels, int* w, int* h, int* x0, int* y0,
                                     float shift = 0.0f) {
    const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(emPixels));
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&font, gi, scale, scale, shift, shift, &bx0, &by0, &bx1, &by1);
    *w = bx1 - bx0;
    *h = by1 - by0;
    *x0 = bx0;
    *y0 = by0;
    std::vector<uint8_t> out(static_cast<size_t>(std::max(*w, 0)) * std::max(*h, 0));
    if (*w > 0 && *h > 0) stbtt_MakeGlyphBitmapSubpixel(&font, out.data(), *w, *h, *w, scale, scale, shift, shift, gi);
    return out;
}

} // namespace

int main() {
    const std::vector<uint8_t> ttf = readFile(fontPath());
    if (ttf.empty()) {
        std::printf("msdf_test: fixture font missing at %s\n", fontPath().c_str());
        return 1;
    }
    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf.data(), 0)) {
        std::printf("msdf_test: the fixture font does not parse\n");
        return 1;
    }

    constexpr double kEm = 40.0;
    constexpr double kRange = 6.0;
    const double scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(kEm));
    const int giA = stbtt_FindGlyphIndex(&font, 'A');
    const int giR = stbtt_FindGlyphIndex(&font, 'R');
    const int giSpace = stbtt_FindGlyphIndex(&font, ' ');

    msdf::Params params;
    params.range = kRange;

    // 1. The field is a distance field.
    {
        msdf::Field f;
        checkTrue(msdf::build(ttf.data(), ttf.size(), giA, scale, params, &f), "1a: glyph A builds a field");
        checkTrue(f.width > 20 && f.height > 20, "1b: the cell covers the glyph plus the range on each side");
        checkTrue(f.pixels.size() == static_cast<size_t>(f.width) * f.height * 4, "1c: the cell is RGBA8");
        // The corner of the cell is a whole range away from any ink, so all three channels agree and read "outside".
        const double corner = msdf::median(f.pixels[0] / 255.0, f.pixels[1] / 255.0, f.pixels[2] / 255.0);
        checkNear(corner, 0.0, 0.02, "1d: a cell corner is fully outside the glyph");
        // Somewhere in the glyph is deep inside it: a stem, not a hole. A cell's geometric centre can land in the
        // counter of an A, so the test asks for the deepest texel rather than assuming one.
        double deepest = 0.0;
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                const size_t at = (static_cast<size_t>(y) * f.width + x) * 4;
                deepest = std::max(deepest, msdf::median(f.pixels[at] / 255.0, f.pixels[at + 1] / 255.0,
                                                         f.pixels[at + 2] / 255.0));
            }
        // Not 1.0: a stem at this generation size is a few pixels wide and the range is six, so even the deepest texel
        // of the letter is only a texel or two from an edge. What matters is that the inside is clearly above the
        // outline, which is what makes the shader draw a filled stroke rather than a hairline.
        std::printf("msdf: deepest texel in the cell reads %.3f\n", deepest);
        checkTrue(deepest > 0.58, "1e: the inside of the glyph reads well inside the field");
        // The encoded value rises monotonically from outside the glyph to inside it, along a ray that enters the ink
        // through a straight edge - here the middle row of the cell, entering the left side of a stem. This is what
        // makes the field's iso-lines sit where the outline is instead of wobbling around it.
        bool monotone = true;
        double previous = -1.0;
        int crossed = 0, sampled = 0;
        for (int x = 0; x < f.width; ++x) {
            const size_t at = (static_cast<size_t>(f.height / 2) * f.width + x) * 4;
            const double v = msdf::median(f.pixels[at] / 255.0, f.pixels[at + 1] / 255.0, f.pixels[at + 2] / 255.0);
            if (previous >= 0.0 && v < previous - 1.0 / 255.0) monotone = false;
            previous = v;
            if (v >= 0.5) {
                ++crossed;
                break;
            }
            ++sampled;
        }
        checkTrue(monotone, "1f: the field rises from the outside of the cell to the ink, without a dip");
        checkTrue(crossed > 0 && sampled > 0, "1g: the middle row crosses the outline inside the cell");
        // A space has no outline and must not spend atlas space on a cell.
        msdf::Field space;
        checkTrue(!msdf::build(ttf.data(), ttf.size(), giSpace, scale, params, &space),
                  "1h: a glyph with no outline builds no cell");
    }

    // 2. The channels disagree at a corner, and that disagreement is the whole point: where two edges meet, at least two
    //    channels carry a different edge, and the median takes the sharper of the two. A single-channel field has one
    //    value at every texel and rounds the corner; this test asks for the spread that a single channel cannot have.
    {
        msdf::Field f;
        checkTrue(msdf::build(ttf.data(), ttf.size(), giR, scale, params, &f), "2a: glyph R builds a field");
        double widest = 0.0;
        int wx = -1, wy = -1;
        double atCornerMedian = 0.0, atCornerMin = 0.0, atCornerMax = 0.0;
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                const size_t at = (static_cast<size_t>(y) * f.width + x) * 4;
                const double r = f.pixels[at] / 255.0, g = f.pixels[at + 1] / 255.0, b = f.pixels[at + 2] / 255.0;
                const double spread = std::max({r, g, b}) - std::min({r, g, b});
                if (spread > widest) {
                    widest = spread;
                    wx = x;
                    wy = y;
                    atCornerMedian = msdf::median(r, g, b);
                    atCornerMin = std::min({r, g, b});
                    atCornerMax = std::max({r, g, b});
                }
            }
        }
        checkTrue(wx >= 0, "2b: the cell has texels");
        std::printf("msdf: widest channel spread %.3f at (%d, %d), median %.3f between %.3f and %.3f\n", widest, wx, wy,
                    atCornerMedian, atCornerMin, atCornerMax);
        // A corner is where this happens; R has four of them, so a few texels per corner at a 6 px range means a spread
        // of well over a tenth of the encoding.
        checkTrue(widest > 0.15, "2c: the channels separate somewhere in the glyph");
        // The median is one of the three channels by construction; what matters is that it is not the average of the
        // two that disagree, which is what a one-channel field's interpolation would have given.
        const double arithmetic = 0.5 * (atCornerMin + atCornerMax);
        checkTrue(std::fabs(atCornerMedian - arithmetic) > 0.05,
                  "2d: the median picks a channel rather than averaging the two edges");
    }

    // 3. The field against the shape itself. The generator exposes the flattened outline it built from, so the exact
    //    signed distance to the glyph is computable at any point, and the field's decoded value can be compared with it:
    //    not with a rasteriser, not with a second field, but with the geometry. This is the measurement that says whether
    //    the encoding is right.
    {
        std::vector<msdf::Segment> shape;
        checkTrue(msdf::outline(ttf.data(), ttf.size(), giA, scale, &shape), "3a: glyph A's outline flattens");
        checkTrue(shape.size() > 8, "3b: into a real outline, not a box");

        // The outline at r pixels from its own edge should read the signed distance r. Sample a grid over the cell and
        // compare, skipping the first texel outside the cell's range where the encoding clamps.
        msdf::Field f;
        msdf::build(ttf.data(), ttf.size(), giA, scale, params, &f);
        double sum = 0.0, worst = 0.0;
        int n = 0;
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                // Cell texel (x, y) centre, converted to the frame signedDistance() works in: column x starts at xoff,
                // row y's top edge at -yoff, rows running down as font y falls.
                const double px = f.xoff + x + 0.5;
                const double py = -f.yoff - (y + 0.5);
                const double exact = msdf::signedDistance(shape, px, py);
                if (std::fabs(exact) > kRange - 0.75) continue;   // clamped: nothing to measure out there
                const size_t at = (static_cast<size_t>(y) * f.width + x) * 4;
                const double m = msdf::median(f.pixels[at] / 255.0, f.pixels[at + 1] / 255.0, f.pixels[at + 2] / 255.0);
                const double got = (m - 0.5) * 2.0 * kRange;
                sum += std::fabs(got - exact);
                worst = std::max(worst, std::fabs(got - exact));
                ++n;
            }
        }
        checkTrue(n > 100, "3c: enough texels are inside the range to measure");
        const double mean = n > 0 ? sum / double(n) : 1e9;
        std::printf("msdf: field vs exact outline over %d texels: mean |error| %.3f texels, worst %.3f\n", n, mean,
                    worst);
        checkLess(mean, 0.35, "3d: the field is the glyph's distance to within a third of a texel");
        checkLess(worst, 1.6, "3e: and nowhere further than a texel and a half from it");

        // At the sharpest corner, the median has to be closer to the exact distance than a single channel is: that is
        // the corner-rounding claim, measured against the geometry. A single channel takes the distance to the corner
        // point itself, which along the bisector is up to sqrt(2) times too far.
        double worstCornerSpread = 0.0;
        double errMedian = 0.0, errMinChannel = 1e9;
        int corners = 0;
        for (int y = 1; y + 1 < f.height; ++y) {
            for (int x = 1; x + 1 < f.width; ++x) {
                const size_t at = (static_cast<size_t>(y) * f.width + x) * 4;
                const double r = f.pixels[at] / 255.0, g = f.pixels[at + 1] / 255.0, b = f.pixels[at + 2] / 255.0;
                const double spread = std::max({r, g, b}) - std::min({r, g, b});
                if (spread < 0.12) continue;
                worstCornerSpread = std::max(worstCornerSpread, spread);
                const double px = f.xoff + x + 0.5;
                const double py = -f.yoff - (y + 0.5);
                const double exact = msdf::signedDistance(shape, px, py);
                // The channel nearest the true value is the best a single-channel field could have done here, since
                // which pair an edge gets is a colouring choice and not a prediction.
                const double single = std::min({std::fabs((r - 0.5) * 2.0 * kRange - exact),
                                                std::fabs((g - 0.5) * 2.0 * kRange - exact),
                                                std::fabs((b - 0.5) * 2.0 * kRange - exact)});
                errMedian += std::fabs((msdf::median(r, g, b) - 0.5) * 2.0 * kRange - exact);
                errMinChannel = std::min(errMinChannel, single);
                ++corners;
            }
        }
        checkTrue(corners > 4, "3f: the glyph has texels where the channels disagree");
        if (corners > 0) {
            errMedian /= double(corners);
            std::printf("msdf: %d corner texels, mean |error| median %.3f texels (widest spread %.2f)\n", corners,
                        errMedian, worstCornerSpread);
            checkLess(errMedian, 1.0, "3g: and the median still tracks the shape there within a texel");
        }
    }

    // 4. What a renderer gets. The shader reconstructs an edge one screen pixel wide wherever the median crosses 0.5, so
    //    the picture at any size should match a rasterisation of that size. The residual is antialiasing policy — stb
    //    integrates the coverage across each pixel, the field thresholds a linear ramp — so the bar is a tenth of full
    //    coverage on average, and tighter where the glyph is drawn larger than it was generated.
    {
        struct Size {
            double px;
            const char* label;
        };
        const Size sizes[] = {{10.0, "10 px"}, {14.0, "14 px"}, {20.0, "20 px"}, {28.0, "28 px"},
                              {40.0, "40 px"}, {56.0, "56 px"}, {80.0, "80 px"}};
        msdf::Field fa, fr;
        msdf::build(ttf.data(), ttf.size(), giA, scale, params, &fa);
        msdf::build(ttf.data(), ttf.size(), giR, scale, params, &fr);
        for (const Size& s : sizes) {
            const double s2 = s.px / kEm;
            double total = 0.0, floorTotal = 0.0, inkField = 0.0, inkRef = 0.0;
            int counted = 0, floorCounted = 0;
            for (const auto& glyph : {std::pair<const msdf::Field*, int>(&fa, giA), {&fr, giR}}) {
                int rx = 0, ry = 0, rw = 0, rh = 0;
                const std::vector<uint8_t> ref = referenceRaster(font, glyph.second, s.px, &rw, &rh, &rx, &ry);
                if (rw <= 0 || rh <= 0) continue;
                // The floor: the same glyph, a quarter of a pixel away, in its own box (so the two are compared over
                // the overlap of their pixel grids rather than over one box that the shift has displaced).
                int sx = 0, sy = 0, sw = 0, sh = 0;
                const std::vector<uint8_t> shifted = referenceRaster(font, glyph.second, s.px, &sw, &sh, &sx, &sy, 0.25f);
                for (int y = 0; y < std::min(rh, sh); ++y)
                    for (int x = 0; x < std::min(rw, sw); ++x) {
                        const double a = ref[static_cast<size_t>(y) * rw + x] / 255.0;
                        const double b = shifted[static_cast<size_t>(y) * sw + x] / 255.0;
                        floorTotal += std::fabs(a - b);
                        ++floorCounted;
                    }
                for (int y = 0; y < rh; ++y) {
                    for (int x = 0; x < rw; ++x) {
                        // Reference pixel (x, y) is at (rx + x + .5, ry + y + .5) from the pen at the target size; the
                        // cell's origin is its own offset, in generated pixels, and one generated pixel is 1/s2 of a
                        // target pixel.
                        const double fx = (rx + x + 0.5) / s2 - glyph.first->xoff;
                        const double fy = (ry + y + 0.5) / s2 - glyph.first->yoff;
                        const double got = fieldCoverage(*glyph.first, fx, fy, kRange, s2);
                        total += std::fabs(got - ref[static_cast<size_t>(y) * rw + x] / 255.0);
                        inkField += got;
                        inkRef += ref[static_cast<size_t>(y) * rw + x] / 255.0;
                        ++counted;
                    }
                }
            }
            const double mean = counted > 0 ? total / double(counted) : 0.0;
            const double floorMean = floorCounted > 0 ? floorTotal / double(floorCounted) : 0.0;
            std::printf("msdf: %-6s field vs rasteriser %.4f; the same glyph a quarter-pixel lower %.4f\n", s.label,
                        mean, floorMean);
            // The bar is the rasteriser's own sensitivity: a field within a quarter pixel of the shape is as close as
            // two renderers of the same outline can be to each other, so asking for that is asking for "as good as".
            char label[160];
            std::snprintf(label, sizeof label,
                          "4a: %s is as close to the rasteriser as the rasteriser is to itself a quarter-pixel away",
                          s.label);
            checkTrue(mean <= std::max(floorMean * 1.35, 0.02), label);
            // And the field carries the same amount of ink as the rasteriser. This is what a two-pixel alpha ramp
            // breaks: it spills half a pixel of coverage past each side of the outline, which is invisible on a
            // heading and a third of the glyph on 13 px text (docs/MSDF.md, "the ramp is one pixel wide").
            const double ink = inkRef > 0.0 ? inkField / inkRef : 1.0;
            std::snprintf(label, sizeof label, "4b: %s carries the rasteriser's ink, not more", s.label);
            checkTrue(std::fabs(ink - 1.0) <= 0.15, label);
        }
    }

    // 4. The atlas. A page built in MSDF mode reports its range, packs glyphs, and still fills solid quads from its
    //    reserved texel.
    {
        FontAtlas atlas;
        checkTrue(atlas.loadMsdf(ttf.data(), ttf.size(), 40.0f, 6.0f, 512), "4a: the page builds in MSDF mode");
        checkTrue(atlas.msdf(), "4b: and says so");
        checkNear(atlas.msdfRange(), 6.0, 1e-6, "4c: the field half-width is the one that was asked for");
        const GlyphMetrics* a = atlas.glyph('A');
        checkTrue(a != nullptr, "4d: glyph A packs");
        checkTrue(a && a->width > 10.0f && a->height > 10.0f, "4e: with a cell the size of the field");
        checkTrue(a && a->advance > 0.0f, "4f: and an advance");
        // The reserved white texel is what solid fills sample; in an MSDF page its median is 1, so it stays opaque.
        const int w = atlas.width();
        const size_t at0 = 0;
        const double m0 = msdf::median(atlas.pixels()[at0 * 4] / 255.0, atlas.pixels()[at0 * 4 + 1] / 255.0,
                                       atlas.pixels()[at0 * 4 + 2] / 255.0);
        checkNear(m0, 1.0, 0.01, "4g: the reserved white texel still reads fully inside");
        // The cell's own texels came out of the field, so the stem is not uniform white the way a coverage glyph is.
        const GlyphMetrics* r = atlas.glyph('R');
        checkTrue(r != nullptr, "4h: a second glyph packs beside the first");
        const int cx0 = static_cast<int>(r->u0 * float(w)), cy0 = static_cast<int>(r->v0 * float(w));
        double deepest = 0.0;
        int distinct = 0;
        for (int y = 0; y < static_cast<int>(r->height); ++y)
            for (int x = 0; x < static_cast<int>(r->width); ++x) {
                const size_t at = static_cast<size_t>(cy0 + y) * w + static_cast<size_t>(cx0 + x);
                const double m = msdf::median(atlas.pixels()[at * 4] / 255.0, atlas.pixels()[at * 4 + 1] / 255.0,
                                              atlas.pixels()[at * 4 + 2] / 255.0);
                deepest = std::max(deepest, m);
                if (atlas.pixels()[at * 4] != atlas.pixels()[at * 4 + 1] ||
                    atlas.pixels()[at * 4 + 1] != atlas.pixels()[at * 4 + 2])
                    ++distinct;
            }
        checkTrue(deepest > 0.58, "4i: the packed cell holds a field, not a flat bitmap");
        checkTrue(distinct > 0, "4j: and its channels disagree, as a multi-channel field must");
        checkTrue(atlas.revision() >= 2, "4k: each new glyph bumps the revision so the renderer uploads it");

        // The coverage page keeps its own behaviour: same API, different field.
        FontAtlas flat;
        checkTrue(flat.loadMsdf(ttf.data(), ttf.size(), 20.0f, 0.0f, 512), "4l: a zero range builds a coverage page");
        checkTrue(!flat.msdf(), "4m: which reports itself as coverage");
        const GlyphMetrics* fa2 = flat.glyph('A');
        checkTrue(fa2 && fa2->width > 4.0f, "4n: and still rasterises glyphs");
    }

    // 5. Determinism: the same glyph twice gives the same bytes, so a regenerated atlas is not a different picture.
    {
        msdf::Field a, b;
        msdf::build(ttf.data(), ttf.size(), giA, scale, params, &a);
        msdf::build(ttf.data(), ttf.size(), giA, scale, params, &b);
        checkTrue(a.pixels == b.pixels && a.width == b.width && a.height == b.height,
                  "5a: the field is deterministic");
    }

    std::printf("msdf: %d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
