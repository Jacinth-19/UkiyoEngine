// The figure for docs/MSDF.md: the same text, the same layout code, one glyph page rasterised at a single size and one
// that holds a distance field, both drawn at sizes neither page was made for.
//
// Nothing here is drawn by hand. HudCanvas lays the text out, FontAtlas produces both pages from the project font, and
// the rasteriser samples a page the way shaders/hud.frag does — bilinear, then either the alpha channel (coverage) or
// the median of the three channels scaled by the field's width in screen pixels (MSDF). So the left column really is
// what the engine drew before this feature and the right column is what it draws now, not two mockups.
//
// The numbers beside the glyphs are measured in this run: the field against the exact outline (msdf::signedDistance over
// the same flattened segments the generator used) and against stb_truetype's own rasterisation at each size, with that
// rasteriser's sensitivity to a quarter-pixel shift as the bar.
//
//   msdf_preview --out docs/msdf.png --font editor/res/fonts/IBMPlexSansJP-Regular.ttf
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/hud.h"
#include "engine/msdf.h"
#include "engine/text_atlas.h"
#include "figure_kit.h"
#include "stb_truetype.h"   // declarations only: text_atlas.cpp provides the implementation this tool links

namespace {

// The figure's image type, plus the alpha blend a HUD quad needs: figure::Image has set() but not compositing.
struct Bitmap : figure::Image {
    void blend(int x, int y, glm::vec3 c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
        glm::vec3& d = color[static_cast<size_t>(y) * w + x];
        d = d * (1.0f - a) + c * a;
    }
};

float sampleChannel(const ukiyo::FontAtlas& font, float u, float v, int channel) {
    const int w = font.width(), h = font.height();
    const float x = u * float(w) - 0.5f, y = v * float(h) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    auto texel = [&](int tx, int ty) {
        tx = std::clamp(tx, 0, w - 1);
        ty = std::clamp(ty, 0, h - 1);
        return float(font.pixels()[(static_cast<size_t>(ty) * w + tx) * 4 + channel]) / 255.0f;
    };
    const float a = texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx;
    const float b = texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx;
    return a * (1 - fy) + b * fy;
}

// What hud.frag computes for one texel: alpha for a coverage page, the median channel scaled by the field's width in
// screen pixels for a distance field. `uvPerPixel` is fwidth(uv).
float coverageAt(const ukiyo::FontAtlas& font, float u, float v, float uvPerPixel) {
    if (!font.msdf()) return sampleChannel(font, u, v, 3);
    const double r = sampleChannel(font, u, v, 0), g = sampleChannel(font, u, v, 1), b = sampleChannel(font, u, v, 2);
    const double rangeUv = double(font.msdfRange()) / double(font.width());
    return float(ukiyo::msdf::coverageFromField(r, g, b, rangeUv, uvPerPixel, uvPerPixel));
}

// Rasterises a HUD triangle list into `img` at (ox, oy). A quad's uv derivative comes from its own corners, which is
// what fwidth gives a fragment shader for a quad whose uv is linear across it.
void raster(const ukiyo::FontAtlas& font, Bitmap& img, const std::vector<ukiyo::HudVertex>& verts, int ox, int oy,
            glm::vec3 ink) {
    for (size_t q = 0; q + 5 < verts.size(); q += 6) {
        const ukiyo::HudVertex& tl = verts[q + 0];
        const ukiyo::HudVertex& tr = verts[q + 1];
        const ukiyo::HudVertex& bl = verts[q + 5];
        const float w = std::max(tr.pos.x - tl.pos.x, 1e-4f);
        const float h = std::max(bl.pos.y - tl.pos.y, 1e-4f);
        const float du = std::fabs(tr.uv.x - tl.uv.x) / w;
        const float dv = std::fabs(bl.uv.y - tl.uv.y) / h;
        const float uvPerPixel = std::max(0.5f * (du + dv), 1e-9f);
        for (int y = int(std::floor(tl.pos.y)); y < int(std::ceil(bl.pos.y)); ++y) {
            for (int x = int(std::floor(tl.pos.x)); x < int(std::ceil(tr.pos.x)); ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                if (px < tl.pos.x || px > tr.pos.x || py < tl.pos.y || py > bl.pos.y) continue;
                const float fu = (px - tl.pos.x) / w, fv = (py - tl.pos.y) / h;
                const float u = tl.uv.x + (tr.uv.x - tl.uv.x) * fu;
                const float v = tl.uv.y + (bl.uv.y - tl.uv.y) * fv;
                img.blend(ox + x, oy + y, ink, coverageAt(font, u, v, uvPerPixel));
            }
        }
    }
}

// Lays out one line with the shipping canvas at `size` pixels and draws it at (ox, oy).
void line(const ukiyo::FontAtlas& font, const std::string& text, float size, Bitmap& img, int ox, int oy,
          glm::vec3 ink) {
    ukiyo::HudCanvas canvas;
    canvas.setFont(const_cast<ukiyo::FontAtlas*>(&font));
    canvas.clear();
    canvas.text(0.0f, 0.0f, text, ukiyo::HudColor(1.0f, 1.0f, 1.0f), size);
    raster(font, img, canvas.vertices(), ox, oy, ink);
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

} // namespace

int main(int argc, char** argv) {
    std::string out = "docs/msdf.png";
    std::string fontPath = "editor/res/fonts/IBMPlexSansJP-Regular.ttf";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--font" && i + 1 < argc) fontPath = argv[++i];
    }

