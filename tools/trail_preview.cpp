// Offline trail preview (docs/TRAIL.md): the engine's own Trail builds the ribbons, and a small software rasteriser
// draws them, so the figure can be made on a machine with no GL or Vulkan.
//
// Three panels, each one a real run of src/engine/trail.cpp:
//   1. a sword arc — billboard alignment, additive blending, a gradient that ends in a hot white head;
//   2. a dust trail on the ground — local alignment, so its width does not change as the camera moves;
//   3. the three UV modes side by side, with the texture coordinate drawn as stripes so the difference is visible.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party tools/trail_preview.cpp src/engine/trail.cpp -o build/trail_preview
// Run:
//     ./build/trail_preview docs/trail-preview.png
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
#include <glm/gtc/matrix_transform.hpp>

#include "engine/trail.h"

using namespace ukiyo;

namespace {

// ---- framebuffer ---------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<glm::vec3> color;

    void reset(int width, int height, glm::vec3 background) {
        w = width;
        h = height;
        color.assign(size_t(w) * h, background);
    }
};

struct Camera {
    glm::vec3 eye{0.0f}, fwd{0.0f, 0.0f, 1.0f}, right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f};
    float focus = 1.0f;

    glm::vec3 toView(const glm::vec3& p) const {
        const glm::vec3 rel = p - eye;
        return glm::vec3(glm::dot(rel, right), glm::dot(rel, up), glm::dot(rel, fwd));
    }
};

Camera makeCamera(const glm::vec3& eye, const glm::vec3& target, float fovDegrees, int height) {
    Camera c;
    c.eye = eye;
    c.fwd = glm::normalize(target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(c.fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    c.right = right;
    c.up = glm::cross(right, c.fwd);
    c.focus = 0.5f * float(height) / std::tan(glm::radians(fovDegrees) * 0.5f);
    return c;
}

glm::vec2 project(const Camera& cam, const glm::vec3& world, const Image& img) {
    const glm::vec3 v = cam.toView(world);
    if (v.z <= 0.05f) return glm::vec2(-1e6f);
    return glm::vec2(v.x / v.z * cam.focus + img.w * 0.5f, img.h * 0.5f - v.y / v.z * cam.focus);
}

// One triangle of a ribbon, kept whole so the panels can sort them back to front before blending.
struct Tri {
    glm::vec3 p[3];
    glm::vec4 c[3];
    glm::vec2 uv[3];
    float depth = 0.0f;
};

// Rasterises one triangle with per-vertex colour and alpha, over `img`. Additive panels add instead of blending.
// `softEdge` fades the outer band of the strip (in v) to zero, evaluated per fragment from the interpolated UV: doing
// it per vertex would zero both edges and leave nothing in between, because a ribbon's vertices only sit on the edges.
// `stripes` draws the u coordinate as bands instead of the vertex colour, so a tiling mode is visible without a texture.
void rasterTri(Image& img, const Camera& cam, const Tri& t, bool additive, float softEdge = 0.0f, bool stripes = false) {
    glm::vec3 v[3];
    for (int i = 0; i < 3; ++i) {
        v[i] = cam.toView(t.p[i]);
        if (v[i].z <= 0.05f) return;
    }
    auto screen = [&](const glm::vec3& p) {
        return glm::vec2(p.x / p.z * cam.focus + img.w * 0.5f, img.h * 0.5f - p.y / p.z * cam.focus);
    };
    const glm::vec2 p0 = screen(v[0]), p1 = screen(v[1]), p2 = screen(v[2]);
    const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::fabs(area) < 1e-9f) return;
    const float inv = 1.0f / area;
    const int x0 = std::max(0, int(std::floor(std::min({p0.x, p1.x, p2.x}))));
    const int x1 = std::min(img.w - 1, int(std::ceil(std::max({p0.x, p1.x, p2.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({p0.y, p1.y, p2.y}))));
    const int y1 = std::min(img.h - 1, int(std::ceil(std::max({p0.y, p1.y, p2.y}))));
    for (int y = y0; y <= y1; ++y) {
        const float py = float(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = float(x) + 0.5f;
            const float l0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) * inv;
            const float l1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) * inv;
            const float l2 = 1.0f - l0 - l1;
            if (l0 < -2e-3f || l1 < -2e-3f || l2 < -2e-3f) continue;
            const float iz = l0 / v[0].z + l1 / v[1].z + l2 / v[2].z;
            if (iz <= 0.0f) continue;
            const float w0 = l0 / v[0].z / iz, w1 = l1 / v[1].z / iz, w2 = l2 / v[2].z / iz;
            const glm::vec4 c = t.c[0] * w0 + t.c[1] * w1 + t.c[2] * w2;
            const glm::vec2 uv = t.uv[0] * w0 + t.uv[1] * w1 + t.uv[2] * w2;
            float alpha = std::max(0.0f, std::min(1.0f, c.a));
            glm::vec3 rgb = glm::vec3(c);
            if (softEdge > 0.0f) {
                // v runs 0..1 across the strip; the outer softEdge half at each edge fades out, as shaders/trail.frag
                // does with smoothstep.
                const float e = 1.0f - std::fabs(uv.y * 2.0f - 1.0f);
                alpha *= std::max(0.0f, std::min(1.0f, e / softEdge));
            }
            if (stripes) {
                const float f = uv.x - std::floor(uv.x);
                const float band = std::pow(std::fabs(std::sin(f * 3.14159265f)), 5.0f);
                rgb = glm::mix(glm::vec3(0.16f, 0.24f, 0.30f), glm::vec3(0.86f, 0.96f, 1.0f), band);
            }
            if (alpha <= 0.002f) continue;
            glm::vec3& dst = img.color[size_t(y) * img.w + x];
            if (additive) dst += rgb * alpha;
            else dst = glm::mix(dst, rgb, alpha);
        }
    }
}

// Splits the ribbon into triangles and draws them back to front.
void drawRibbon(Image& img, const Camera& cam, const Trail& trail, bool additive, float softEdge) {
    std::vector<TrailVertex> verts;
    std::vector<uint32_t> idx;
    trail.buildRibbon(cam.eye, &verts, &idx);
    std::vector<Tri> tris;
    tris.reserve(idx.size() / 3);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        Tri t;
        float z = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const TrailVertex& tv = verts[idx[i + k]];
            t.p[k] = tv.position;
            t.uv[k] = tv.uv;
            t.c[k] = tv.color;
            z += cam.toView(tv.position).z;
        }
        t.depth = z / 3.0f;
        tris.push_back(t);
    }
    std::sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
    for (const Tri& t : tris) rasterTri(img, cam, t, additive, softEdge);
}

