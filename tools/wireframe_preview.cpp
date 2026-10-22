// The figure in docs/WIREFRAME.md: one scene, one camera, drawn twice — flat-shaded, and as the edges of the triangles
// the renderer actually submits. The second panel is the wireframe view: same geometry, same projection, hidden lines
// removed by the same depth test the shaded panel uses, so what is visible is the mesh's own wiring rather than a mess of
// every triangle behind every other one.
//
// The scene is procedural on purpose. It is the shape of a debug view, not a shot: a displaced grid and three boxes, at
// the triangle counts and pixel sizes this engine's scenes actually reach. The GPU test (tests/wireframe_test.cpp)
// checks the RHI flag itself on both backends; this draws the picture the flag produces.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "figure_kit.h"

namespace {

struct Tri {
    glm::vec3 a, b, c;
    glm::vec3 colour;
};

struct Frame {
    int w = 0, h = 0;
    std::vector<float> depth;
    std::vector<glm::vec3> color;
    void reset(int width, int height, glm::vec3 bg) {
        w = width;
        h = height;
        depth.assign(size_t(w) * size_t(h), 1e30f);
        color.assign(size_t(w) * size_t(h), bg);
    }
    bool visible(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
};

struct Camera {
    glm::mat4 viewProj;
    glm::vec3 eye;
};

glm::vec3 project(const Camera& cam, const glm::vec3& p, float* z, float w, float h) {
    const glm::vec4 clip = cam.viewProj * glm::vec4(p, 1.0f);
    const float iw = 1.0f / std::max(clip.w, 1e-6f);
    *z = clip.w;   // view-space distance, which is what the depth test wants
    return glm::vec3((clip.x * iw * 0.5f + 0.5f) * w, (0.5f - clip.y * iw * 0.5f) * h, clip.z * iw);
}

void rasterise(Frame& f, const Camera& cam, const Tri& t, bool shade, const glm::vec3& light) {
    float za = 0, zb = 0, zc = 0;
    const glm::vec3 a = project(cam, t.a, &za, float(f.w), float(f.h));
    const glm::vec3 b = project(cam, t.b, &zb, float(f.w), float(f.h));
    const glm::vec3 c = project(cam, t.c, &zc, float(f.w), float(f.h));
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    const int minX = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int maxX = std::min(f.w - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int minY = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int maxY = std::min(f.h - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    const glm::vec3 n = glm::normalize(glm::cross(t.b - t.a, t.c - t.a));
    const float lambert = std::max(0.0f, glm::dot(n, light));
    const glm::vec3 ink = t.colour * (0.25f + 0.85f * lambert);
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            // Perspective-correct z would need the w's; the view-space distance interpolates linearly in screen space
            // only for w == 1. At these distances the error is under a pixel of edge, which is all this figure needs.
            const float z = za * w0 + zb * w1 + zc * w2;
            const size_t i = size_t(y) * f.w + x;
            if (z >= f.depth[i]) continue;
            f.depth[i] = z;
            if (shade) f.color[i] = ink;
        }
    }
}

// Bresenham with a depth test per pixel: an edge shows only where it is in front of the surface behind it, which is
// what makes a wireframe readable instead of a ball of string.
void line(Frame& f, int x0, int y0, float z0, int x1, int y1, float z1, const glm::vec3& col) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    int i = 0;
    for (;;) {
        if (f.visible(x0, y0)) {
            const float t = steps > 0 ? float(i) / float(steps) : 0.0f;
            const float z = z0 + (z1 - z0) * t;
            const size_t at = size_t(y0) * f.w + x0;
            // A small bias so an edge lying on its own triangle wins the comparison it just lost to it.
            if (z <= f.depth[at] * 1.004f + 1e-4f) f.color[at] = col;
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        ++i;
    }
}

void drawWireframe(Frame& f, const Camera& cam, const Tri& t, const glm::vec3& col) {
    float za = 0, zb = 0, zc = 0;
    const glm::vec3 a = project(cam, t.a, &za, float(f.w), float(f.h));
    const glm::vec3 b = project(cam, t.b, &zb, float(f.w), float(f.h));
    const glm::vec3 c = project(cam, t.c, &zc, float(f.w), float(f.h));
    line(f, int(a.x), int(a.y), za, int(b.x), int(b.y), zb, col);
    line(f, int(b.x), int(b.y), zb, int(c.x), int(c.y), zc, col);
    line(f, int(c.x), int(c.y), zc, int(a.x), int(a.y), za, col);
}

std::vector<Tri> scene() {
    std::vector<Tri> tris;
    const glm::vec3 ground(0.30f, 0.36f, 0.30f);
    const glm::vec3 warm(0.62f, 0.46f, 0.34f);
    const glm::vec3 cool(0.40f, 0.44f, 0.56f);
    // A displaced grid: five metres across, two triangles a cell.
    const int n = 14;
    const float span = 5.0f;
    auto height = [](float x, float z) {
        return 0.35f * std::sin(x * 1.1f) * std::cos(z * 0.9f) + 0.12f * std::sin(x * 2.7f + z * 1.9f);
    };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const float x0 = -span + 2.0f * span * float(i) / float(n);
            const float x1 = -span + 2.0f * span * float(i + 1) / float(n);
            const float z0 = -span + 2.0f * span * float(j) / float(n);
            const float z1 = -span + 2.0f * span * float(j + 1) / float(n);
            const glm::vec3 p00(x0, height(x0, z0), z0), p10(x1, height(x1, z0), z0);
            const glm::vec3 p11(x1, height(x1, z1), z1), p01(x0, height(x0, z1), z1);
            tris.push_back({p00, p10, p11, ground});
            tris.push_back({p00, p11, p01, ground});
        }
    // Three boxes: the thing a wireframe view is usually opened to look at.
    auto box = [&](glm::vec3 c, glm::vec3 half, glm::vec3 col) {
        const glm::vec3 v[8] = {{c.x - half.x, c.y - half.y, c.z - half.z}, {c.x + half.x, c.y - half.y, c.z - half.z},
                                {c.x + half.x, c.y + half.y, c.z - half.z}, {c.x - half.x, c.y + half.y, c.z - half.z},
                                {c.x - half.x, c.y - half.y, c.z + half.z}, {c.x + half.x, c.y - half.y, c.z + half.z},
                                {c.x + half.x, c.y + half.y, c.z + half.z}, {c.x - half.x, c.y + half.y, c.z + half.z}};
        const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 3, 7, 4}};
        for (const auto& f : faces) {
            const glm::vec3 n = glm::normalize(glm::cross(v[f[1]] - v[f[0]], v[f[2]] - v[f[0]]));
            const glm::vec3 shade = col * (0.75f + 0.25f * std::fabs(n.y));
            tris.push_back({v[f[0]], v[f[1]], v[f[2]], shade});
            tris.push_back({v[f[0]], v[f[2]], v[f[3]], shade});
        }
    };
    box({-1.2f, 0.55f, -0.4f}, {0.55f, 0.55f, 0.55f}, warm);
    box({0.5f, 0.35f, 0.9f}, {0.35f, 0.35f, 0.35f}, cool);
    box({1.4f, 0.22f, -1.5f}, {0.22f, 0.22f, 0.22f}, warm);
    return tris;
}

} // namespace

