// The launcher icon, drawn by a committed tool.
//
// An APK's icon is a set of PNGs at five densities, all of the same drawing. Hand-drawn art would be five files that
// drift apart the moment the mark changes; this is one drawing, rasterised five times, in the same spirit as the other
// figures in docs/ — a committed program that produces a committed PNG.
//
// The mark is the identity's own (tools/brand.py, docs/brand): the ensō, one breath drawn with a brush that lands heavy
// at the start and runs dry at the end, with the vermilion seal of 浮 sitting in the gap the brush left. On a phone the
// ring is what is recognised at 48 px; the seal is what carries the name at 192.
//
//   tools/android_icon                  # writes android/res/mipmap-*/ic_launcher.png and docs/icon.png
//   tools/android_icon --out android/res --font editor/res/fonts/ShipporiMinchoDisplay-Bold.ttf
//
// Everything is drawn at four times the size and filtered down, so the ring's edge and the brush's taper stay clean at
// every density. No OpenGL, no Qt: the same "runs anywhere" rule as the other tools.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

// The identity's colours, from tools/brand.py.
const float kIndigo[3] = {26.0f / 255.0f, 40.0f / 255.0f, 68.0f / 255.0f};     // 藍
const float kInk[3] = {241.0f / 255.0f, 235.0f / 255.0f, 223.0f / 255.0f};     // warm paper white
const float kVermilion[3] = {204.0f / 255.0f, 72.0f / 255.0f, 50.0f / 255.0f}; // 朱

constexpr int kSuper = 4;   // the supersampling factor

struct Canvas {
    int w = 0, h = 0;
    std::vector<float> rgb;   // three floats per pixel, 0..1

    void reset(int width, int height, const float c[3]) {
        w = width;
        h = height;
        rgb.assign(size_t(w) * size_t(h) * 3, 0.0f);
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
            rgb[i * 3 + 0] = c[0];
            rgb[i * 3 + 1] = c[1];
            rgb[i * 3 + 2] = c[2];
        }
    }
    void blend(int x, int y, const float c[3], float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
        if (a > 1.0f) a = 1.0f;
        float* p = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        for (int k = 0; k < 3; ++k) p[k] = p[k] * (1.0f - a) + c[k] * a;
    }
};

// One dot of the brush. The arc below is drawn as a great many of these, so the bristle taper is just the radius of the
// dot changing along the stroke - which is exactly what a brush does.
void dot(Canvas& c, float cx, float cy, float r, const float col[3]) {
    if (r <= 0.0f) return;
    const int x0 = int(std::floor(cx - r - 1.0f)), x1 = int(std::ceil(cx + r + 1.0f));
    const int y0 = int(std::floor(cy - r - 1.0f)), y1 = int(std::ceil(cy + r + 1.0f));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > r + 0.7f) continue;
            // A one-pixel soft edge even before the supersample: the ring is a brush, not a vector stroke.
            c.blend(x, y, col, std::clamp(r + 0.5f - d, 0.0f, 1.0f));
        }
    }
}

// A rounded rectangle, by distance to the rounded box. `r` is the corner radius.
void roundedRect(Canvas& c, float cx, float cy, float halfW, float halfH, float r, const float col[3]) {
    const int x0 = int(std::floor(cx - halfW - 1.0f)), x1 = int(std::ceil(cx + halfW + 1.0f));
    const int y0 = int(std::floor(cy - halfH - 1.0f)), y1 = int(std::ceil(cy + halfH + 1.0f));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = std::fabs(float(x) + 0.5f - cx) - (halfW - r);
            const float py = std::fabs(float(y) + 0.5f - cy) - (halfH - r);
            const float ox = std::max(px, 0.0f), oy = std::max(py, 0.0f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(px, py), 0.0f) - r;
            c.blend(x, y, col, std::clamp(0.5f - d, 0.0f, 1.0f));
        }
    }
}

constexpr float kPi = 3.14159265358979f;

