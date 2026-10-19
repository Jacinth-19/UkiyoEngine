// Offline virtual-geometry preview (docs/VGEOM.md). Renders the Terrain Lab heightmap as one static mesh, builds the
// cluster tree with the engine's own buildVGeom, and draws three views side by side: the source geometry, the cut at a
// one-pixel budget, and the cut at four pixels. Clusters are tinted by level, so the picture shows the tree itself.
//
// This is a preview tool, not the engine: the cut, the cluster ranges and the triangle counts are the engine's, while
// the shading is a flat Lambert done here so a still can be made on a machine with no GL or Vulkan.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party -I third_party/meshoptimizer tools/vgeom_preview.cpp
//         src/engine/terrain.cpp src/engine/vgeom.cpp third_party/meshoptimizer/simplifier.cpp
//         third_party/meshoptimizer/allocator.cpp -o build/vgeom_preview
// Run:
//     ./build/vgeom_preview docs/vgeom-levels.png . 1800 900
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/terrain.h"
#include "engine/vgeom.h"

using namespace ukiyo;

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<glm::vec3> color;
    std::vector<float> depth;

    void reset(int width, int height) {
        w = width;
        h = height;
        color.assign(size_t(w) * h, glm::vec3(0.0f));
        depth.assign(size_t(w) * h, 1e30f);
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
    const glm::vec3 right = glm::normalize(glm::cross(c.fwd, glm::vec3(0, 1, 0)));
    c.right = right;
    c.up = glm::cross(right, c.fwd);
    c.focus = 0.5f * float(height) / std::tan(glm::radians(fovDegrees) * 0.5f);
    return c;
}

void rasterTri(Image& img, const Camera& cam, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
               const glm::vec3& color) {
    const glm::vec3 va = cam.toView(a), vb = cam.toView(b), vc = cam.toView(c);
    if (va.z <= 0.05f || vb.z <= 0.05f || vc.z <= 0.05f) return;   // no near-plane clipping: the camera stays outside
    auto screen = [&](const glm::vec3& v) {
        return glm::vec2(v.x / v.z * cam.focus + img.w * 0.5f, img.h * 0.5f - v.y / v.z * cam.focus);
    };
    const glm::vec2 p0 = screen(va), p1 = screen(vb), p2 = screen(vc);
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
            const float iz = l0 / va.z + l1 / vb.z + l2 / vc.z;
            if (iz <= 0.0f) continue;
            const size_t idx = size_t(y) * img.w + x;
            const float z = 1.0f / iz;
            if (z >= img.depth[idx]) continue;
            img.depth[idx] = z;
            img.color[idx] = color;
        }
    }
}

// The same six-step ramp the renderer would show, in sRGB, per level.
glm::vec3 levelColor(uint32_t level) {
    static const glm::vec3 kLevels[6] = {{0.36f, 0.68f, 0.44f}, {0.85f, 0.80f, 0.33f}, {0.93f, 0.58f, 0.26f},
                                         {0.82f, 0.33f, 0.31f}, {0.63f, 0.41f, 0.80f}, {0.33f, 0.55f, 0.86f}};
    return kLevels[std::min<uint32_t>(level, 5)];
}

glm::vec3 sunColor(glm::vec3 n, glm::vec3 base) {
    const glm::vec3 sun = glm::normalize(glm::vec3(0.46f, 0.72f, 0.52f));
    const float lambert = std::max(glm::dot(n, sun), 0.0f);
    return base * (0.30f + 0.78f * lambert);
}

glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

// A 5x7 bitmap font, just the glyphs this sheet needs. Text is drawn into the final image so the figure stays
// readable wherever the PNG is embedded; nothing here is part of the engine.
const uint8_t kGlyphRows[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},      {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},
    {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 2, 12},   {0, 0, 0, 0, 0, 12, 12},      {1, 1, 2, 4, 8, 16, 16},
    {0, 12, 12, 0, 12, 12, 0},    {24, 25, 2, 4, 8, 19, 3},     {0, 0, 0, 31, 0, 0, 0},
};
// Index of a glyph in kGlyphRows, or -1 for a character the sheet does not use.
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

