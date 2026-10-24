// Offline figure for docs/ALPHA_MIPS.md: what an alpha-tested cut-out looks like with plain mipmaps and with
// alpha-coverage-preserving mipmaps (Godot 4.8's GH-104289).
//
// The scene is a cut-out texture on a ground plane, receding to the horizon, sampled trilinearly and alpha tested at 0.5 —
// the same filtering the GPU does. On the left the chain is the plain box filter, so the alpha averages towards the
// texture's mean and the far field falls under the threshold and disappears. On the right every level's alpha has been
// scaled to hold the level-0 coverage, so the cut-out survives.
//
// The footer plots the measured coverage per level for both chains, and prints the numbers the doc quotes. Both chains and
// both coverage series come from src/engine/texture_mips.cpp — the same code the loader runs.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party -I third_party/glm tools/alpha_mips_preview.cpp src/engine/texture_mips.cpp
//         -o build/alpha_mips_preview
// Run:
//     ./build/alpha_mips_preview docs/alpha-mips.png 1600 1000
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <glm/glm.hpp>

#include "engine/texture_mips.h"

using namespace ukiyo;

namespace {

// ---- texture -------------------------------------------------------------------------------

float valueNoise(int x, int y, int period, int seed) {
    auto hash = [&](int ix, int iy) {
        unsigned h = unsigned(ix) * 374761393u + unsigned(iy) * 668265263u + unsigned(seed) * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
    };
    const int x0 = x / period, y0 = y / period;
    const float fx = float(x % period) / float(period);
    const float fy = float(y % period) / float(period);
    const float a = hash(x0, y0), b = hash(x0 + 1, y0), c = hash(x0, y0 + 1), d = hash(x0 + 1, y0 + 1);
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    return glm::mix(glm::mix(a, b, sx), glm::mix(c, d, sx), sy);
}

// A net: two families of thin strands on a 12-texel grid, with a sparse speckle so the pattern is not perfectly regular.
// The strands are 2 texels wide, so a mip level averages them below the 0.5 test within two levels — that is the cut-out
// dissolving — while the coverage (about a third) is what the preserved chain has to hold.
std::vector<uint8_t> makeCutout(int w, int h, float* coverageHint) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    int on = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool strand = (x % 12) < 2 || (y % 12) < 2 || ((x + y) % 19) < 1;
            const bool speck = valueNoise(x, y, 3, 11) > 0.86f;
            const bool opaque = strand || speck;
            if (opaque) ++on;
            const size_t i = (size_t(y) * w + x) * 4;
            const float vein = 0.8f + 0.2f * std::sin(float(x) * 0.7f + float(y) * 0.25f);
            img[i + 0] = uint8_t(std::clamp(58.0f * vein + 38.0f, 0.0f, 255.0f));
            img[i + 1] = uint8_t(std::clamp(142.0f * vein + 40.0f, 0.0f, 255.0f));
            img[i + 2] = uint8_t(std::clamp(52.0f * vein + 28.0f, 0.0f, 255.0f));
            // Binary alpha: the shape is in the mask, not in a soft edge, which is what a cut-out texture looks like and
            // what makes averaging it below the threshold lose the surface.
            img[i + 3] = opaque ? 255 : 0;
        }
    }
    if (coverageHint) *coverageHint = float(on) / float(w * h);
    return img;
}

// ---- sampling ------------------------------------------------------------------------------

glm::vec4 texel(const MipLevel& l, int x, int y) {
    x = std::clamp(x, 0, int(l.width) - 1);
    y = std::clamp(y, 0, int(l.height) - 1);
    const uint8_t* p = l.rgba.data() + (size_t(y) * l.width + size_t(x)) * 4u;
    return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
}