// The ensō: one stroke, drawn from `fromDeg` to `toDeg`, heavy where it starts and dry where it runs out. The gap the
// stroke does not close is where the seal sits.
void enso(Canvas& c, float cx, float cy, float radius, const float col[3], float fromDeg, float toDeg) {
    const float span = toDeg - fromDeg;
    const int steps = std::max(64, int(std::fabs(span) * radius * 0.06f));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const float a = (fromDeg + span * t) * kPi / 180.0f;
        // Heavy at the start (t=0), running dry by the end: a taper, then a lift of the brush over the last eighth so
        // the stroke leaves the paper as a point rather than stopping as a cut.
        const float taper = 0.16f + 0.84f * std::pow(1.0f - t, 0.60f);
        const float lift = t > 0.86f ? std::pow((1.0f - t) / 0.14f, 0.7f) : 1.0f;
        const float rr = radius * (1.0f - 0.02f * t);   // a hair of spiral, so it reads as a breath and not a circle
        const float width = radius * 0.19f * taper * std::clamp(lift, 0.0f, 1.0f);
        dot(c, cx + std::cos(a) * rr, cy + std::sin(a) * rr, width, col);
        // A little dry-brush texture: sparse dots beside the stroke, thinning with the taper.
        if (i % 3 == 0 && t > 0.25f) {
            const float off = radius * 0.09f * (1.0f - t);
            for (int k = -1; k <= 1; k += 2) {
                const float wobble = std::sin(t * 37.0f + float(k)) * radius * 0.012f;
                dot(c, cx + std::cos(a) * (rr + off * float(k) + wobble),
                    cy + std::sin(a) * (rr + off * float(k) + wobble), width * 0.28f, col);
            }
        }
    }
}

// The character on the seal. stb_truetype renders it at the seal's size; a font that will not load leaves the seal
// plain, which still reads (a hanko with a worn face is a real thing, a crash is not).
bool drawGlyph(Canvas& c, const std::string& fontPath, const char* utf8, float cx, float cy, float box,
               const float col[3]) {
    std::vector<unsigned char> ttf;
    if (FILE* f = std::fopen(fontPath.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0) {
            ttf.resize(size_t(size));
            if (std::fread(ttf.data(), 1, ttf.size(), f) != ttf.size()) ttf.clear();
        }
        std::fclose(f);
    }
    if (ttf.empty()) return false;

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) return false;

    // The codepoint of 浮 (U+6D6E).
    unsigned int cp = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
    if (p[0] < 0x80) cp = p[0];
    else if ((p[0] & 0xE0) == 0xC0) cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    else if ((p[0] & 0xF0) == 0xE0) cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    else if ((p[0] & 0xF8) == 0xF0)
        cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
    const int glyph = stbtt_FindGlyphIndex(&info, int(cp));
    if (glyph == 0) return false;

    const float scale = stbtt_ScaleForPixelHeight(&info, box);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, glyph, &adv, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0, gh = y1 - y0;
    if (gw <= 0 || gh <= 0) return false;

    std::vector<unsigned char> mask(size_t(gw) * size_t(gh), 0);
    stbtt_MakeGlyphBitmap(&info, mask.data(), gw, gh, gw, scale, scale, glyph);

    // The ink box is what is centred. x0/y0 describe where the ink sits relative to the pen, which is already
    // accounted for by the crop stb takes; adding them again pushes the character off the seal.
    const int left = int(std::lround(cx - float(gw) * 0.5f));
    const int top = int(std::lround(cy - float(gh) * 0.5f));
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x)
            c.blend(left + x, top + y, col, float(mask[size_t(y) * size_t(gw) + size_t(x)]) / 255.0f);
    return true;
}

// Box filter from the supersampled canvas down to `size`.
std::vector<unsigned char> downsample(const Canvas& c, int size) {
    std::vector<unsigned char> out(size_t(size) * size_t(size) * 3, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float sum[3] = {0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < kSuper; ++sy) {
                for (int sx = 0; sx < kSuper; ++sx) {
                    const size_t i = (size_t(y * kSuper + sy) * size_t(c.w) + size_t(x * kSuper + sx)) * 3;
                    sum[0] += c.rgb[i + 0];
                    sum[1] += c.rgb[i + 1];
                    sum[2] += c.rgb[i + 2];
                }
            }
            const float n = float(kSuper * kSuper);
            const size_t o = (size_t(y) * size_t(size) + size_t(x)) * 3;
            for (int k = 0; k < 3; ++k) {
                // The colours above are stored as sRGB values (26/255 is 藍 as specified), so there is no transfer
                // function left to apply: converting again would wash the indigo out to slate and turn the vermilion
                // pale, which is exactly what a figure tool that forgets this looks like.
                const float v = std::clamp(sum[k] / n, 0.0f, 1.0f);
                out[o + size_t(k)] = static_cast<unsigned char>(std::lround(v * 255.0f));
            }
        }
    }
    return out;
}