// ---- text ----------------------------------------------------------------------------------

// A 5x7 bitmap font, just the glyphs the captions need.
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
    {24, 25, 2, 4, 8, 19, 3},     {0, 0, 0, 31, 0, 0, 0},
};

int glyphIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
        case '.': return 36;
        case '/': return 37;
        case ':': return 38;
        case '-': return 39;
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

// ---- scene helpers -------------------------------------------------------------------------

void verticalGradient(Image& img, glm::vec3 top, glm::vec3 bottom) {
    for (int y = 0; y < img.h; ++y) {
        const float t = float(y) / float(std::max(1, img.h - 1));
        fillRect(img, 0, y, img.w, 1, glm::mix(top, bottom, t), 1.0f);
    }
}

void drawLine(Image& img, const Camera& cam, glm::vec3 a, glm::vec3 b, glm::vec3 color, float alpha) {
    if (cam.toView(a).z <= 0.05f || cam.toView(b).z <= 0.05f) return;
    const glm::vec2 p = project(cam, a, img);
    const glm::vec2 q = project(cam, b, img);
    const int steps = int(std::max(std::abs(q.x - p.x), std::abs(q.y - p.y)));
    if (steps <= 0) return;
    if (steps > 4000) return;   // a line going to the horizon: not worth drawing
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const glm::vec2 at = glm::mix(p, q, t);
        fillRect(img, int(at.x), int(at.y), 1, 1, color, alpha);
    }
}

// A ground plane grid, in metres, drawn as lines. Used by both 3D panels for a sense of scale.
void groundGrid(Image& img, const Camera& cam, float extent, float step, glm::vec3 color, float alpha) {
    for (float v = -extent; v <= extent + 1e-4f; v += step) {
        drawLine(img, cam, glm::vec3(-extent, 0.0f, v), glm::vec3(extent, 0.0f, v), color, alpha);
        drawLine(img, cam, glm::vec3(v, 0.0f, -extent), glm::vec3(v, 0.0f, extent), color, alpha);
    }
}

