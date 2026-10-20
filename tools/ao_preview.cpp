// Offline multi-bounce AO preview (docs/AO.md): what the approximation in shaders/effects.frag does to a coloured
// corner, next to the two things it is trying to be.
//
// The scene is three planes — floor, and two walls at right angles — under a uniform sky of radiance 1. Four panels:
//
//   1. NO AO            albedo · sky, the flat ambient a forward renderer would add with occlusion switched off
//   2. SINGLE-BOUNCE AO albedo · sky · ao, what the engine did before GH-115426
//   3. MULTI-BOUNCE     albedo · sky · multiBounceVisibility(ao, albedo), the curve in src/engine/ao.h
//   4. REFERENCE        a path tracer, two bounces: the corner really is lit by light bouncing off its own walls
//
// The AO value in panel 2 and 3 is the *exact* visibility of the hemisphere from a ray cast, not the twelve-tap SSAO
// estimate, so the comparison is about the curve and not about sampling noise. Panel 4 is the ground truth the curve
// approximates; the RMSE lines printed at the end are measured against it.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party -I third_party/glm tools/ao_preview.cpp -o build/ao_preview
// Run:
//     ./build/ao_preview docs/ao-preview.png 1600 900
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

#include "engine/ao.h"

using namespace ukiyo;

namespace {

// ---- scene ---------------------------------------------------------------------------------

// Three axis-aligned planes: y = 0 (floor), x = 0 (left wall), z = 0 (back wall). A ray that hits none of them escapes
// to the sky.
struct Surface {
    glm::vec3 albedo{1.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

struct Hit {
    bool hit = false;
    float t = 1e30f;
    Surface surface;
};

Hit intersect(const glm::vec3& o, const glm::vec3& d) {
    Hit h;
    // Floor.
    if (d.y < -1e-6f) {
        const float t = -o.y / d.y;
        if (t > 1e-3f && t < h.t) {
            h.hit = true;
            h.t = t;
            h.surface = {glm::vec3(0.55f, 0.55f, 0.60f), glm::vec3(0.0f, 1.0f, 0.0f)};
        }
    }
    // Left wall, saturated red.
    if (d.x < -1e-6f) {
        const float t = -o.x / d.x;
        if (t > 1e-3f && t < h.t) {
            h.hit = true;
            h.t = t;
            h.surface = {glm::vec3(0.85f, 0.20f, 0.16f), glm::vec3(1.0f, 0.0f, 0.0f)};
        }
    }
    // Back wall, saturated blue.
    if (d.z < -1e-6f) {
        const float t = -o.z / d.z;
        if (t > 1e-3f && t < h.t) {
            h.hit = true;
            h.t = t;
            h.surface = {glm::vec3(0.16f, 0.28f, 0.82f), glm::vec3(0.0f, 0.0f, 1.0f)};
        }
    }
    return h;
}

// ---- sampling ------------------------------------------------------------------------------

struct Rng {
    uint32_t s = 1;
    explicit Rng(uint32_t seed) : s(seed * 747796405u + 2891336453u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return float(next() & 0x00FFFFFFu) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

// A cosine-weighted direction around `n` (the distribution a diffuse surface wants).
glm::vec3 cosineDir(const glm::vec3& n, Rng& rng) {
    const float u1 = rng.unit(), u2 = rng.unit();
    const float r = std::sqrt(u1);
    const float phi = 6.2831853f * u2;
    glm::vec3 t = std::fabs(n.y) < 0.99f ? glm::normalize(glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f)))
                                         : glm::normalize(glm::cross(n, glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::vec3 b = glm::cross(n, t);
    return glm::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - u1)));
}

// The exact fraction of the hemisphere above `n` that escapes to the sky. The AO term, without the SSAO estimate.
float visibility(const glm::vec3& p, const glm::vec3& n, Rng& rng, int samples) {
    int escaped = 0;
    for (int i = 0; i < samples; ++i) {
        const glm::vec3 d = cosineDir(n, rng);
        if (!intersect(p + n * 1e-3f, d).hit) ++escaped;
    }
    return float(escaped) / float(samples);
}

// Sky radiance: uniform and white, which is the ambient model the engine's hemisphere colours stand in for.
constexpr float kSky = 1.0f;

// Reference: cosine-sampled path tracing with `bounces` diffuse bounces, sky on a miss.
glm::vec3 trace(const glm::vec3& p, const glm::vec3& n, Rng& rng, int bounces) {
    glm::vec3 radiance(0.0f);
    glm::vec3 through(1.0f);
    glm::vec3 origin = p + n * 1e-3f;
    glm::vec3 normal = n;
    for (int b = 0; b < bounces; ++b) {
        const glm::vec3 d = cosineDir(normal, rng);
        const Hit h = intersect(origin, d);
        if (!h.hit) {
            // One sample of the sky through a cosine-weighted direction already carries the 1/pi the estimator needs.
            radiance += through * kSky;
            break;
        }
        through *= h.surface.albedo;
        origin = origin + d * h.t + h.surface.normal * 1e-3f;
        normal = h.surface.normal;
    }
    return radiance;
}

// ---- image and text ------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<glm::vec3> color;
    void reset(int width, int height) {
        w = width;
        h = height;
        color.assign(size_t(w) * h, glm::vec3(0.0f));
    }
};

const uint8_t kGlyphRows[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},      {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},   {6, 8, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 12, 12},      {1, 1, 2, 4, 8, 16, 16},      {0, 12, 12, 0, 12, 12, 0},
    {0, 0, 0, 31, 0, 0, 0},       {0, 4, 4, 31, 4, 4, 0},
};

int glyphIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
        case '.': return 36;
        case '/': return 37;
        case ':': return 38;
        case '-': return 39;
        case '+': return 40;
        default: return -1;
    }
}

void fillRect(Image& img, int x0, int y0, int rw, int rh, glm::vec3 c, float alpha) {
    for (int y = std::max(0, y0); y < std::min(img.h, y0 + rh); ++y)
        for (int x = std::max(0, x0); x < std::min(img.w, x0 + rw); ++x)
            img.color[size_t(y) * img.w + x] = glm::mix(img.color[size_t(y) * img.w + x], c, alpha);
}

void drawText(Image& img, int x, int y, const std::string& text, int scale, glm::vec3 c) {
    for (char ch : text) {
        if (ch == ' ') { x += 4 * scale; continue; }
        const int g = glyphIndex(ch);
        if (g < 0) { x += 6 * scale; continue; }
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (kGlyphRows[g][row] & (1u << (4 - col))) fillRect(img, x + col * scale, y + row * scale, scale, scale, c, 1.0f);
        x += 6 * scale;
    }
}

glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

// One panel: the same camera into the corner, shaded four ways.
struct PanelStats {
    double rmseSingle = 0.0;
    double rmseMulti = 0.0;
    double meanAo = 0.0;
    double maxLift = 0.0;
};

PanelStats renderPanels(int panelW, int panelH, int spp, Image& sheet, int ox, int oy, bool twoByTwo) {
    // Camera inside the corner, looking at the seam where the three planes meet.
    const glm::vec3 eye(1.8f, 1.6f, 1.9f);
    const glm::vec3 target(0.25f, 0.30f, 0.25f);
    const glm::vec3 fwd = glm::normalize(target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, fwd);
    const float focus = 0.5f * float(panelH) / std::tan(glm::radians(52.0f) * 0.5f);

    PanelStats stats;
    double sumS = 0.0, sumM = 0.0, sumAo = 0.0;
    int counted = 0;

    std::vector<glm::vec3> panels[4];
    for (auto& p : panels) p.assign(size_t(panelW) * panelH, glm::vec3(0.0f));

    // Supersampled: two samples a pixel each way, box-filtered at the end. It smooths the tracer's residual noise and
    // gives the panel edges clean lines, which is also what keeps the PNG a sensible size.
    const int ss = 2;
    for (int yy = 0; yy < panelH * ss; ++yy) {
        for (int xx = 0; xx < panelW * ss; ++xx) {
            const int x = xx / ss, y = yy / ss;
            Rng rng(uint32_t(xx * 1973 + yy * 9277 + 12345));
            const glm::vec2 ndc((float(xx) + 0.5f) / float(panelW * ss) * 2.0f - 1.0f,
                                1.0f - (float(yy) + 0.5f) / float(panelH * ss) * 2.0f);
            const glm::vec3 dir = glm::normalize(fwd * focus + right * (ndc.x * float(panelW) * 0.5f) +
                                                 up * (ndc.y * float(panelH) * 0.5f));
            const Hit h = intersect(eye, dir);
            const size_t at = size_t(y) * panelW + x;
            if (!h.hit) {
                for (auto& p : panels) p[at] += glm::vec3(kSky) / float(ss * ss);
                continue;
            }
            const glm::vec3 p = eye + dir * h.t;
            const glm::vec3 albedo = h.surface.albedo;

            // AO from an exact visibility estimate. The reference panels are rendered separately below, from the same
            // camera, with far more samples; here they copy the numbers so the four panels line up pixel for pixel.
            const float ao = visibility(p, h.surface.normal, rng, spp);
            // The tracer needs four times the samples to reach the same noise floor as a visibility test: it is
            // estimating a colour, not a yes/no.
            const int traceSamples = spp * 4;
            glm::vec3 reference(0.0f);
            for (int i = 0; i < traceSamples; ++i) reference += trace(p, h.surface.normal, rng, 2);
            reference /= float(traceSamples);

            const glm::vec3 none = albedo * kSky;
            const glm::vec3 single = none * singleBounceVisibility(ao);
            const glm::vec3 multi = none * multiBounceVisibility(ao, albedo);

            panels[0][at] += none / float(ss * ss);
            panels[1][at] += single / float(ss * ss);
            panels[2][at] += multi / float(ss * ss);
            panels[3][at] += reference / float(ss * ss);

            const auto lum = [](const glm::vec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; };
            const double dS = lum(single) - lum(reference);
            const double dM = lum(multi) - lum(reference);
            sumS += dS * dS;
            sumM += dM * dM;
            sumAo += ao;
            stats.maxLift = std::max(stats.maxLift, double(glm::length(multi - single)));
            ++counted;
        }
    }
    stats.rmseSingle = counted ? std::sqrt(sumS / counted) : 0.0;
    stats.rmseMulti = counted ? std::sqrt(sumM / counted) : 0.0;
    stats.meanAo = counted ? sumAo / counted : 0.0;

    const char* names[4] = {"NO AO", "SINGLE-BOUNCE AO", "MULTI-BOUNCE (ENGINE)", "PATH TRACED (2 BOUNCES)"};
    const char* notes[4] = {"ALBEDO X SKY", "THE OLD TERM", "GH-115426 FIT", "GROUND TRUTH"};
    const int cols = twoByTwo ? 2 : 4;
    for (int i = 0; i < 4; ++i) {
        const int px = ox + (i % cols) * panelW;
        const int py = oy + (i / cols) * panelH;
        for (int y = 0; y < panelH; ++y)
            for (int x = 0; x < panelW; ++x) {
                glm::vec3 c = toSrgb(panels[i][size_t(y) * panelW + x]);
                const size_t o = size_t(py + y) * sheet.w + (px + x);
                sheet.color[o] = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
            }
        // A dark plate behind each caption: the panels are bright, and white text on the red wall is unreadable.
        fillRect(sheet, px + 12, py + 12, 236, 50, glm::vec3(0.02f, 0.02f, 0.03f), 0.72f);
        drawText(sheet, px + 26, py + 22, names[i], 2, glm::vec3(0.95f, 0.96f, 0.99f));
        drawText(sheet, px + 26, py + 40, notes[i], 2, glm::vec3(0.62f, 0.68f, 0.76f));
    }
    return stats;
}

} // namespace