    ukiyo::FontAtlas flat;
    if (!flat.loadFromFile(fontPath, 20.0f, 1024)) {
        std::fprintf(stderr, "msdf_preview: could not load %s\n", fontPath.c_str());
        return 1;
    }
    flat.preloadAscii();
    ukiyo::FontAtlas field;
    if (!field.loadMsdfFromFile(fontPath, 40.0f, 6.0f, 1024)) {
        std::fprintf(stderr, "msdf_preview: could not load %s as a field\n", fontPath.c_str());
        return 1;
    }
    field.preloadAscii();
    std::printf("msdf: coverage page %dx%d at %.0f px; field page %dx%d at %.0f px, range %.0f texels\n", flat.width(),
                flat.height(), double(flat.pixelHeight()), field.width(), field.height(),
                double(field.pixelHeight()), double(field.msdfRange()));

    const int W = 1600, H = 1000;
    Bitmap sheet;
    sheet.reset(W, H, glm::vec3(0.045f, 0.05f, 0.06f));

    const std::string latin = "Ukiyo Engine - text at every size";
    const std::string jp = "\xE9\x9D\x99\xE3\x81\x91\xE3\x81\x95\xE5\x86\x99\xE7\x9C\x9F\xE3\x81\xAE\xE5\xB3\xB6";  // 静けさ写真の島

    figure::panelHeader(sheet, 30, 18, "COVERAGE PAGE, RASTERISED AT 20 PX", "THE PAGE DRAWN AT 12, 20 AND 40 PX");
    figure::panelHeader(sheet, 820, 18, "DISTANCE FIELD, GENERATED AT 40 PX", "THE PAGE DRAWN AT 12, 20 AND 40 PX");

    const float sizes[3] = {12.0f, 20.0f, 40.0f};
    const int yAt[3] = {96, 152, 214};
    for (int i = 0; i < 3; ++i) {
        char label[32];
        std::snprintf(label, sizeof label, "%d PX", int(sizes[i]));
        figure::drawText(sheet, 30, yAt[i] + 2, label, 1, glm::vec3(0.45f, 0.50f, 0.58f));
        figure::drawText(sheet, 820, yAt[i] + 2, label, 1, glm::vec3(0.45f, 0.50f, 0.58f));
        line(flat, latin, sizes[i], sheet, 90, yAt[i], glm::vec3(0.92f, 0.95f, 1.0f));
        line(field, latin, sizes[i], sheet, 880, yAt[i], glm::vec3(0.92f, 0.95f, 1.0f));
    }