// The whole icon at one size: the indigo ground, the ring, and the seal in the ring's gap.
bool drawIcon(const std::string& fontPath, int size, const std::string& outPath) {
    const int S = size * kSuper;
    Canvas c;
    c.reset(S, S, kIndigo);

    // A ground that is very slightly lighter at the centre, so a 48 px icon is not a flat square of one colour.
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const float dx = (float(x) - float(S) * 0.5f) / float(S);
            const float dy = (float(y) - float(S) * 0.5f) / float(S);
            const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;   // 0 at the centre, 1 at the edge
            const float lift = 0.055f * std::max(0.0f, 1.0f - r * r);
            float warm[3] = {kIndigo[0] + lift, kIndigo[1] + lift * 0.95f, kIndigo[2] + lift * 0.8f};
            c.blend(x, y, warm, 0.0f);   // colours only; the blend below is what writes

            float* p = &c.rgb[(size_t(y) * size_t(S) + size_t(x)) * 3];
            p[0] = warm[0];
            p[1] = warm[1];
            p[2] = warm[2];
        }
    }

    // Launchers on Android 8 and later mask the icon, so the ring stays inside the middle of the square; the ground can
    // go all the way to the edges.
    const float centre = float(S) * 0.5f;
    const float radius = float(S) * 0.30f;
    // One stroke, clockwise, from the lower left round to the lower right, stopping short: the gap it leaves is where
    // the seal goes. Angles are measured with y downwards, so 130 degrees is lower left and 85 is lower right.
    const float startDeg = 128.0f;
    const float endDeg = startDeg + 296.0f;   // 64 degrees, leaving a 64 degree gap under the ring
    enso(c, centre, centre * 0.99f, radius, kInk, startDeg, endDeg);

    // The seal in the gap: vermilion, a little rounded, with 浮 in paper white. It sits in the middle of the gap and
    // slightly outside the ring's radius, so the stroke's two ends frame it instead of running under it.
    const float sealR = float(S) * 0.108f;
    const float gapAngle = (startDeg + 296.0f + 64.0f * 0.5f) * kPi / 180.0f;
    const float sealX = centre + std::cos(gapAngle) * radius * 0.96f;
    const float sealY = centre * 0.99f + std::sin(gapAngle) * radius * 0.96f;
    roundedRect(c, sealX, sealY, sealR, sealR, sealR * 0.14f, kVermilion);
    // The character, at two thirds of the seal: a hanko is mostly margin.
    if (!fontPath.empty()) drawGlyph(c, fontPath, "\xE6\xB5\xAE", sealX, sealY, sealR * 1.45f, kInk);

    const std::vector<unsigned char> rgba = downsample(c, size);
    if (!stbi_write_png(outPath.c_str(), size, size, 3, rgba.data(), size * 3)) {
        std::fprintf(stderr, "could not write %s\n", outPath.c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string out = "android/res";
    std::string font = "editor/res/fonts/ShipporiMinchoDisplay-Bold.ttf";
    std::string docIcon;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--font" && i + 1 < argc) font = argv[++i];
        else if (a == "--doc" && i + 1 < argc) docIcon = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--out dir] [--font file] [--doc file]\n", argv[0]);
            return 2;
        }
    }

    // The five densities Android asks for, and the 512 px image a store listing wants.
    struct Density {
        const char* dir;
        int size;
    } densities[] = {{"mipmap-mdpi", 48}, {"mipmap-hdpi", 72}, {"mipmap-xhdpi", 96},
                     {"mipmap-xxhdpi", 144}, {"mipmap-xxxhdpi", 192}};
    for (const Density& d : densities) {
        const std::string dir = out + "/" + d.dir;
        std::string cmd = "mkdir -p '" + dir + "'";
        if (std::system(cmd.c_str()) != 0) {
            std::fprintf(stderr, "could not create %s\n", dir.c_str());
            return 1;
        }
        const std::string path = dir + "/ic_launcher.png";
        if (!drawIcon(font, d.size, path)) return 1;
        std::printf("%s  %dx%d\n", path.c_str(), d.size, d.size);
    }
    if (!docIcon.empty()) {
        if (!drawIcon(font, 512, docIcon)) return 1;
        std::printf("%s  512x512\n", docIcon.c_str());
    }
    return 0;
}
