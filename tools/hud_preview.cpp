// The figure for docs/PLAYER.md: the player's HUD, laid out by the shipping code, composited over a real engine frame
// with a software rasteriser.
//
// CI has no GPU, so the HUD's own renderer (shaders/hud.vert|frag, src/engine/hud_renderer.cpp) cannot run here. But the
// layout is CPU code (HudCanvas + drawPlayerHud), and a HUD is nothing but textured, alpha-blended triangles. This tool
// takes exactly the vertex list Renderer::drawHud would upload, rasterises it with the same blend the pipeline uses, and
// writes a PNG. If the layout changes, the figure changes with it; it cannot drift into a mockup.
//
//   hud_preview --out docs/player-hud.png --background docs/terrain-hero.png
//               --font editor/res/fonts/IBMPlexSansJP-Regular.ttf
//
// The corner panel is drawn by this tool, not by drawPlayerHud: it identifies the font the glyphs came from, which is the
// one thing a single frame cannot show by itself.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "engine/hud.h"
#include "engine/msdf.h"
#include "engine/text_atlas.h"
#include "player/hud_layout.h"

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA8
};

float bilinear(const ukiyo::FontAtlas& font, float u, float v, int channel) {
    // Bilinear, in texel space. The pipeline samples the same atlas with Filter::Linear, so a glyph edge lands where the
    // GPU would put it.
    const int w = font.width();
    const int h = font.height();
    float x = u * float(w) - 0.5f;
    float y = v * float(h) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    auto texel = [&](int tx, int ty) {
        tx = tx < 0 ? 0 : (tx >= w ? w - 1 : tx);
        ty = ty < 0 ? 0 : (ty >= h ? h - 1 : ty);
        return int(font.pixels()[(size_t(ty) * w + tx) * 4 + channel]);
    };
    const float a = float(texel(x0, y0)) * (1 - fx) + float(texel(x0 + 1, y0)) * fx;
    const float b = float(texel(x0, y0 + 1)) * (1 - fx) + float(texel(x0 + 1, y0 + 1)) * fx;
    return (a * (1 - fy) + b * fy) / 255.0f;
}

// hud.frag's own rule: the median of three distances, scaled by the page's range in screen pixels. `pc.p0.z` carries the
// range, which is what HudRenderer pushes for a field page and 0 for a coverage page.
float coverage(const ukiyo::FontAtlas& font, float u, float v, float range, const glm::vec2& uvPerPixel) {
    if (range <= 0.0f) return bilinear(font, u, v, 3);
    const float rangeUv = range / float(font.width());
    return float(ukiyo::msdf::coverageFromField(bilinear(font, u, v, 0), bilinear(font, u, v, 1),
                                                bilinear(font, u, v, 2), double(rangeUv), double(uvPerPixel.x),
                                                double(uvPerPixel.y)));
}

void blendPixel(Image& img, int x, int y, const glm::vec4& c) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
    uint8_t* p = &img.px[(size_t(y) * img.w + x) * 4];
    const float a = c.a < 0 ? 0 : (c.a > 1 ? 1 : c.a);
    const float r = c.r < 0 ? 0 : (c.r > 1 ? 1 : c.r);
    const float g = c.g < 0 ? 0 : (c.g > 1 ? 1 : c.g);
    const float b = c.b < 0 ? 0 : (c.b > 1 ? 1 : c.b);
    p[0] = uint8_t(std::lround(float(p[0]) * (1 - a) + r * 255.0f * a));
    p[1] = uint8_t(std::lround(float(p[1]) * (1 - a) + g * 255.0f * a));
    p[2] = uint8_t(std::lround(float(p[2]) * (1 - a) + b * 255.0f * a));
    p[3] = 255;
}