    // Japanese: dense strokes, where a page rasterised at one size runs out of resolution first.
    figure::panelHeader(sheet, 30, 300, "JAPANESE AT 64 PX, FROM A 20 PX PAGE", "THE STROKES ARE THE PAGE'S PIXELS");
    line(flat, jp, 64.0f, sheet, 90, 348, glm::vec3(0.92f, 0.95f, 1.0f));
    figure::panelHeader(sheet, 820, 300, "JAPANESE AT 64 PX, FROM THE FIELD", "THE STROKES ARE THE OUTLINE");
    line(field, jp, 64.0f, sheet, 880, 348, glm::vec3(0.92f, 0.95f, 1.0f));

    // Latin at 96 px, where the difference stops being a matter of opinion.
    figure::panelHeader(sheet, 30, 440, "ONE LETTER AT 96 PX", "");
    line(flat, "W", 96.0f, sheet, 90, 482, glm::vec3(0.92f, 0.95f, 1.0f));
    figure::panelHeader(sheet, 820, 440, "ONE LETTER AT 96 PX", "");
    line(field, "W", 96.0f, sheet, 880, 482, glm::vec3(0.92f, 0.95f, 1.0f));

    // ---- measurements, from this run -----------------------------------------------------------------------------
    const std::vector<uint8_t> ttf = readFile(fontPath);
    stbtt_fontinfo stb{};
    double fieldError = 0.0, worstField = 0.0;
    int fieldCount = 0;
    double coverageAtSize[3] = {0.0, 0.0, 0.0};
    double fieldAtSize[3] = {0.0, 0.0, 0.0};
    double floorAtSize[3] = {0.0, 0.0, 0.0};
    if (!ttf.empty() && stbtt_InitFont(&stb, ttf.data(), 0)) {
        const double em = 40.0, range = 6.0;
        const double scale = stbtt_ScaleForPixelHeight(&stb, static_cast<float>(em));
        const int gi = stbtt_FindGlyphIndex(&stb, 'A');
        ukiyo::msdf::Params params;
        params.range = range;
        ukiyo::msdf::Field f;
        std::vector<ukiyo::msdf::Segment> shape;
        if (ukiyo::msdf::build(ttf.data(), ttf.size(), gi, scale, params, &f) &&
            ukiyo::msdf::outline(ttf.data(), ttf.size(), gi, scale, &shape)) {
            for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                    const double px = f.xoff + x + 0.5, py = -f.yoff - (y + 0.5);
                    const double exact = ukiyo::msdf::signedDistance(shape, px, py);
                    if (std::fabs(exact) > range - 0.75) continue;
                    const size_t at = (static_cast<size_t>(y) * f.width + x) * 4;
                    const double m = ukiyo::msdf::median(f.pixels[at] / 255.0, f.pixels[at + 1] / 255.0,
                                                         f.pixels[at + 2] / 255.0);
                    const double err = std::fabs((m - 0.5) * 2.0 * range - exact);
                    fieldError += err;
                    worstField = std::max(worstField, err);
                    ++fieldCount;
                }
        }
        // Three pictures of the same glyph, each compared with the rasterisation the requested size deserves: the
        // coverage page drawn at that size (which is what the engine did before), the field drawn at that size, and, as
        // the bar for both, that rasterisation shifted a quarter of a pixel — no two renderers of one outline agree
        // more closely than that at these sizes.
        for (int i = 0; i < 3; ++i) {
            const float s = stbtt_ScaleForPixelHeight(&stb, sizes[i]);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&stb, gi, s, s, &x0, &y0, &x1, &y1);
            const int rw = x1 - x0, rh = y1 - y0;
            if (rw <= 0 || rh <= 0) continue;
            std::vector<uint8_t> ref(static_cast<size_t>(rw) * rh), shifted(ref.size());
            stbtt_MakeGlyphBitmap(&stb, ref.data(), rw, rh, rw, s, s, gi);
            stbtt_MakeGlyphBitmapSubpixel(&stb, shifted.data(), rw, rh, rw, s, s, 0.25f, 0.25f, gi);