int main(int argc, char** argv) {
    std::string out = "docs/wireframe.png";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--out") out = argv[i + 1];

    const int W = 1600, H = 1000;
    figure::Image sheet;
    sheet.reset(W, H, glm::vec3(0.045f, 0.05f, 0.06f));

    figure::panelHeader(sheet, 30, 18, "SHADED", "THE FRAME THE PLAYER DRAWS");
    figure::panelHeader(sheet, 800, 18, "WIREFRAME", "THE SAME TRIANGLES, AS THEIR EDGES");

    const std::vector<Tri> tris = scene();
    std::printf("wireframe_preview: %zu triangles\n", tris.size());

    const glm::vec3 eye(4.6f, 3.4f, 6.4f);
    const glm::mat4 view = glm::lookAtRH(eye, glm::vec3(0.0f, 0.25f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(38.0f), 1.0f, 0.1f, 60.0f);
    Camera cam;
    cam.viewProj = proj * view;
    cam.eye = eye;
    const glm::vec3 light = glm::normalize(glm::vec3(-0.55f, 0.78f, 0.30f));

    const int panelW = 700, panelH = 700;

    Frame shaded, wired;
    shaded.reset(panelW, panelH, glm::vec3(0.055f, 0.065f, 0.08f));
    wired.reset(panelW, panelH, glm::vec3(0.055f, 0.065f, 0.08f));
    size_t shadedPixels = 0;
    for (const Tri& t : tris) rasterise(shaded, cam, t, true, light);
    for (int i = 0; i < panelW * panelH; ++i)
        if (shaded.depth[size_t(i)] < 1e29f) ++shadedPixels;
    // The wireframe panel starts from the shaded one's depth buffer, so hidden lines are hidden by the geometry itself.
    wired.depth = shaded.depth;
    for (const Tri& t : tris) drawWireframe(wired, cam, t, glm::vec3(0.55f, 0.86f, 0.70f));
    size_t wirePixels = 0;
    for (int i = 0; i < panelW * panelH; ++i)
        if (wired.color[size_t(i)] != glm::vec3(0.055f, 0.065f, 0.08f)) ++wirePixels;

    auto frame = [&](int x0, int y0) {
        figure::fillRect(sheet, x0 - 2, y0 - 2, panelW + 4, panelH + 4, glm::vec3(0.14f, 0.16f, 0.19f), 1.0f);
        figure::fillRect(sheet, x0, y0, panelW, panelH, glm::vec3(0.055f, 0.065f, 0.08f), 1.0f);
    };
    frame(70, 110);
    frame(840, 110);
    for (int y = 0; y < panelH; ++y) {
        for (int x = 0; x < panelW; ++x) {
            sheet.set(70 + x, 110 + y, figure::toSrgb(shaded.color[size_t(y) * panelW + x]));
            sheet.set(840 + x, 110 + y, figure::toSrgb(wired.color[size_t(y) * panelW + x]));
        }
    }

    char buf[256];
    std::snprintf(buf, sizeof buf, "%zu TRIANGLES / %zu EDGE PIXELS", tris.size(), wirePixels);
    figure::drawText(sheet, 840, 838, buf, 2, glm::vec3(0.68f, 0.74f, 0.82f));
    std::snprintf(buf, sizeof buf, "%.2f PERCENT OF THE PANEL", 100.0 * double(wirePixels) / double(panelW * panelH));
    figure::drawText(sheet, 840, 858, buf, 2, glm::vec3(0.48f, 0.54f, 0.62f));
    std::snprintf(buf, sizeof buf, "SHADED COVERS %.1f PERCENT, THE EDGES %.2f", 100.0 * double(shadedPixels) / double(panelW * panelH),
                  100.0 * double(wirePixels) / double(panelW * panelH));
    figure::drawText(sheet, 70, 838, buf, 2, glm::vec3(0.68f, 0.74f, 0.82f));
    figure::drawText(sheet, 70, 858, "THE SAME 428 TRIANGLES, TWO RASTERISER MODES", 2, glm::vec3(0.48f, 0.54f, 0.62f));
    figure::drawText(sheet, 70, 902, "VULKAN: FILLMODENONSOLID + VK_POLYGON_MODE_LINE.   GL: GLPOLYGONMODE LINE.", 2,
                     glm::vec3(0.45f, 0.50f, 0.58f));
    figure::drawText(sheet, 70, 930, "FULLSCREEN AND SHADOW PASSES STAY SOLID - ONLY GEOMETRY BECOMES EDGES.", 2,
                     glm::vec3(0.45f, 0.50f, 0.58f));

    if (!figure::savePng(sheet, out)) {
        std::fprintf(stderr, "wireframe_preview: could not write %s\n", out.c_str());
        return 1;
    }
    return 0;
}
