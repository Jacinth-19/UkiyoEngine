// The figure for docs/CHARACTER.md: the capsule the runtime actually moves, drawn from the numbers the controller
// reports, not from a picture of one.
//
// The tool builds the same PhysicsWorld the tests use (a floor, a 0.3 m step), walks a real CharacterVirtual across it
// with Jolt underneath, jumps, and records CharacterState every 1/60 s. It then draws a side elevation: the ground is
// the collision geometry itself, found by raycasting down one column at a time, and each capsule outline is where the
// controller said the feet were. If the controller regresses, the figure changes.
//
//   character_preview --out docs/character.png --font editor/res/fonts/IBMPlexSansJP-Regular.ttf
//
// No GPU and no window: CI renders this on every push, which is why the figure cannot drift into a mockup.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "engine/text_atlas.h"
#include "systems/physics.h"

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA8
};

void blendPixel(Image& img, int x, int y, const glm::vec4& c) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
    uint8_t* p = &img.px[(size_t(y) * img.w + x) * 4];
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    p[0] = uint8_t(std::lround(float(p[0]) * (1 - a) + std::clamp(c.r, 0.0f, 1.0f) * 255.0f * a));
    p[1] = uint8_t(std::lround(float(p[1]) * (1 - a) + std::clamp(c.g, 0.0f, 1.0f) * 255.0f * a));
    p[2] = uint8_t(std::lround(float(p[2]) * (1 - a) + std::clamp(c.b, 0.0f, 1.0f) * 255.0f * a));
    p[3] = 255;
}

// A disc with a one-pixel-antialiased edge (coverage from the distance to the radius).
void disc(Image& img, float cx, float cy, float r, const glm::vec4& c) {
    const int x0 = int(std::floor(cx - r - 1)), x1 = int(std::ceil(cx + r + 1));
    const int y0 = int(std::floor(cy - r - 1)), y1 = int(std::ceil(cy + r + 1));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float d = std::sqrt((float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) +
                                      (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy));
            const float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            blendPixel(img, x, y, glm::vec4(c.r, c.g, c.b, c.a * cov));
        }
    }
}

void line(Image& img, float ax, float ay, float bx, float by, float width, const glm::vec4& c) {
    const float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    const int steps = int(len * 2.0f) + 1;
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        disc(img, ax + dx * t, ay + dy * t, width * 0.5f, c);
    }
}

// A capsule between the feet and the head: two caps and the rectangle between them, blended from the same colour so an
// overlapping capsule darkens where two frames sit on top of each other.
void capsule(Image& img, float fx, float fy, float hx, float hy, float r, const glm::vec4& c) {
    const float dx = hx - fx, dy = hy - fy;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float nx = len > 1e-4f ? -dy / len : 1.0f, ny = len > 1e-4f ? dx / len : 0.0f;
    // The body: a quad between the two cap centres, as a fan of discs along the axis (cheap and watertight).
    const int steps = std::max(2, int(len));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        disc(img, fx + dx * t, fy + dy * t, r, c);
    }
    // The outline: both silhouettes, so a capsule reads even when it overlaps the next one.
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        for (int s = -1; s <= 1; s += 2) {
            const float px = fx + dx * t + nx * r * float(s);
            const float py = fy + dy * t + ny * r * float(s);
            blendPixel(img, int(px), int(py), glm::vec4(0.35f, 0.72f, 0.92f, 0.55f));
        }
    }
}

// The outline of a vertical capsule: two sides and two half arcs. Ghost frames use this; only the final frame is filled.
void strokeCapsule(Image& img, float cx, float feetY, float headY, float r, const glm::vec4& c) {
    const int steps = std::max(8, int(r));
    for (int i = 0; i <= steps; ++i) {
        const float a = float(i) / float(steps) * 3.14159265f;
        // The bottom cap bulges down from the feet, the top cap bulges up from the head.
        blendPixel(img, int(cx - std::cos(a) * r), int(feetY + std::sin(a) * r), c);
        blendPixel(img, int(cx + std::cos(a) * r), int(feetY + std::sin(a) * r), c);
        blendPixel(img, int(cx - std::cos(a) * r), int(headY - std::sin(a) * r), c);
        blendPixel(img, int(cx + std::cos(a) * r), int(headY - std::sin(a) * r), c);
    }
    for (int y = int(headY); y <= int(feetY); ++y) {
        blendPixel(img, int(cx - r), y, c);
        blendPixel(img, int(cx + r), y, c);
    }
}