            // The page's own glyph: the cell it was rasterised into at 20 px, and where that cell sits in the atlas.
            const ukiyo::GlyphMetrics* page = const_cast<ukiyo::FontAtlas&>(flat).glyph('A');
            const double pageEm = double(flat.pixelHeight());
            const double s2 = sizes[i] / em;
            double eFlat = 0.0, eField = 0.0, eFloor = 0.0;
            for (int y = 0; y < rh; ++y) {
                for (int x = 0; x < rw; ++x) {
                    const double want = ref[static_cast<size_t>(y) * rw + x] / 255.0;
                    eFloor += std::fabs(want - shifted[static_cast<size_t>(y) * rw + x] / 255.0);

                    // Both pages were built from the same font at the same em, so a font-space point is the same point
                    // in both; the offsets and scales below only convert to each one's pixel grid.
                    const double targetX = x0 + x + 0.5, targetY = y0 + y + 0.5;   // pixels at `sizes[i]`

                    // The coverage page: one page pixel is (pageEm / sizes[i]) target pixels, magnified.
                    if (page && page->width > 0.0f && page->height > 0.0f) {
                        const double pageX = targetX * (pageEm / sizes[i]) - page->xoff;
                        const double pageY = targetY * (pageEm / sizes[i]) - page->yoff;
                        const float u = page->u0 + float(pageX / page->width) * (page->u1 - page->u0);
                        const float v = page->v0 + float(pageY / page->height) * (page->v1 - page->v0);
                        eFlat += std::fabs(sampleChannel(flat, u, v, 3) - want);
                    }

                    // The field: its cell starts at (xoff, yoff) in pixels at the generation em, so divide by the ratio.
                    const double fx = targetX / s2 - f.xoff;
                    const double fy = targetY / s2 - f.yoff;
                    auto sample = [&](int ch) {
                        const double px = fx - 0.5, py = fy - 0.5;
                        const int gx = int(std::floor(px)), gy = int(std::floor(py));
                        const double ax = px - gx, ay = py - gy;
                        auto t = [&](int tx, int ty) {
                            tx = std::clamp(tx, 0, f.width - 1);
                            ty = std::clamp(ty, 0, f.height - 1);
                            return f.pixels[(static_cast<size_t>(ty) * f.width + tx) * 4 + ch] / 255.0;
                        };
                        const double a = t(gx, gy) * (1 - ax) + t(gx + 1, gy) * ax;
                        const double b = t(gx, gy + 1) * (1 - ax) + t(gx + 1, gy + 1) * ax;
                        return a * (1 - ay) + b * ay;
                    };
                    const double cov = ukiyo::msdf::coverageFromField(sample(0), sample(1), sample(2),
                                                                     range / double(f.width), 1.0 / (f.width * s2),
                                                                     1.0 / (f.width * s2));
                    eField += std::fabs(cov - want);
                }
            }
            coverageAtSize[i] = eFlat / double(rw * rh);
            fieldAtSize[i] = eField / double(rw * rh);
            floorAtSize[i] = eFloor / double(rw * rh);
        }
    }
    std::printf("msdf: field vs exact outline: mean %.3f texels over %d, worst %.3f\n",
                fieldCount ? fieldError / fieldCount : 0.0, fieldCount, worstField);
    for (int i = 0; i < 3; ++i)
        std::printf("msdf: %2d px  coverage page %.4f  field %.4f  (a quarter-pixel shift moves the reference %.4f)\n",
                    int(sizes[i]), coverageAtSize[i], fieldAtSize[i], floorAtSize[i]);

    // ---- the channel view, and the numbers ----------------------------------------------------------------------
    const int cx = 30, cy = 700, cw = 150, ch = 180;
    figure::panelHeader(sheet, cx, cy - 40, "WHAT THE FIELD STORES", "THREE DISTANCES, THEN THE MEDIAN");
    const ukiyo::GlyphMetrics* g = const_cast<ukiyo::FontAtlas&>(field).glyph('R');
    if (g) {
        const int pw = field.width();
        const int gx0 = int(g->u0 * float(pw)), gy0 = int(g->v0 * float(pw));
        const int gw = int(g->width), gh = int(g->height);
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const int sx = gx0 + x * gw / cw, sy = gy0 + y * gh / ch;
                const uint8_t* px = &field.pixels()[(static_cast<size_t>(sy) * pw + sx) * 4];
                sheet.color[static_cast<size_t>(cy + y) * W + cx + x] =
                    glm::vec3(px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f);
                sheet.color[static_cast<size_t>(cy + y) * W + cx + cw + 24 + x] =
                    glm::vec3(0.92f, 0.95f, 1.0f) * (px[3] / 255.0f);
            }
        }
        figure::drawText(sheet, cx, cy + ch + 8, "RGB", 1, glm::vec3(0.55f, 0.60f, 0.68f));
        figure::drawText(sheet, cx + cw + 24, cy + ch + 8, "MEDIAN", 1, glm::vec3(0.55f, 0.60f, 0.68f));
    }

    // The numbers, as this run printed them.
    char buf[160];
    int ty = cy - 44;
    const int tx = 430;
    figure::drawText(sheet, tx, ty, "MEASURED, THIS RUN", 2, glm::vec3(0.85f, 0.88f, 0.93f));
    ty += 34;
    std::snprintf(buf, sizeof buf, "FIELD VS THE EXACT OUTLINE   %.3f TEXELS", fieldCount ? fieldError / fieldCount : 0.0);
    figure::drawText(sheet, tx, ty, buf, 1, glm::vec3(0.35f, 0.85f, 0.95f));
    ty += 20;
    std::snprintf(buf, sizeof buf, "WORST TEXEL                 %.3f TEXELS", worstField);
    figure::drawText(sheet, tx, ty, buf, 1, glm::vec3(0.55f, 0.60f, 0.68f));
    ty += 34;
    figure::drawText(sheet, tx, ty, "AGAINST A RASTERISATION AT THAT SIZE, AND THE", 1, glm::vec3(0.85f, 0.88f, 0.93f));
    ty += 16;
    figure::drawText(sheet, tx, ty, "SAME GLYPH A QUARTER PIXEL LOWER", 1, glm::vec3(0.85f, 0.88f, 0.93f));
    ty += 24;
    for (int i = 0; i < 3; ++i) {
        std::snprintf(buf, sizeof buf, "%2d PX   COVERAGE %.3f   FIELD %.3f   QUARTER PIXEL %.3f", int(sizes[i]),
                      coverageAtSize[i], fieldAtSize[i], floorAtSize[i]);
        figure::drawText(sheet, tx, ty, buf, 1, i == 1 ? glm::vec3(0.35f, 0.85f, 0.95f) : glm::vec3(0.62f, 0.68f, 0.76f));
        ty += 18;
    }
    ty += 10;
    figure::drawText(sheet, tx, ty, "AT 20 PX THE COVERAGE PAGE IS THE RASTERISER ITSELF.", 1,
                     glm::vec3(0.50f, 0.55f, 0.63f));
    ty += 14;
    figure::drawText(sheet, tx, ty, "EVERYWHERE ELSE IT IS A BLOWN-UP BITMAP.", 1, glm::vec3(0.50f, 0.55f, 0.63f));
    ty += 20;
    std::snprintf(buf, sizeof buf, "PAGE %d X %d, RANGE %.0f TEXELS, GENERATED AT %.0f PX", field.width(), field.height(),
                  double(field.msdfRange()), double(field.pixelHeight()));
    figure::drawText(sheet, tx, ty, buf, 1, glm::vec3(0.50f, 0.55f, 0.63f));

    if (!figure::savePng(sheet, out)) {
        std::fprintf(stderr, "msdf_preview: could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", out.c_str(), W, H);
    return 0;
}