glm::vec4 bilinear(const MipLevel& l, float u, float v) {
    const float x = u * float(l.width) - 0.5f;
    const float y = v * float(l.height) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    const glm::vec4 a = texel(l, x0, y0), b = texel(l, x0 + 1, y0);
    const glm::vec4 c = texel(l, x0, y0 + 1), d = texel(l, x0 + 1, y0 + 1);
    return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

glm::vec4 sampleTrilinear(const std::vector<MipLevel>& chain, float u, float v, float lod) {
    lod = std::clamp(lod, 0.0f, float(chain.size()) - 1.001f);
    const int l0 = int(std::floor(lod));
    const float f = lod - float(l0);
    const glm::vec4 a = bilinear(chain[size_t(l0)], u, v);
    if (l0 + 1 >= int(chain.size())) return a;
    const glm::vec4 b = bilinear(chain[size_t(l0) + 1], u, v);
    return glm::mix(a, b, f);
}

// ---- image and text ------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<glm::vec3> c;
    void reset(int width, int height) {
        w = width;
        h = height;
        c.assign(size_t(w) * h, glm::vec3(0.0f));
    }
};

const uint8_t kGlyphRows[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},      {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},   {6, 8, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 12, 12},      {1, 1, 2, 4, 8, 16, 16},      {0, 12, 12, 0, 12, 12, 0},
    {0, 0, 0, 31, 0, 0, 0},       {0, 4, 4, 31, 4, 4, 0},       {25, 26, 4, 8, 8, 22, 19},
};

int glyphIndex(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= '0' && ch <= '9') return 26 + (ch - '0');
    switch (ch) {
        case '.': return 36;
        case '/': return 37;
        case ':': return 38;
        case '-': return 39;
        case '+': return 40;
        case '%': return 41;
        default: return -1;
    }
}

void fillRect(Image& img, int x0, int y0, int rw, int rh, glm::vec3 c, float alpha = 1.0f) {
    for (int y = std::max(0, y0); y < std::min(img.h, y0 + rh); ++y)
        for (int x = std::max(0, x0); x < std::min(img.w, x0 + rw); ++x)
            img.c[size_t(y) * img.w + x] = glm::mix(img.c[size_t(y) * img.w + x], c, alpha);
}

int textWidth(const std::string& text, int scale) {
    int w = 0;
    for (char ch : text) w += (ch == ' ') ? 4 * scale : 6 * scale;
    return w > 0 ? w - scale : 0;
}

void drawText(Image& img, int x, int y, const std::string& text, int scale, glm::vec3 c) {
    for (char ch : text) {
        if (ch == ' ') {
            x += 4 * scale;
            continue;
        }
        const int g = glyphIndex(ch);
        if (g < 0) {
            x += 6 * scale;
            continue;
        }
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (kGlyphRows[g][row] & (1u << (4 - col)))
                    fillRect(img, x + col * scale, y + row * scale, scale, scale, c);
        x += 6 * scale;
    }
}

glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

void drawLine(Image& img, int x0, int y0, int x1, int y1, glm::vec3 c, int thickness = 2) {
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    for (int i = 0; i <= steps; ++i) {
        const float t = steps ? float(i) / float(steps) : 0.0f;
        const int x = int(glm::mix(float(x0), float(x1), t));
        const int y = int(glm::mix(float(y0), float(y1), t));
        fillRect(img, x - thickness / 2, y - thickness / 2, thickness, thickness, c);
    }
}

// ---- the receding plane ---------------------------------------------------------------------

struct Camera {
    glm::vec3 eye{0.0f, 1.6f, -2.6f};
    float pitch = -0.34f;   // radians, down
    float fovY = 0.95f;
};