void triangle(Image& img, const ukiyo::FontAtlas& font, const ukiyo::HudVertex& a, const ukiyo::HudVertex& b,
              const ukiyo::HudVertex& c) {
    // Screen pixels, top-left origin, half-pixel centres: the three edges are evaluated at pixel centres.
    const float minX = std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}));
    const float maxX = std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}));
    const float minY = std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}));
    const float maxY = std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}));
    const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (std::fabs(area) < 1e-6f) return;
    // uv is affine over the triangle, so its screen derivative is exact: evaluate it a pixel away. This is what the GPU's
    // fwidth sees, and it is what the field reconstruction needs to make the edge one pixel wide.
    auto uvAt = [&](float x, float y) {
        const float u0 = ((b.pos.x - x) * (c.pos.y - y) - (b.pos.y - y) * (c.pos.x - x)) / area;
        const float u1 = ((c.pos.x - x) * (a.pos.y - y) - (c.pos.y - y) * (a.pos.x - x)) / area;
        return a.uv * u0 + b.uv * u1 + c.uv * (1.0f - u0 - u1);
    };
    for (int y = int(minY); y <= int(maxY); ++y) {
        for (int x = int(minX); x <= int(maxX); ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            // Barycentrics; a pixel centre exactly on an edge belongs to the triangle (>= 0 in the winding's sign).
            float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
            float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
            float w2 = 1.0f - w0 - w1;
            const float eps = -1e-5f;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            const glm::vec2 uv = a.uv * w0 + b.uv * w1 + c.uv * w2;
            glm::vec4 col = a.color * w0 + b.color * w1 + c.color * w2;
            // The atlas stores either coverage in alpha with white RGB, or three distances in RGB whose median is the
            // coverage - the same branch hud.frag takes on pc.p0.z, at the same screen derivative.
            if (font.loaded()) {
                col.a *= coverage(font, uv.x, uv.y, font.msdfRange(),
                                  {std::fabs(uvAt(px + 1.0f, py).x - uv.x) + std::fabs(uvAt(px + 1.0f, py).y - uv.y),
                                   std::fabs(uvAt(px, py + 1.0f).x - uv.x) + std::fabs(uvAt(px, py + 1.0f).y - uv.y)});
            }
            blendPixel(img, x, y, col);
        }
    }
}