struct Text {
    const ukiyo::FontAtlas* font = nullptr;

    // Places glyphs the way `HudCanvas::text` does - pen + xoff, baseline + yoff - and copies their texels 1:1. Drawing
    // at the atlas's own size is what keeps the figure's text sharp: a scaled glyph is a resampled glyph.
    void draw(Image& img, float pen, float baseline, const std::string& str, const glm::vec4& col) const {
        if (!font || !font->loaded()) return;
        const int w = font->width(), h = font->height();
        auto texel = [&](int tx, int ty) {
            tx = std::clamp(tx, 0, w - 1);
            ty = std::clamp(ty, 0, h - 1);
            return float(font->pixels()[(size_t(ty) * size_t(w) + size_t(tx)) * 4 + 3]) / 255.0f;
        };
        auto sample = [&](float u, float v) {
            const float fx = u * float(w) - 0.5f, fy = v * float(h) - 0.5f;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const float ax = fx - float(x0), ay = fy - float(y0);
            const float a = texel(x0, y0) * (1 - ax) + texel(x0 + 1, y0) * ax;
            const float b = texel(x0, y0 + 1) * (1 - ax) + texel(x0 + 1, y0 + 1) * ax;
            return a * (1 - ay) + b * ay;
        };
        for (uint32_t cp : ukiyo::FontAtlas::decodeUtf8(str)) {
            const ukiyo::GlyphMetrics* g = const_cast<ukiyo::FontAtlas*>(font)->glyph(cp);
            if (!g) continue;
            const float gx = pen + g->xoff, gy = baseline + g->yoff;
            const int iw = int(std::lround(g->width)), ih = int(std::lround(g->height));
            for (int y = 0; y < ih; ++y) {
                for (int x = 0; x < iw; ++x) {
                    const float u = g->u0 + (g->u1 - g->u0) * (float(x) + 0.5f) / float(iw);
                    const float v = g->v0 + (g->v1 - g->v0) * (float(y) + 0.5f) / float(ih);
                    const float a = sample(u, v);
                    if (a > 0.008f) blendPixel(img, int(gx) + x, int(gy) + y, glm::vec4(col.r, col.g, col.b, col.a * a));
                }
            }
            pen += g->advance;
        }
    }
    float width(const std::string& str) const {
        if (!font || !font->loaded()) return 0.0f;
        float pen = 0.0f;
        for (uint32_t cp : ukiyo::FontAtlas::decodeUtf8(str))
            if (const ukiyo::GlyphMetrics* g = const_cast<ukiyo::FontAtlas*>(font)->glyph(cp)) pen += g->advance;
        return pen;
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string out = "docs/character.png";
    std::string fontPath = "editor/res/fonts/IBMPlexSansJP-Regular.ttf";
    int width = 1600, height = 900;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--font" && i + 1 < argc) fontPath = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = std::stoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) height = std::stoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: character_preview [--out docs/character.png] [--font font.ttf] [--width 1600] "
                        "[--height 900]\n");
            return 0;
        }
    }

    // --- The course, and a real character walking it ------------------------------------------------
    ukiyo::PhysicsWorld world;
    world.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    world.addStaticBox(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(20.0f, 0.5f, 8.0f));   // the floor, top at y = 0
    world.addStaticBox(glm::vec3(2.6f, 0.15f, 0.0f), glm::vec3(1.8f, 0.15f, 8.0f));    // a 0.3 m ledge from x 0.8 to 4.4

    ukiyo::CharacterSettings s;
    s.radius = 0.3f;
    s.height = 1.8f;
    ukiyo::CharacterHandle h = world.addCharacter(glm::vec3(-1.6f, 0.02f, 0.0f), s);

    const float dt = 1.0f / 60.0f;
    struct Sample { float t; glm::vec3 feet; ukiyo::CharacterState st; };
    std::vector<Sample> samples;
    auto step = [&](const glm::vec3& planar, bool jump, float seconds, bool record) {
        const int n = int(seconds / dt);
        for (int i = 0; i < n; ++i) {
            ukiyo::CharacterState st;
            world.moveCharacter(h, planar, jump && i == 0, dt, &st);
            if (record && (i % 15 == 0)) samples.push_back({0.0f, st.position, st});
        }
    };

    step(glm::vec3(0.0f), false, 0.5f, false);                        // land
    const float xStart = [&] { ukiyo::CharacterState st; world.characterState(h, &st); return st.position.x; }();
    step(glm::vec3(2.0f, 0.0f, 0.0f), false, 1.5f, true);             // walk up onto the ledge
    step(glm::vec3(0.0f), false, 0.2f, true);                         // stand
    step(glm::vec3(2.0f, 0.0f, 0.0f), true, 0.9f, true);              // jump forward
    step(glm::vec3(0.0f), false, 0.6f, true);                         // land and settle

    ukiyo::CharacterState end;
    world.characterState(h, &end);

    // The numbers the caption quotes, measured rather than typed in.
    float apex = 0.0f, run = 0.0f;
    for (const Sample& sm : samples) {
        apex = std::max(apex, sm.st.position.y);
        run = std::max(run, sm.st.position.x);
    }

    // --- The frame ----------------------------------------------------------------------------------
    Image img;
    img.w = width;
    img.h = height;
    img.px.assign(size_t(width) * size_t(height) * 4, 0);
    const float wx0 = -2.6f, wx1 = 5.4f, wy0 = -0.35f, wy1 = 3.45f;
    const float sx = float(width) / (wx1 - wx0);
    const float sy = float(height) / (wy1 - wy0);
    auto px = [&](float wx) { return (wx - wx0) * sx; };
    auto py = [&](float wy) { return float(height) - (wy - wy0) * sy; };

    // Sky: a dusk gradient with a faint horizon glow, so a flat frame is impossible.
    for (int y = 0; y < height; ++y) {
        const float t = float(y) / float(height);
        for (int x = 0; x < width; ++x) {
            const float glow = std::exp(-(t - 0.72f) * (t - 0.72f) * 26.0f) * 0.5f;
            uint8_t* p = &img.px[(size_t(y) * width + x) * 4];
            p[0] = uint8_t(std::lround(16.0f + 26.0f * t + 40.0f * glow));
            p[1] = uint8_t(std::lround(20.0f + 28.0f * t + 30.0f * glow));
            p[2] = uint8_t(std::lround(30.0f + 34.0f * t));
            p[3] = 255;
        }
    }

    // The ground, from the collision geometry: a ray down every column. This is the shape Jolt answers with, so the
    // capsule sits exactly on the step rather than floating over a drawing of one.
    for (int x = 0; x < width; ++x) {
        const float wx = wx0 + (float(x) + 0.5f) / sx;
        glm::vec3 hit(0.0f);
        float groundY = wy0;
        if (world.raycast(glm::vec3(wx, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 4.0f, &hit)) groundY = float(hit.y);
        const int y0 = int(py(groundY));
        for (int y = std::max(0, y0 + 2); y < height; ++y) {
            const float depth = float(y - y0) / float(height);
            blendPixel(img, x, y, glm::vec4(0.17f + 0.06f * (1 - depth), 0.19f + 0.07f * (1 - depth),
                                            0.18f + 0.06f * (1 - depth), 1.0f));
        }
        // A lit lip, 2 px, so the silhouette reads.
        blendPixel(img, x, y0, glm::vec4(0.56f, 0.68f, 0.52f, 1.0f));
        blendPixel(img, x, y0 + 1, glm::vec4(0.34f, 0.42f, 0.33f, 1.0f));
    }

    // A metre grid: vertical rules every 1 m, faint; and a labelled 1 m bar in the lower left.
    for (float gx = std::ceil(wx0); gx <= wx1; gx += 1.0f) {
        for (int y = 0; y < height; y += 7) blendPixel(img, int(px(gx)), y, glm::vec4(1, 1, 1, 0.045f));
    }
    for (float gy = 0.0f; gy <= 2.0f; gy += 1.0f) {
        for (int x = 0; x < width; x += 7) blendPixel(img, x, int(py(gy)), glm::vec4(1, 1, 1, 0.045f));
    }
    line(img, px(-2.35f), py(0.0f), px(-2.35f), py(1.0f), 3.0f, glm::vec4(0.85f, 0.87f, 0.90f, 0.85f));
    line(img, px(-2.45f), py(0.0f), px(-2.25f), py(0.0f), 2.0f, glm::vec4(0.85f, 0.87f, 0.90f, 0.7f));
    line(img, px(-2.45f), py(1.0f), px(-2.25f), py(1.0f), 2.0f, glm::vec4(0.85f, 0.87f, 0.90f, 0.7f));

    const float footToCap = s.radius;                               // the bottom cap bulges up from the feet
    const float capAxis = s.height - 2.0f * s.radius;               // cap centre to cap centre

    // Every sampled frame as an outline: the walk, the hop up the step, the jump arc.
    for (const Sample& sm : samples) {
        const float feet = py(sm.st.position.y) - footToCap * sy;
        strokeCapsule(img, px(sm.st.position.x), feet, feet - capAxis * sy, s.radius * sx,
                      glm::vec4(0.55f, 0.78f, 0.95f, 0.30f));
    }

    // The feet path, drawn over the ghosts so it stays readable.
    for (size_t i = 1; i < samples.size(); ++i) {
        line(img, px(samples[i - 1].st.position.x), py(samples[i - 1].st.position.y), px(samples[i].st.position.x),
             py(samples[i].st.position.y), 2.0f, glm::vec4(0.98f, 0.76f, 0.34f, 0.9f));
    }

    // The last frame, filled, with what it stands on.
    {
        const float fx = px(end.position.x), ground = py(end.position.y);
        const float feet = ground - footToCap * sy;
        capsule(img, fx, feet, fx, feet - capAxis * sy, s.radius * sx, glm::vec4(0.36f, 0.62f, 0.86f, 0.42f));
        strokeCapsule(img, fx, feet, feet - capAxis * sy, s.radius * sx, glm::vec4(0.68f, 0.86f, 1.0f, 0.95f));
        const glm::vec3 n = end.groundNormal;
        line(img, fx + 74.0f, ground, fx + 74.0f + n.x * 70.0f, ground - n.y * 70.0f, 2.5f,
             glm::vec4(0.55f, 0.95f, 0.55f, 0.95f));
        std::printf("character_preview: final feet (%.2f, %.2f, %.2f) grounded=%d normal.y=%.2f\n", end.position.x,
                    end.position.y, end.position.z, end.grounded ? 1 : 0, end.groundNormal.y);
    }

    // --- Labels ---------------------------------------------------------------------------------------
    ukiyo::FontAtlas title, body;
    const bool haveTitle = title.loadFromFile(fontPath, 34.0f, 1024);
    const bool haveBody = body.loadFromFile(fontPath, 20.0f, 1024);
    if (haveTitle) title.preloadAscii();
    if (haveBody) body.preloadAscii();
    const Text head{&title}, text{&body};
    if (haveTitle && haveBody) {
        head.draw(img, 44.0f, 74.0f, "The capsule character, walked by the shipping controller",
                  glm::vec4(0.95f, 0.96f, 0.98f, 1.0f));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "feet path %+.2f m at 2.0 m/s   step 0.30 m   jump apex %.2f m   grounded %s",
                      run - xStart, apex, end.grounded ? "yes" : "no");
        text.draw(img, 46.0f, 108.0f, buf, glm::vec4(0.80f, 0.84f, 0.90f, 1.0f));
        text.draw(img, px(-2.15f), py(0.0f) - 12.0f, "start", glm::vec4(0.80f, 0.84f, 0.74f, 1.0f));
        text.draw(img, px(1.35f), py(0.30f) - 22.0f, "a 0.3 m step", glm::vec4(0.80f, 0.84f, 0.74f, 1.0f));
        text.draw(img, px(end.position.x) + 92.0f, py(0.72f), "ground normal", glm::vec4(0.62f, 0.94f, 0.62f, 1.0f));
        text.draw(img, px(-2.52f), py(1.0f) - 14.0f, "1 m", glm::vec4(0.86f, 0.88f, 0.92f, 1.0f));
        text.draw(img, 46.0f, float(height) - 40.0f,
                  "each outline is one frame of CharacterState, every 0.25 s   -   capsule radius 0.3 m, height 1.8 m",
                  glm::vec4(0.70f, 0.74f, 0.80f, 1.0f));
    } else {
        std::printf("character_preview: no font at %s, writing the figure unlabelled\n", fontPath.c_str());
    }

    if (!stbi_write_png(out.c_str(), img.w, img.h, 4, img.px.data(), img.w * 4)) {
        std::printf("character_preview: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("character_preview: wrote %s (%dx%d), %zu samples\n", out.c_str(), img.w, img.h, samples.size());
    return 0;
}