// Blits one panel into the sheet.
void blit(Image& dst, const Image& src, int x0, int y0) {
    for (int y = 0; y < src.h; ++y) {
        if (y0 + y < 0 || y0 + y >= dst.h) continue;
        for (int x = 0; x < src.w; ++x) {
            if (x0 + x < 0 || x0 + x >= dst.w) continue;
            dst.color[size_t(y0 + y) * dst.w + (x0 + x)] = src.color[size_t(y) * src.w + x];
        }
    }
}

// ---- the panels' scenes --------------------------------------------------------------------

// A sword arc: an emitter on a circle sweeping through 200 degrees, billboarded, additive.
Trail swordArc() {
    TrailSettings s;
    s.limit = TrailLimit::Lifetime;
    s.lifetime = 0.55f;
    s.minSectionLength = 0.05f;
    s.width = 0.34f;
    s.align = TrailAlign::Billboard;
    s.tiling = TrailTiling::Tile;
    s.tilingMultiplier = 1.6f;
    s.widthCurve.setPoints({{0.0f, 0.10f}, {0.55f, 1.0f}, {1.0f, 0.45f}});
    s.gradient.setStops({
        {0.0f, glm::vec4(0.25f, 0.05f, 0.60f, 0.0f)},
        {0.35f, glm::vec4(0.55f, 0.18f, 0.95f, 0.30f)},
        {0.75f, glm::vec4(1.00f, 0.55f, 0.25f, 0.80f)},
        {1.00f, glm::vec4(1.00f, 0.95f, 0.80f, 1.0f)},
    });
    Trail t(s);
    // A near-vertical arc in front of the camera: 190 degrees of a 1.45 m circle, with a little depth so the
    // billboard twist is visible rather than implied.
    const int steps = 34;
    for (int i = 0; i <= steps; ++i) {
        const float u = float(i) / float(steps);
        const float a = glm::radians(-100.0f + 200.0f * u);
        t.advance(glm::vec3(std::cos(a) * 1.15f, 1.15f + std::sin(a) * 1.15f, 0.18f * std::sin(u * 3.14159f)));
        t.update(1.0f / 60.0f);
    }
    return t;
}

// A dust trail: a wandering path on the ground, kept to a fixed length, aligned to the ground plane.
Trail dustTrail() {
    TrailSettings s;
    s.limit = TrailLimit::MaxLength;
    s.maxLength = 5.5f;
    s.minSectionLength = 0.08f;
    s.width = 0.60f;
    s.align = TrailAlign::Local;
    s.localUp = glm::vec3(0.0f, 1.0f, 0.0f);
    s.widthCurve.setPoints({{0.0f, 1.0f}, {0.75f, 0.85f}, {1.0f, 0.15f}});
    s.gradient.setStops({
        {0.0f, glm::vec4(0.42f, 0.34f, 0.27f, 0.0f)},
        {0.4f, glm::vec4(0.58f, 0.48f, 0.37f, 0.30f)},
        {1.0f, glm::vec4(0.74f, 0.66f, 0.52f, 0.55f)},
    });
    Trail t(s);
    const int steps = 64;
    for (int i = 0; i <= steps; ++i) {
        const float u = float(i) / float(steps);
        const float x = -2.4f + 4.8f * u;
        const float z = 0.60f * std::sin(u * 5.2f) + 0.20f * std::sin(u * 11.0f);
        t.advance(glm::vec3(x, 0.02f, z));
        t.update(1.0f / 60.0f);
    }
    return t;
}

// A straight ribbon for the tiling panel: same geometry, one of the three UV modes.
Trail tilingRibbon(TrailTiling mode, bool pinUv, float row) {
    TrailSettings s;
    s.limit = TrailLimit::MaxLength;
    s.maxLength = 8.0f;
    s.minSectionLength = 0.10f;
    s.width = 0.30f;
    s.align = TrailAlign::Local;
    s.tiling = mode;
    s.tilingMultiplier = 1.8f;   // one texture repeat every 0.55 m of path
    s.pinUv = pinUv;
    s.widthCurve.setPoints({{0.0f, 0.8f}, {1.0f, 1.0f}});
    s.gradient.setStops({{0.0f, glm::vec4(0.30f, 0.62f, 0.72f, 0.35f)}, {1.0f, glm::vec4(0.62f, 0.90f, 0.95f, 1.0f)}});
    Trail t(s);
    // 0.18 m a step, so every advance clears minSectionLength and lays a section down.
    const int steps = 24;
    for (int i = 0; i <= steps; ++i) {
        const float u = float(i) / float(steps);
        t.advance(glm::vec3(-2.2f + 4.4f * u, 0.0f, row));
        t.update(1.0f / 60.0f);
    }
    return t;
}