int main(int argc, char** argv) {
    const std::string outPath = argc > 1 ? argv[1] : "docs/ao-preview.png";
    const int outW = argc > 2 ? std::atoi(argv[2]) : 1600;
    const int outH = argc > 3 ? std::atoi(argv[3]) : 900;
    const int spp = argc > 4 ? std::atoi(argv[4]) : 192;   // visibility samples per pixel; the tracer takes 4x this

    Image sheet;
    sheet.reset(outW, outH);
    for (auto& c : sheet.color) c = glm::vec3(0.035f, 0.040f, 0.050f);

    const int panelW = outW / 2, panelH = (outH - 118) / 2;
    const PanelStats stats = renderPanels(panelW, panelH, spp, sheet, 0, 78, true);

    // Rules between the panels.
    for (int x = 0; x < outW; ++x) fillRect(sheet, x, 78 + panelH - 1, outW, 2, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);
    for (int y = 0; y < outH; ++y) fillRect(sheet, panelW - 1, y, 2, outH, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);

    // Header and the measured numbers.
    drawText(sheet, 28, 20, "MULTI-BOUNCE AMBIENT OCCLUSION", 3, glm::vec3(0.95f, 0.96f, 0.99f));
    drawText(sheet, 28, 46, "GH-115426 - ACTIVISION 2016 FIT - DOCS/AO.MD", 2, glm::vec3(0.55f, 0.62f, 0.72f));
    char line[256];
    std::snprintf(line, sizeof(line), "LUMINANCE RMSE VS TRACER - SINGLE %.4f - MULTI %.4f", stats.rmseSingle,
                  stats.rmseMulti);
    drawText(sheet, 28, outH - 52, line, 2, glm::vec3(0.72f, 0.80f, 0.90f));
    std::snprintf(line, sizeof(line), "MEAN VISIBILITY %.3f   LARGEST CORRECTION %.3f   %d VISIBILITY RAYS A PIXEL",
                  stats.meanAo, stats.maxLift, spp);
    drawText(sheet, 28, outH - 30, line, 2, glm::vec3(0.45f, 0.52f, 0.62f));

    std::vector<uint8_t> bytes(size_t(outW) * outH * 3);
    for (size_t i = 0; i < sheet.color.size(); ++i) {
        const glm::vec3 c = glm::clamp(sheet.color[i], glm::vec3(0.0f), glm::vec3(1.0f));
        bytes[i * 3 + 0] = uint8_t(c.x * 255.0f + 0.5f);
        bytes[i * 3 + 1] = uint8_t(c.y * 255.0f + 0.5f);
        bytes[i * 3 + 2] = uint8_t(c.z * 255.0f + 0.5f);
    }
    if (!stbi_write_png(outPath.c_str(), outW, outH, 3, bytes.data(), outW * 3)) {
        std::fprintf(stderr, "could not write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)  RMSE single %.4f  multi %.4f  mean visibility %.3f\n", outPath.c_str(), outW, outH,
                stats.rmseSingle, stats.rmseMulti, stats.meanAo);
    return 0;
}