void drawVertices(Image& img, const ukiyo::FontAtlas& font, const std::vector<ukiyo::HudVertex>& verts) {
    // HudCanvas emits each quad as two triangles: TL,TR,BR and TL,BR,BL. Six vertices, one triangle list, exactly what
    // Renderer::drawHud uploads as a non-indexed draw.
    for (size_t i = 0; i + 5 < verts.size(); i += 6) {
        triangle(img, font, verts[i + 0], verts[i + 1], verts[i + 2]);
        triangle(img, font, verts[i + 3], verts[i + 4], verts[i + 5]);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string out = "docs/player-hud.png";
    std::string background = "docs/terrain-hero.png";
    std::string fontPath = "editor/res/fonts/IBMPlexSansJP-Regular.ttf";
    int width = 1600, height = 900;
    bool msdf = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* dst) {
            if (i + 1 < argc) *dst = argv[++i];
        };
        if (a == "--out") next(&out);
        else if (a == "--background") next(&background);
        else if (a == "--font") next(&fontPath);
        else if (a == "--coverage") msdf = false;
        else if (a == "--width" && i + 1 < argc) width = std::stoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) height = std::stoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: hud_preview [--out docs/player-hud.png] [--background frame.png]\n"
                        "                   [--font font.ttf] [--width 1600] [--height 900] [--coverage]\n");
            return 0;
        }
    }

    Image img;
    img.w = width;
    img.h = height;
    img.px.assign(size_t(width) * height * 4, 0);

    // The frame under the HUD: a real render, stretched to the output size. Without one, a gradient so the tool still
    // produces something CI can diff for size (a flat image is a silent failure).
    int bw = 0, bh = 0, bc = 0;
    stbi_uc* bg = stbi_load(background.c_str(), &bw, &bh, &bc, 4);
    if (bg) {
        for (int y = 0; y < height; ++y) {
            const int sy = int((int64_t)y * bh / height);
            for (int x = 0; x < width; ++x) {
                const int sx = int((int64_t)x * bw / width);
                std::memcpy(&img.px[(size_t(y) * width + x) * 4], &bg[(size_t(sy) * bw + sx) * 4], 4);
            }
        }
        stbi_image_free(bg);
        std::printf("hud_preview: background %s (%dx%d)\n", background.c_str(), bw, bh);
    } else {
        std::printf("hud_preview: no background at %s, drawing a gradient\n", background.c_str());
        for (int y = 0; y < height; ++y) {
            const float t = float(y) / float(height);
            for (int x = 0; x < width; ++x) {
                uint8_t* p = &img.px[(size_t(y) * width + x) * 4];
                p[0] = uint8_t(std::lround(70 + 120 * (1.0f - t)));
                p[1] = uint8_t(std::lround(90 + 100 * (1.0f - t)));
                p[2] = uint8_t(std::lround(130 + 90 * (1.0f - t)));
                p[3] = 255;
            }
        }
    }

    // The player ships a multi-channel distance field page: 40 px, a 6-texel range, and hud.frag reconstructs the edge
    // from it at whatever size the layout asks for. `--coverage` renders the page the engine used before it, so the two
    // can be compared without a GPU.
    ukiyo::FontAtlas font;
    const bool ok = msdf ? font.loadMsdfFromFile(fontPath, 40.0f, 6.0f, 1024) : font.loadFromFile(fontPath, 20.0f, 1024);
    if (!ok) {
        std::fprintf(stderr, "hud_preview: could not load the font %s\n", fontPath.c_str());
        return 1;
    }
    font.preloadAscii();

    ukiyo::HudCanvas hud;
    hud.setFont(&font);
    hud.setScreen(float(width), float(height));

    // The shipped layout. The text is what PlayerApp::hudText produces for a running build.
    ukiyo::PlayerHudText text;
    text.title = "Seed Garden 0.4.0";
    text.stats = "60 fps   16.7 ms   128 draws   412.3k tris";
    text.hint = "WASD move   drag to orbit   wheel zoom   F1 hide HUD   Esc quit";
    text.game = "Lanterns 2 / 5";
    ukiyo::drawPlayerHud(hud, text, 1.0f);
    drawVertices(img, font, hud.vertices());

    // The tool's annotation: which font drew the glyphs. Preloaded explicitly so every code point is in the atlas.
    const std::string jp = "フォント: IBM Plex Sans JP — 日本語もこのアトラスから";
    font.preload(jp);
    font.preloadAscii();
    ukiyo::HudCanvas panel;
    panel.setFont(&font);
    panel.setScreen(float(width), float(height));
    const float pw = panel.textWidth(jp, 14.0f) + 24.0f;
    ukiyo::HudRect box;
    box.x = float(width) - pw - 16.0f;
    box.y = float(height) - 34.0f - 16.0f - 46.0f;
    box.w = pw;
    box.h = 46.0f;
    panel.rect(box, ukiyo::HudColor(0.04f, 0.045f, 0.06f, 0.62f));
    panel.border(box, 1.0f, ukiyo::HudColor(1.0f, 1.0f, 1.0f, 0.12f));
    panel.text(box.x + 12.0f, box.y + 7.0f, jp, ukiyo::HudColor(0.95f, 0.95f, 0.95f, 1.0f), 14.0f);
    panel.text(box.x + 12.0f, box.y + 25.0f, "FontAtlas 1024 x 1024   glyphs rasterised on demand",
               ukiyo::HudColor(0.72f, 0.74f, 0.78f, 0.9f), 12.0f);
    drawVertices(img, font, panel.vertices());

    if (!stbi_write_png(out.c_str(), width, height, 4, img.px.data(), width * 4)) {
        std::fprintf(stderr, "hud_preview: could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("hud_preview: wrote %s (%dx%d), %zu HUD vertices\n", out.c_str(), width, height, hud.vertices().size());
    return 0;
}