// Draws a tiling ribbon with the texture coordinate as stripes, so the mode is visible without a texture file.
void drawTilingRibbon(Image& img, const Camera& cam, const Trail& trail) {
    std::vector<TrailVertex> verts;
    std::vector<uint32_t> idx;
    trail.buildRibbon(cam.eye, &verts, &idx);
    std::vector<Tri> tris;
    tris.reserve(idx.size() / 3);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        Tri t;
        float z = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const TrailVertex& tv = verts[idx[i + k]];
            t.p[k] = tv.position;
            t.uv[k] = tv.uv;
            t.c[k] = tv.color;
            z += cam.toView(tv.position).z;
        }
        t.depth = z / 3.0f;
        tris.push_back(t);
    }
    std::sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
    for (const Tri& t : tris) rasterTri(img, cam, t, true, 0.0f, true);
}

glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

} // namespace

int main(int argc, char** argv) {
    const std::string outPath = argc > 1 ? argv[1] : "docs/trail-preview.png";
    const int outW = argc > 2 ? std::atoi(argv[2]) : 1600;
    const int outH = argc > 3 ? std::atoi(argv[3]) : 900;
    const int ss = 2;   // supersampling: everything is drawn at 2x and box-filtered once at the end

    const int pw = outW * ss / 2, ph = outH * ss / 2;   // panel size in device pixels
    const glm::vec3 kBg(0.040f, 0.048f, 0.062f);

    Image sheet;
    sheet.reset(outW * ss, outH * ss, kBg);

    // --- top left: the sword arc, three-quarter view ---------------------------------------------------------
    {
        Image panel;
        panel.reset(pw, ph, kBg);
        verticalGradient(panel, glm::vec3(0.070f, 0.085f, 0.125f), glm::vec3(0.030f, 0.034f, 0.044f));
        const Camera cam = makeCamera(glm::vec3(0.0f, 1.55f, 5.15f), glm::vec3(0.0f, 1.30f, 0.0f), 44.0f, ph);
        groundGrid(panel, cam, 4.0f, 0.5f, glm::vec3(0.20f, 0.24f, 0.32f), 0.30f);
        const Trail arc = swordArc();
        drawRibbon(panel, cam, arc, true, 0.0f);
        blit(sheet, panel, 0, 0);
        drawText(sheet, 40 * ss, 30 * ss, "THE SWEEP", 3 * ss, glm::vec3(0.93f, 0.95f, 0.98f));
        drawText(sheet, 40 * ss, 54 * ss, "BILLBOARD / ADDITIVE / LIFETIME 0.30 S", 2 * ss, glm::vec3(0.55f, 0.62f, 0.72f));
        char stats[160];
        std::snprintf(stats, sizeof(stats), "%d SECTIONS  %.2f M", arc.sectionCount(), double(arc.length()));
        drawText(sheet, 40 * ss, int(0.5f * outH * ss) - 40 * ss, stats, 2 * ss, glm::vec3(0.45f, 0.52f, 0.62f));
    }

    // --- top right: the three UV modes, stripes instead of a texture ------------------------------------------
    {
        Image panel;
        panel.reset(pw, ph, glm::vec3(0.030f, 0.036f, 0.048f));
        const Trail stretch = tilingRibbon(TrailTiling::Stretch, false, 0.0f);
        const Trail tile = tilingRibbon(TrailTiling::Tile, false, 0.0f);
        const Trail pinned = tilingRibbon(TrailTiling::Tile, true, 0.0f);
        const Trail* ribbons[3] = {&stretch, &tile, &pinned};
        const char* names[3] = {"STRETCH", "TILE", "PIN UV"};
        const char* notes[3] = {"U RUNS 0..1 OVER THE WHOLE TRAIL", "U = ARC X 1.8 (ONE REPEAT / 0.55 M)", "THE PATTERN STAYS IN SPACE"};
        const int rowH = ph / 3;
        Image row;
        for (int i = 0; i < 3; ++i) {
            row.reset(pw, rowH, glm::vec3(0.030f, 0.036f, 0.048f));
            // From above, looking down: the ribbon runs left to right across the row, with the whole 4.4 m in frame.
            const Camera cam = makeCamera(glm::vec3(0.0f, 2.75f, 0.02f), glm::vec3(0.0f, 0.0f, 0.0f), 46.0f, rowH);
            drawTilingRibbon(row, cam, *ribbons[i]);
            blit(panel, row, 0, i * rowH);
            drawText(panel, 34 * ss, i * rowH + 22 * ss, names[i], 2 * ss, glm::vec3(0.90f, 0.94f, 0.99f));
            drawText(panel, 34 * ss, i * rowH + 40 * ss, notes[i], 2 * ss, glm::vec3(0.55f, 0.63f, 0.74f));
            // Row separators, so the three read as three.
            if (i > 0) fillRect(panel, 24 * ss, i * rowH, pw - 48 * ss, 1, glm::vec3(0.11f, 0.13f, 0.17f), 0.8f);
        }
        blit(sheet, panel, outW * ss / 2, 0);
        drawText(sheet, outW * ss / 2 + 40 * ss, int(0.5f * outH * ss) - 46 * ss, "UV MODES  /  THE SAME GEOMETRY, THREE MAPPINGS",
                 2 * ss, glm::vec3(0.50f, 0.57f, 0.67f));
    }

    // --- bottom left: the dust trail, local alignment ---------------------------------------------------------
    {
        Image panel;
        panel.reset(pw, ph, kBg);
        verticalGradient(panel, glm::vec3(0.055f, 0.060f, 0.070f), glm::vec3(0.028f, 0.028f, 0.032f));
        const Camera cam = makeCamera(glm::vec3(0.0f, 5.20f, 4.60f), glm::vec3(0.0f, 0.0f, 0.0f), 46.0f, ph);
        groundGrid(panel, cam, 3.6f, 0.4f, glm::vec3(0.18f, 0.19f, 0.22f), 0.35f);
        const Trail dust = dustTrail();
        drawRibbon(panel, cam, dust, false, 0.45f);
        blit(sheet, panel, 0, outH * ss / 2);
        drawText(sheet, 40 * ss, outH * ss / 2 + 34 * ss, "GROUND DUST", 3 * ss, glm::vec3(0.93f, 0.95f, 0.98f));
        drawText(sheet, 40 * ss, outH * ss / 2 + 58 * ss, "LOCAL UP 0 1 0 / ALPHA / MAX LENGTH 5.50 M", 2 * ss,
                 glm::vec3(0.55f, 0.62f, 0.72f));
        char stats[160];
        std::snprintf(stats, sizeof(stats), "%d SECTIONS  %.2f M KEPT", dust.sectionCount(), double(dust.length()));
        drawText(sheet, 40 * ss, outH * ss / 2 + 82 * ss, stats, 2 * ss, glm::vec3(0.45f, 0.52f, 0.62f));
    }

    // --- bottom right: what the CPU ticks over, as numbers and as the two curves ------------------------------
    {
        Image panel;
        panel.reset(pw, ph, glm::vec3(0.034f, 0.040f, 0.052f));
        const int x0 = 46 * ss, y0 = 30 * ss;
        drawText(panel, x0, y0, "WHAT THE CPU OWNS", 3 * ss, glm::vec3(0.93f, 0.95f, 0.98f));
        drawText(panel, x0, y0 + 26 * ss, "NO DEVICE, NO CLOCK, NO SCENE TREE", 2 * ss, glm::vec3(0.55f, 0.62f, 0.72f));

        const Trail arc = swordArc();
        const Trail dust = dustTrail();
        const Trail tile = tilingRibbon(TrailTiling::Tile, false, 0.0f);
        const char* lines[] = {
            "TRAIL HOLDS SECTIONS: POSITION, AGE, ARC",
            "EMIT ADDS ONE WHEN THE HEAD MOVES 0.12 M",
            "UPDATE AGES AND TRIMS BY LIFETIME OR LENGTH",
            "BUILD RIBBON EMITS 2 VERTICES PER SECTION",
            "AND 6 INDICES PER SECTION PAIR",
        };
        for (int i = 0; i < 5; ++i)
            drawText(panel, x0, y0 + (64 + i * 20) * ss, lines[i], 2 * ss, glm::vec3(0.58f, 0.65f, 0.75f));

        char buf[160];
        std::snprintf(buf, sizeof(buf), "SWEEP   %2d SECTIONS  %5.2f M  %4d TRIANGLES", arc.sectionCount(),
                      double(arc.length()), (arc.sectionCount() - 1) * 2);
        drawText(panel, x0, y0 + 186 * ss, buf, 2 * ss, glm::vec3(0.72f, 0.80f, 0.90f));
        std::snprintf(buf, sizeof(buf), "DUST    %2d SECTIONS  %5.2f M  %4d TRIANGLES", dust.sectionCount(),
                      double(dust.length()), (dust.sectionCount() - 1) * 2);
        drawText(panel, x0, y0 + 208 * ss, buf, 2 * ss, glm::vec3(0.72f, 0.80f, 0.90f));
        std::snprintf(buf, sizeof(buf), "RIBBON  %2d SECTIONS  %5.2f M  %4d TRIANGLES", tile.sectionCount(),
                      double(tile.length()), (tile.sectionCount() - 1) * 2);
        drawText(panel, x0, y0 + 230 * ss, buf, 2 * ss, glm::vec3(0.72f, 0.80f, 0.90f));

        // The width curve and the gradient of the sweep, plotted from the same objects the ribbon used.
        const int gx = x0, gy = y0 + 268 * ss, gw = 240 * ss, gh = 74 * ss;
        drawText(panel, gx, gy - 18 * ss, "WIDTH ALONG THE TRAIL", 2 * ss, glm::vec3(0.50f, 0.57f, 0.66f));
        for (int x = 0; x <= gw; ++x) {
            const float t = float(x) / float(gw);
            const float w = arc.settings().widthCurve.sample(t);
            const int y = gy + gh - int(w * float(gh) * 0.85f);
            fillRect(panel, gx + x, y, 1, 2, glm::vec3(0.55f, 0.80f, 0.95f), 1.0f);
        }
        const int gx2 = x0 + gw + 40 * ss;
        drawText(panel, gx2, gy - 18 * ss, "COLOUR AND ALPHA", 2 * ss, glm::vec3(0.50f, 0.57f, 0.66f));
        for (int x = 0; x <= gw; ++x) {
            const float t = float(x) / float(gw);
            const glm::vec4 c = arc.settings().gradient.sample(t);
            const float a = std::max(0.0f, std::min(1.0f, c.a));
            const glm::vec3 rgb = a > 1e-4f ? glm::vec3(c) / a : glm::vec3(c);
            for (int y = 0; y < gh / 3; ++y) {
                fillRect(panel, gx2 + x, gy + y, 1, 1, glm::clamp(rgb, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
                fillRect(panel, gx2 + x, gy + y + gh / 3 + 4, 1, 1, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);
                fillRect(panel, gx2 + x, gy + y + gh / 3 + 4, 1, 1, glm::vec3(c), a);
            }
        }
        blit(sheet, panel, outW * ss / 2, outH * ss / 2);
    }

    // Panel rules and the sheet's footer.
    for (int x = 0; x < sheet.w; ++x) {
        for (int y : {outH * ss / 2 - 1, outH * ss / 2}) fillRect(sheet, x, y, 1, 1, glm::vec3(0.13f, 0.15f, 0.19f), 1.0f);
    }
    for (int y = 0; y < sheet.h; ++y) {
        for (int x : {outW * ss / 2 - 1, outW * ss / 2}) fillRect(sheet, x, y, 1, 1, glm::vec3(0.13f, 0.15f, 0.19f), 1.0f);
    }
    drawText(sheet, 40 * ss, sheet.h - 30 * ss, "UKIYO ENGINE  TRAIL3D  ITEM 27  DOCS/TRAIL.MD", 2 * ss,
             glm::vec3(0.36f, 0.42f, 0.50f));

    // Downsample.
    std::vector<uint8_t> bytes(size_t(outW) * outH * 3);
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            glm::vec3 acc(0.0f);
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) acc += sheet.color[size_t(y * ss + sy) * sheet.w + (x * ss + sx)];
            const glm::vec3 c = toSrgb(acc / float(ss * ss));
            const size_t o = (size_t(y) * outW + x) * 3;
            bytes[o + 0] = uint8_t(c.x * 255.0f + 0.5f);
            bytes[o + 1] = uint8_t(c.y * 255.0f + 0.5f);
            bytes[o + 2] = uint8_t(c.z * 255.0f + 0.5f);
        }
    }
    if (!stbi_write_png(outPath.c_str(), outW, outH, 3, bytes.data(), outW * 3)) {
        std::fprintf(stderr, "could not write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", outPath.c_str(), outW, outH);
    return 0;
}