// One panel: a ground plane textured with the cut-out, alpha tested at 0.5, sampled from a FIXED mip level. Fixing the
// level is what makes the comparison legible: it is exactly what the GPU does for a surface whose footprint sits at that
// level, without the per-pixel LOD banding that would otherwise hide the effect.
std::vector<glm::vec3> renderPlane(const std::vector<MipLevel>& chain, int w, int h, float tiling, float lod,
                                   const Camera& cam, const glm::vec3& ground, const glm::vec3& sky) {
    std::vector<glm::vec3> out(size_t(w) * h);
    const float tanH = std::tan(cam.fovY * 0.5f);
    const glm::vec3 fwd(0.0f, std::sin(cam.pitch), std::cos(cam.pitch));
    const glm::vec3 right(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = glm::cross(fwd, right);   // (0, cos(pitch), -sin(pitch)): screen up

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (2.0f * (float(x) + 0.5f) / float(w) - 1.0f) * tanH * (float(w) / float(h));
            const float ny = (1.0f - 2.0f * (float(y) + 0.5f) / float(h)) * tanH;
            const glm::vec3 dir = glm::normalize(fwd + right * nx + up * ny);
            const glm::vec3 skyCol = glm::mix(sky * 0.72f, sky, float(y) / float(h));
            glm::vec3 c = skyCol;
            if (dir.y < -1e-4f) {
                const float t = -cam.eye.y / dir.y;
                const glm::vec3 p = cam.eye + dir * t;
                const glm::vec4 s = sampleTrilinear(chain, p.x * tiling, p.z * tiling, lod);
                c = s.a > 0.5f ? glm::vec3(s.r, s.g, s.b) * 0.9f : ground;
                // Aerial perspective towards the horizon, so the far field does not shout.
                const float haze = std::clamp((t - 5.0f) / 22.0f, 0.0f, 0.6f);
                c = glm::mix(c, skyCol, haze);
            }
            out[size_t(y) * w + x] = c;
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const std::string outPath = argc > 1 ? argv[1] : "docs/alpha-mips.png";
    const int outW = argc > 2 ? std::atoi(argv[2]) : 1600;
    const int outH = argc > 3 ? std::atoi(argv[3]) : 1000;

    const int texSize = 256;
    float density = 0.0f;
    const std::vector<uint8_t> img = makeCutout(texSize, texSize, &density);

    const std::vector<MipLevel> naive = buildMipChain(img.data(), texSize, texSize, false);
    const std::vector<MipLevel> kept = buildMipChain(img.data(), texSize, texSize, true, 0.5f);
    const float c0 = alphaTestCoverage(img.data(), texSize, texSize, 0.5f);

    // Coverage per level, the series the footer plots.
    std::vector<float> covNaive(naive.size()), covKept(kept.size());
    for (size_t i = 0; i < naive.size(); ++i) {
        covNaive[i] = alphaTestCoverage(naive[i].rgba.data(), naive[i].width, naive[i].height, 0.5f);
        covKept[i] = alphaTestCoverage(kept[i].rgba.data(), kept[i].width, kept[i].height, 0.5f);
    }

    const int strip = 104;
    const int footer = 236;
    const int rowH = (outH - strip - footer) / 2;
    const int labelW = 152;                       // the row labels down the left edge
    const int colW = (outW - labelW) / 4;         // four mip levels
    const int lods[4] = {2, 3, 4, 5};

    Image sheet;
    sheet.reset(outW, outH);
    fillRect(sheet, 0, 0, outW, outH, glm::vec3(0.035f, 0.040f, 0.050f));

    const glm::vec3 title(0.95f, 0.96f, 0.99f);
    const glm::vec3 body(0.60f, 0.67f, 0.76f);
    const glm::vec3 dim(0.42f, 0.48f, 0.57f);
    const glm::vec3 ground(0.30f, 0.24f, 0.18f);
    const glm::vec3 sky(0.55f, 0.68f, 0.82f);
    const Camera cam;

    for (int row = 0; row < 2; ++row) {
        const std::vector<MipLevel>& chain = row == 0 ? naive : kept;
        const int y0 = strip + row * rowH;
        for (int col = 0; col < 4; ++col) {
            // 1.4 repeats per metre: the 12-texel mesh is about 4 cm, so each column shows the level it names.
            const std::vector<glm::vec3> px = renderPlane(chain, colW, rowH, 0.35f, float(lods[col]), cam, ground, sky);
            for (int y = 0; y < rowH; ++y)
                for (int x = 0; x < colW; ++x)
                    sheet.c[size_t(y0 + y) * outW + (labelW + col * colW + x)] =
                        toSrgb(px[size_t(y) * colW + x]);
            // Column caption: the level and what survives it.
            char cap[96];
            std::snprintf(cap, sizeof(cap), "LOD %d - %.3f", lods[col],
                          double(row == 0 ? covNaive[size_t(lods[col])] : covKept[size_t(lods[col])]));
            fillRect(sheet, labelW + col * colW + 12, y0 + 12, textWidth(cap, 2) + 16, 30, glm::vec3(0.02f, 0.02f, 0.03f),
                     0.66f);
            drawText(sheet, labelW + col * colW + 20, y0 + 20, cap, 2, row == 0 ? glm::vec3(0.95f, 0.72f, 0.66f)
                                                                                : glm::vec3(0.72f, 0.93f, 0.76f));
            if (col > 0) fillRect(sheet, labelW + col * colW - 1, y0, 2, rowH, glm::vec3(0.16f, 0.19f, 0.24f));
        }
        fillRect(sheet, 0, y0, labelW - 1, rowH, glm::vec3(0.075f, 0.085f, 0.10f));
        const glm::vec3 rowCol = row == 0 ? glm::vec3(0.93f, 0.62f, 0.55f) : glm::vec3(0.60f, 0.90f, 0.66f);
        drawText(sheet, 20, y0 + 26, row == 0 ? "PLAIN" : "COVERAGE", 2, rowCol);
        drawText(sheet, 20, y0 + 48, row == 0 ? "MIPS" : "PRESERVED", 2, rowCol);
        drawText(sheet, 20, y0 + 78, row == 0 ? "BOX FILTER" : "ALPHA SCALED", 1, dim);
        drawText(sheet, 20, y0 + 92, row == 0 ? "ALONE" : "PER LEVEL", 1, dim);
        if (row == 0) fillRect(sheet, 0, y0 + rowH - 2, outW, 2, glm::vec3(0.16f, 0.19f, 0.24f));
    }

    // Header.
    drawText(sheet, 26, 18, "ALPHA-COVERAGE-PRESERVING MIPMAPS", 3, title);
    drawText(sheet, 26, 48, "GH-104289 - SRC/ENGINE/TEXTURE_MIPS.CPP - DOCS/ALPHA_MIPS.MD", 2, body);
    char line[256];
    std::snprintf(line, sizeof(line), "LEVEL 0 COVERAGE %.3f - %d LEVELS - ALPHA TEST 0.50", double(c0),
                  int(naive.size()));
    drawText(sheet, outW - 26 - textWidth(line, 2), 26, line, 2, glm::vec3(0.78f, 0.85f, 0.93f));
    std::snprintf(line, sizeof(line), "PLAIN AT LOD 4: %.4f OF THE SURFACE LEFT", double(covNaive[4]));
    drawText(sheet, outW - 26 - textWidth(line, 2), 52, line, 2, glm::vec3(0.85f, 0.55f, 0.50f));
    std::snprintf(line, sizeof(line), "PRESERVED AT LOD 4: %.4f", double(covKept[4]));
    drawText(sheet, outW - 26 - textWidth(line, 2), 78, line, 2, glm::vec3(0.55f, 0.82f, 0.62f));

    // Footer: the coverage plot on the left, the reading on the right.
    const int fy = strip + rowH * 2;   // == outH - footer
    fillRect(sheet, 0, fy, outW, footer, glm::vec3(0.055f, 0.062f, 0.075f));
    fillRect(sheet, 0, fy, outW, 2, glm::vec3(0.16f, 0.19f, 0.24f));

    const int px0 = 60, py0 = fy + 58, pw = 400, ph = 92;
    fillRect(sheet, px0 - 12, py0 - 30, pw + 24, ph + 52, glm::vec3(0.10f, 0.12f, 0.15f));
    drawText(sheet, px0 - 2, fy + 12, "COVERAGE PER MIP LEVEL", 2, body);
    drawText(sheet, px0 + 2, fy + 32, "GREEN: PRESERVED - RED: PLAIN - DOTTED: LEVEL 0", 1, dim);
    drawLine(sheet, px0, py0 + ph, px0 + pw, py0 + ph, dim, 1);
    drawLine(sheet, px0, py0, px0, py0 + ph, dim, 1);
    auto plotX = [&](size_t i) { return px0 + int(float(i) / float(naive.size() - 1) * float(pw)); };
    auto plotY = [&](float c) { return py0 + ph - int(std::clamp(c, 0.0f, 1.0f) * float(ph)); };
    for (size_t i = 1; i < naive.size(); ++i) {
        drawLine(sheet, plotX(i - 1), plotY(covNaive[i - 1]), plotX(i), plotY(covNaive[i]),
                 glm::vec3(0.85f, 0.42f, 0.36f), 2);
        drawLine(sheet, plotX(i - 1), plotY(covKept[i - 1]), plotX(i), plotY(covKept[i]),
                 glm::vec3(0.45f, 0.80f, 0.55f), 2);
    }
    drawLine(sheet, px0, plotY(c0), px0 + pw, plotY(c0), glm::vec3(0.50f, 0.56f, 0.66f), 1);
    drawText(sheet, px0 + 4, plotY(c0) - 16, "LEVEL 0", 1, dim);
    drawText(sheet, px0 - 2, py0 + ph + 8, "L0", 1, dim);
    drawText(sheet, px0 + pw - 14, py0 + ph + 8, "L8", 1, dim);

    const int tx = px0 + pw + 56;
    int ty = fy + 12;
    drawText(sheet, tx, ty, "READING THE FIGURE", 2, title);
    ty += 30;
    drawText(sheet, tx, ty, "ONE MIP LEVEL PER COLUMN, ONE CHAIN PER ROW - WHAT THE", 2, body);
    ty += 20;
    drawText(sheet, tx, ty, "GPU DOES WHEN A SURFACE'S FOOTPRINT SITS AT THAT LEVEL.", 2, body);
    ty += 28;
    drawText(sheet, tx, ty, "PLAIN MIPS AVERAGE THE HOLES INTO THE ALPHA, SO EVERY", 2, body);
    ty += 20;
    drawText(sheet, tx, ty, "LEVEL IS THINNER THAN THE LAST: BY LOD 4 THE CUT-OUT HAS", 2, body);
    ty += 20;
    drawText(sheet, tx, ty, "FALLEN UNDER THE 0.50 CUTOFF AND GONE.", 2, body);
    ty += 26;
    drawText(sheet, tx, ty, "THE PRESERVED CHAIN RESCALES EACH LEVEL'S ALPHA TO HOLD", 2, body);
    ty += 20;
    drawText(sheet, tx, ty, "THE LEVEL-0 COVERAGE, SO THE MESH STAYS AS DENSE AT", 2, body);
    ty += 20;
    drawText(sheet, tx, ty, "DISTANCE AS IT IS CLOSE UP. RGB IS NEVER TOUCHED.", 2, body);

    std::vector<uint8_t> bytes(size_t(outW) * outH * 3);
    for (size_t i = 0; i < sheet.c.size(); ++i) {
        const glm::vec3 c = glm::clamp(sheet.c[i], glm::vec3(0.0f), glm::vec3(1.0f));
        bytes[i * 3 + 0] = uint8_t(c.x * 255.0f + 0.5f);
        bytes[i * 3 + 1] = uint8_t(c.y * 255.0f + 0.5f);
        bytes[i * 3 + 2] = uint8_t(c.z * 255.0f + 0.5f);
    }
    if (!stbi_write_png(outPath.c_str(), outW, outH, 3, bytes.data(), outW * 3)) {
        std::fprintf(stderr, "could not write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)  c0 %.4f  naive L4 %.4f  preserved L4 %.4f\n", outPath.c_str(), outW, outH, double(c0),
                double(covNaive[4]), double(covKept[4]));
    return 0;
}