void fillRect(std::vector<glm::vec3>& px, int w, int h, int x0, int y0, int rw, int rh, glm::vec3 c, float alpha) {
    for (int y = std::max(0, y0); y < std::min(h, y0 + rh); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x0 + rw); ++x) {
            glm::vec3& d = px[size_t(y) * w + x];
            d = glm::mix(d, c, alpha);
        }
}

int textWidth(const char* text, int scale) {
    int n = 0;
    for (const char* p = text; *p; ++p) n += (*p == ' ' ? 4 : 6) * scale;
    return n > 0 ? n - scale : 0;
}

void drawText(std::vector<glm::vec3>& px, int w, int h, int x, int y, const char* text, int scale, glm::vec3 c) {
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') { x += 4 * scale; continue; }
        const int g = glyphIndex(*p);
        if (g < 0) { x += 6 * scale; continue; }
        for (int row = 0; row < 7; ++row) {
            const uint8_t bits = kGlyphRows[g][row];
            for (int col = 0; col < 5; ++col)
                if (bits & (1u << (4 - col))) fillRect(px, w, h, x + col * scale, y + row * scale, scale, scale, c, 1.0f);
        }
        x += 6 * scale;
    }
}

std::string withThousands(size_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out.push_back(' ');
        out.push_back(digits[i]);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s out.png <repoRoot> [width height]\n", argv[0]);
        return 2;
    }
    const std::string outPath = argv[1];
    const std::string root = argv[2];
    const int outW = argc > 3 ? std::atoi(argv[3]) : 1800;
    const int outH = argc > 4 ? std::atoi(argv[4]) : 900;

    Heightmap map;
    map.cellSize = 1.0f;
    map.heightScale = 14.0f;
    std::string error;
    if (!loadHeightmapU16(root + "/tests/data/terrain_hills_257.u16", 257, &map, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    // One static mesh for the whole tile, shared vertices (the engine's chunks split them; the tree does not care).
    const uint32_t n = map.size;
    std::vector<float> positions;
    positions.reserve(size_t(n) * n * 3);
    for (uint32_t z = 0; z < n; ++z) {
        for (uint32_t x = 0; x < n; ++x) {
            positions.push_back(float(x) * map.cellSize);
            positions.push_back(map.heightAtMetres(float(x) * map.cellSize, float(z) * map.cellSize));
            positions.push_back(float(z) * map.cellSize);
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve(size_t(n - 1) * (n - 1) * 6);
    for (uint32_t z = 0; z + 1 < n; ++z) {
        for (uint32_t x = 0; x + 1 < n; ++x) {
            const uint32_t a = z * n + x, b = a + 1, c = a + n, d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    std::printf("mesh: %zu vertices, %zu triangles\n", positions.size() / 3, indices.size() / 3);

    const VGeomMesh mesh = buildVGeom(positions.data(), positions.size() / 3, sizeof(float) * 3, indices.data(),
                                      indices.size(), VGeomSettings{});
    if (!mesh.valid()) {
        std::fprintf(stderr, "the tree did not build\n");
        return 1;
    }
    size_t leaves = 0;
    for (const VGeomCluster& c : mesh.clusters) leaves += c.childCount == 0 ? 1 : 0;
    std::printf("tree: %zu clusters, %u levels, %zu leaves, %zu cluster triangles\n", mesh.clusters.size(), mesh.levels,
                leaves, mesh.triangleCount());

    const auto origin = [](const std::vector<float>& p, uint32_t v) {
        return glm::vec3(p[size_t(v) * 3], p[size_t(v) * 3 + 1], p[size_t(v) * 3 + 2]);
    };

    struct Panel {
        const char* label;
        float pixels;   // <= 0 draws the source
    };
    Panel panels[3] = {{"source", 0.0f}, {"1 px", 1.0f}, {"8 px", 8.0f}};
    if (const char* text = std::getenv("VG_BUDGETS")) {
        float a = 0, b = 0, c = 0;
        if (std::sscanf(text, "%f,%f,%f", &a, &b, &c) == 3) {
            panels[0].pixels = a;
            panels[1].pixels = b;
            panels[2].pixels = c;
        }
    }

    const int gutter = 12, margin = 12, legend = 46;
    const int panelW = (outW - margin * 2 - gutter * 2) / 3;
    const int panelH = outH - margin * 2 - legend;

    auto envVec = [](const char* name, glm::vec3 fallback) {
        const char* text = std::getenv(name);
        float x = 0, y = 0, z = 0;
        if (text && std::sscanf(text, "%f,%f,%f", &x, &y, &z) == 3) return glm::vec3(x, y, z);
        return fallback;
    };
    const glm::vec3 eye = envVec("VG_EYE", glm::vec3(300.0f, 150.0f, 300.0f));
    const glm::vec3 target = envVec("VG_TARGET", glm::vec3(128.0f, 8.0f, 128.0f));
    const Camera cam = makeCamera(eye, target, 46.0f, panelH);
    // The selection frustum has to match the raster's: same vertical FOV, the panel's aspect ratio.
    VGeomView view;
    view.viewProj = glm::perspective(glm::radians(46.0f), float(panelW) / float(panelH), 0.1f, 2000.0f) *
                    glm::lookAt(cam.eye, target, glm::vec3(0, 1, 0));
    view.eye = cam.eye;
    view.pixelScale = cam.focus;   // height / (2 tan(fov / 2)), exactly what the renderer passes
    view.cull = std::getenv("VG_NOCULL") == nullptr;
    std::vector<glm::vec3> finalPixels(size_t(outW) * outH, glm::vec3(0.045f, 0.050f, 0.058f));
    std::vector<uint8_t> sourceMask(size_t(panelW) * panelH, 0);
    std::string headline[3], detail[3];

    for (int p = 0; p < 3; ++p) {
        Image img;
        img.reset(panelW, panelH);
        std::vector<uint32_t> cut;
        VGeomStats stats;
        struct Tri {
            uint32_t v[3];
            uint32_t level;
        };
        std::vector<Tri> tris;
        if (panels[p].pixels > 0.0f) {
            // A cluster's range indexes the tree's own stream, which holds original vertex ids: read the vertices from
            // the cluster stream, not from the source index array (their triangle numbers are not the same).
            selectVGeom(mesh, glm::mat4(1.0f), view, panels[p].pixels, cut, &stats);
            for (uint32_t id : cut) {
                const VGeomCluster& c = mesh.clusters[id];
                for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; i += 3)
                    tris.push_back({{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]}, c.level});
            }
        } else {
            for (size_t t = 0; t < indices.size() / 3; ++t)
                tris.push_back({{indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]}, 0u});
        }
        const size_t drawn = tris.size();
        std::printf("%-6s %6zu triangles in %5zu clusters -> %.1f%% of the source", panels[p].label, drawn, cut.size(),
                    100.0 * double(drawn) / double(indices.size() / 3));
        char head[48], sub[64];
        if (panels[p].pixels > 0.0f) {
            std::snprintf(head, sizeof(head), "CUT AT %g PX", double(panels[p].pixels));
            std::snprintf(sub, sizeof(sub), "%s TRIS / %s CLUSTERS", withThousands(drawn).c_str(),
                          withThousands(cut.size()).c_str());
        } else {
            std::snprintf(head, sizeof(head), "SOURCE");
            std::snprintf(sub, sizeof(sub), "%s TRIS", withThousands(drawn).c_str());
        }
        headline[p] = head;
        detail[p] = sub;
        if (!cut.empty()) {
            int perLevel[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (uint32_t id : cut) ++perLevel[std::min<uint32_t>(mesh.clusters[id].level, 7)];
            std::printf("  levels");
            for (int l = 0; l < 8; ++l)
                if (perLevel[l]) std::printf(" %d:%d", l, perLevel[l]);
        }
        std::printf("\n");

        for (const Tri& tri : tris) {
            const glm::vec3 a = origin(positions, tri.v[0]), b = origin(positions, tri.v[1]), c = origin(positions, tri.v[2]);
            const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
            const glm::vec3 base = panels[p].pixels > 0.0f ? levelColor(tri.level) : glm::vec3(0.62f, 0.63f, 0.58f);
            rasterTri(img, cam, a, b, c, sunColor(normal, base));
        }
        if (p == 0) {
            for (size_t i = 0; i < sourceMask.size(); ++i) sourceMask[i] = img.depth[i] < 1e29f ? 1 : 0;
        } else {
            size_t covered = 0, missing = 0;
            for (size_t i = 0; i < sourceMask.size(); ++i) {
                if (!sourceMask[i]) continue;
                ++covered;
                if (img.depth[i] >= 1e29f) ++missing;
            }
            std::printf("  %s: %zu of %zu source pixels uncovered (%.3f%%)\n", panels[p].label, missing, covered,
                        100.0 * double(missing) / double(covered));
        }
        // Composite the panel into the sheet, sRGB, over a soft sky gradient.
        const int ox = margin + p * (panelW + gutter), oy = margin;
        for (int y = 0; y < panelH; ++y) {
            for (int x = 0; x < panelW; ++x) {
                const size_t src = size_t(y) * panelW + x;
                const glm::vec3 sky = glm::mix(glm::vec3(0.09f, 0.12f, 0.17f), glm::vec3(0.20f, 0.24f, 0.28f),
                                               float(y) / float(panelH));
                glm::vec3 c = img.depth[src] >= 1e29f ? sky : toSrgb(img.color[src]);
                if (p > 0 && sourceMask[src] && img.depth[src] >= 1e29f) c = glm::vec3(1.0f, 0.0f, 1.0f);   // hole
                finalPixels[size_t(oy + y) * outW + (ox + x)] = c;
            }
        }
    }

    // Panel captions, on a soft plate so they read over both sky and terrain.
    for (int p = 0; p < 3; ++p) {
        const int ox = margin + p * (panelW + gutter) + 3;
        const int oy = margin + 3;
        const int tw = std::max(textWidth(headline[p].c_str(), 2), textWidth(detail[p].c_str(), 2));
        fillRect(finalPixels, outW, outH, ox, oy, tw + 26, 56, glm::vec3(0.02f, 0.025f, 0.035f), 0.55f);
        drawText(finalPixels, outW, outH, ox + 13, oy + 8, headline[p].c_str(), 2, glm::vec3(0.95f, 0.95f, 0.93f));
        drawText(finalPixels, outW, outH, ox + 13, oy + 31, detail[p].c_str(), 2, glm::vec3(0.68f, 0.72f, 0.72f));
    }

    // Legend: one swatch per level at the foot of the sheet, in the same tint order the panels use.
    static const char* kLevelNames[6] = {"LEAF", "L1", "L2", "L3", "L4", "ROOT"};
    for (uint32_t l = 0; l < 6; ++l) {
        const int sw = 26, slot = 92;
        const int x0 = margin + int(l) * slot;
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < sw; ++x)
                finalPixels[size_t(outH - 25 + y) * outW + (x0 + x)] = toSrgb(levelColor(l));
        drawText(finalPixels, outW, outH, x0 + 34, outH - 29, kLevelNames[l], 2, glm::vec3(0.70f, 0.73f, 0.74f));
    }
    static const char* kFooter = "TINT BY CLUSTER LEVEL";
    drawText(finalPixels, outW, outH, outW - margin - textWidth(kFooter, 2), outH - 29, kFooter, 2,
             glm::vec3(0.46f, 0.50f, 0.52f));

    std::vector<uint8_t> bytes(size_t(outW) * outH * 3);
    for (size_t i = 0; i < finalPixels.size(); ++i) {
        const glm::vec3 c = glm::clamp(finalPixels[i], glm::vec3(0.0f), glm::vec3(1.0f));
        bytes[i * 3 + 0] = uint8_t(c.x * 255.0f + 0.5f);
        bytes[i * 3 + 1] = uint8_t(c.y * 255.0f + 0.5f);
        bytes[i * 3 + 2] = uint8_t(c.z * 255.0f + 0.5f);
    }
    if (!stbi_write_png(outPath.c_str(), outW, outH, 3, bytes.data(), outW * 3)) {
        std::fprintf(stderr, "could not write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", outPath.c_str(), outW, outH);
    return 0;
}
