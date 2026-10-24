// Offline contact-shadow preview (docs/CONTACT_SHADOWS.md): what the screen-space pass adds to a shadow map that is too
// coarse to resolve a contact.
//
// The scene is a crate and three small props on a floor under a low sun. The shadow map is 32 x 32 over a 4.4 m frustum,
// which puts one texel at 13.8 cm and forces about 20 cm of depth bias: the crate's shadow detaches from its base and the
// props lose their shadow entirely. The march in src/engine/contact_shadow.h reads the camera's own depth buffer instead
// and resolves contact at pixel width.
//
// The sheet is a 2 x 2 grid: the shadow map alone, the contact pass alone, both, and a legend with the measurements taken
// on the rendered frame.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party -I third_party/glm tools/contact_shadow_preview.cpp
//         -o build/contact_shadow_preview
// Run:
//     ./build/contact_shadow_preview docs/contact-shadows.png 1600 1000
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

#include "engine/contact_shadow.h"

using namespace ukiyo;

namespace {

// ---- scene ---------------------------------------------------------------------------------

const glm::vec3 kSun = glm::normalize(glm::vec3(-0.55f, 0.45f, 0.30f));   // towards the sun

// A crate on the floor, plus three props whose footprint is a fraction of one shadow-map texel. Those are the cases the
// pass exists for.
struct Box {
    glm::vec3 lo, hi;
    glm::vec3 albedo;
};

const Box kBoxes[] = {
    {{-0.50f, 0.00f, -0.50f}, {0.50f, 1.00f, 0.50f}, {0.80f, 0.42f, 0.30f}},
    {{1.04f, 0.00f, 0.52f}, {1.19f, 0.15f, 0.67f}, {0.34f, 0.55f, 0.62f}},
    {{1.34f, 0.00f, 0.34f}, {1.49f, 0.30f, 0.49f}, {0.62f, 0.62f, 0.35f}},
    {{1.66f, 0.00f, 0.58f}, {1.81f, 0.10f, 0.73f}, {0.55f, 0.40f, 0.70f}},
};

struct Hit {
    bool hit = false;
    float t = 0.0f;
    glm::vec3 p{0.0f};
    glm::vec3 n{0.0f};
    glm::vec3 albedo{0.5f};
};

bool slab(const Box& b, const glm::vec3& o, const glm::vec3& d, float* tNear) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < b.lo[i] || o[i] > b.hi[i]) return false;
            continue;
        }
        float a = (b.lo[i] - o[i]) / d[i], c = (b.hi[i] - o[i]) / d[i];
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a);
        t1 = std::min(t1, c);
        if (t0 > t1) return false;
    }
    *tNear = t0;
    return t0 > 1e-4f;
}

glm::vec3 boxNormal(const Box& b, const glm::vec3& p) {
    const float e = 1e-3f;
    if (std::fabs(p.x - b.hi.x) < e) return {1, 0, 0};
    if (std::fabs(p.x - b.lo.x) < e) return {-1, 0, 0};
    if (std::fabs(p.z - b.hi.z) < e) return {0, 0, 1};
    if (std::fabs(p.z - b.lo.z) < e) return {0, 0, -1};
    if (std::fabs(p.y - b.hi.y) < e) return {0, 1, 0};
    return {0, -1, 0};
}

Hit trace(const glm::vec3& o, const glm::vec3& d) {
    Hit h;
    float best = 1e30f;
    if (std::fabs(d.y) > 1e-6f) {
        const float t = -o.y / d.y;
        if (t > 1e-4f) {
            best = t;
            h.hit = true;
            h.t = t;
            h.p = o + d * t;
            h.n = {0, 1, 0};
            h.albedo = {0.52f, 0.53f, 0.58f};
        }
    }
    for (const Box& b : kBoxes) {
        float t = 0.0f;
        if (slab(b, o, d, &t) && t < best) {
            best = t;
            h.hit = true;
            h.t = t;
            h.p = o + d * t;
            h.n = boxNormal(b, h.p);
            h.albedo = b.albedo;
        }
    }
    return h;
}

// Distance along the sun, in metres, to the first occluder (a big number when nothing blocks it).
float sunDistance(const glm::vec3& p) {
    const Hit h = trace(p + kSun * 1e-3f, kSun);
    return h.hit ? h.t : 1e9f;
}

bool sunBlocked(const glm::vec3& p) { return sunDistance(p) < 1e8f; }

// ---- shadow map ----------------------------------------------------------------------------

constexpr int kMapSize = 32;
constexpr float kMapHalfExtent = 2.2f;   // 4.4 m across, so one texel is 13.8 cm
constexpr float kMapBias = 0.20f;        // what a map that covers a whole view needs at this texel size

// A small orthographic depth map from the sun, rendered by ray casting. Deliberately coarse: the point of the figure is
// what a map that cannot resolve a contact looks like.
struct ShadowMap {
    glm::mat4 viewProj{1.0f};
    glm::vec3 eye{0.0f};
    std::vector<float> depth;   // distance along the light, metres

    ShadowMap() {
        eye = glm::vec3(0.0f, 0.4f, 0.0f) + kSun * 8.0f;
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.4f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 proj =
            glm::ortho(-kMapHalfExtent, kMapHalfExtent, -1.6f, 1.6f, 0.1f, 20.0f);
        viewProj = proj * view;
        depth.assign(size_t(kMapSize) * kMapSize, 1e9f);
        const glm::mat4 inv = glm::inverse(viewProj);
        for (int y = 0; y < kMapSize; ++y) {
            for (int x = 0; x < kMapSize; ++x) {
                const glm::vec4 ndc((float(x) + 0.5f) / kMapSize * 2.0f - 1.0f,
                                    (float(y) + 0.5f) / kMapSize * 2.0f - 1.0f, -1.0f, 1.0f);
                const glm::vec4 nearP = inv * ndc;
                const glm::vec4 farP = inv * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
                const glm::vec3 a = glm::vec3(nearP) / nearP.w;
                const glm::vec3 b = glm::vec3(farP) / farP.w;
                const Hit h = trace(a, glm::normalize(b - a));
                // Depth along the light, not euclidean distance from the eye: the map is orthographic, so the axial
                // distance is what the comparison in visibility() must use.
                if (h.hit) depth[size_t(y) * kMapSize + x] = glm::dot(h.p - eye, -kSun);
            }
        }
    }

    // Visibility from one nearest texel, with a bias generous enough for a map that has to cover a whole scene.
    float visibility(const glm::vec3& world) const {
        const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 0.0f) return 1.0f;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z > 1.0f) return 1.0f;
        const int x = int((ndc.x * 0.5f + 0.5f) * kMapSize);
        const int y = int((ndc.y * 0.5f + 0.5f) * kMapSize);
        if (x < 0 || y < 0 || x >= kMapSize || y >= kMapSize) return 1.0f;
        const float d = depth[size_t(y) * kMapSize + x];
        return (glm::dot(world - eye, -kSun) - kMapBias <= d) ? 1.0f : 0.0f;
    }
};

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

int glyphIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
        case '.': return 36;
        case '/': return 37;
        case ':': return 38;
        case '-': return 39;
        case '+': return 40;
        case '%': return 41;
        default: return -1;
    }
}

void fillRect(Image& img, int x0, int y0, int rw, int rh, glm::vec3 c, float alpha) {
    for (int y = std::max(0, y0); y < std::min(img.h, y0 + rh); ++y)
        for (int x = std::max(0, x0); x < std::min(img.w, x0 + rw); ++x)
            img.color[size_t(y) * img.w + x] = glm::mix(img.color[size_t(y) * img.w + x], c, alpha);
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
                    fillRect(img, x + col * scale, y + row * scale, scale, scale, c, 1.0f);
        x += 6 * scale;
    }
}

glm::vec3 toSrgb(glm::vec3 c);

// A 2x nearest-neighbour crop of the shaded frame, so the contact band is legible at sheet size.
void drawInset(Image& sheet, const glm::vec3* src, int srcW, int srcH, int cropX, int cropY, int cropW,
               int cropH, int dstX, int dstY, int zoom) {
    for (int y = 0; y < cropH * zoom; ++y) {
        for (int x = 0; x < cropW * zoom; ++x) {
            const int sx = std::clamp(cropX + x / zoom, 0, srcW - 1);
            const int sy = std::clamp(cropY + y / zoom, 0, srcH - 1);
            const int dx = dstX + x, dy = dstY + y;
            if (dx < 0 || dy < 0 || dx >= sheet.w || dy >= sheet.h) continue;
            sheet.color[size_t(dy) * sheet.w + dx] = toSrgb(src[size_t(sy) * srcW + sx]);
        }
    }
    fillRect(sheet, dstX, dstY, cropW * zoom, 2, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);
    fillRect(sheet, dstX, dstY + cropH * zoom - 2, cropW * zoom, 2, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);
    fillRect(sheet, dstX, dstY, 2, cropH * zoom, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);
    fillRect(sheet, dstX + cropW * zoom - 2, dstY, 2, cropH * zoom, glm::vec3(0.10f, 0.12f, 0.15f), 1.0f);
}

glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

glm::vec3 shade(const Hit& h, float shadow) {
    const float ndl = std::max(0.0f, glm::dot(h.n, kSun));
    const float ambient = 0.16f;
    const glm::vec3 sun(1.0f, 0.96f, 0.88f);
    return h.albedo * (ambient * glm::vec3(0.55f, 0.65f, 0.85f) + sun * (ndl * shadow));
}

} // namespace

int main(int argc, char** argv) {
    const std::string outPath = argc > 1 ? argv[1] : "docs/contact-shadows.png";
    const int outW = argc > 2 ? std::atoi(argv[2]) : 1600;
    const int outH = argc > 3 ? std::atoi(argv[3]) : 1000;

    // 2 x 2 sheet: a header strip, then four cells of outW/2 by (outH - strip)/2. Each render is twice a cell, so the
    // downsampled panels keep the render's aspect ratio exactly.
    const int strip = 108;
    const int panelW = outW / 2;
    const int panelH = (outH - strip) / 2;
    const int renderW = panelW * 2;
    const int renderH = panelH * 2;

    // Close on the crate's base and the props: the figure is about the first 30 cm of contact, not about the whole room.
    const glm::vec3 eye(2.48f, 1.06f, 2.86f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.85f, 0.14f, 0.45f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(44.0f), float(renderW) / float(renderH), 0.05f, 50.0f);
    const glm::mat4 invProj = glm::inverse(proj);
    const glm::mat4 invView = glm::inverse(view);

    const ShadowMap shadowMap;
    ContactShadowParams params;
    params.strength = 1.0f;
    // The engine's default thickness (8 cm) is tuned for screen-space depth precision on thin geometry. The crate here is
    // a thick occluder, so the figure gives the march the thickness the crate actually has; the legend says so.
    params.thickness = 0.30f;

    // Pass 1: the camera's own depth buffer and primary hits, which the march needs before anything is shaded.
    struct Fragment {
        bool hit = false;
        Hit hitInfo;
        float eyeDepth = 0.0f;
    };
    std::vector<Fragment> fragments(size_t(renderW) * renderH);
    for (int y = 0; y < renderH; ++y) {
        for (int x = 0; x < renderW; ++x) {
            const glm::vec4 clip((float(x) + 0.5f) / renderW * 2.0f - 1.0f,
                                 1.0f - (float(y) + 0.5f) / renderH * 2.0f, 1.0f, 1.0f);
            const glm::vec4 farP = invProj * clip;
            const glm::vec3 dirView = glm::normalize(glm::vec3(farP) / farP.w);
            const glm::vec3 dirWorld = glm::normalize(glm::vec3(invView * glm::vec4(dirView, 0.0f)));
            Fragment& f = fragments[size_t(y) * renderW + x];
            f.hitInfo = trace(eye, dirWorld);
            f.hit = f.hitInfo.hit;
            if (f.hit) {
                const glm::vec3 vp = glm::vec3(view * glm::vec4(f.hitInfo.p, 1.0f));
                f.eyeDepth = -vp.z;
            }
        }
    }
    // The march's sampler: nearest-texel eye depth, 0 for sky.
    const auto depthAt = [&](const glm::vec2& uv) {
        const int x = std::min(renderW - 1, std::max(0, int(uv.x * float(renderW))));
        const int y = std::min(renderH - 1, std::max(0, int((1.0f - uv.y) * float(renderH))));
        const Fragment& f = fragments[size_t(y) * renderW + x];
        return f.hit ? f.eyeDepth : 0.0f;
    };

    // Pass 2: shade three ways, and measure.
    std::vector<glm::vec3> panelMap(size_t(renderW) * renderH), panelContact(size_t(renderW) * renderH),
        panelBoth(size_t(renderW) * renderH);
    long contactPixels = 0, contactDarkened = 0, litPixels = 0, litDarkened = 0;
    for (int y = 0; y < renderH; ++y) {
        for (int x = 0; x < renderW; ++x) {
            const size_t at = size_t(y) * renderW + x;
            const Fragment& f = fragments[at];
            const glm::vec3 sky =
                glm::mix(glm::vec3(0.28f, 0.38f, 0.55f), glm::vec3(0.62f, 0.72f, 0.86f), float(y) / renderH);
            if (!f.hit) {
                panelMap[at] = panelContact[at] = panelBoth[at] = sky;
                continue;
            }
            const Hit& h = f.hitInfo;
            const float mapped = shadowMap.visibility(h.p);
            const glm::vec3 vp = glm::vec3(view * glm::vec4(h.p, 1.0f));
            const glm::vec3 vn = glm::mat3(view) * h.n;
            const glm::vec3 sunView = glm::mat3(view) * kSun;
            const float contact = contactShadowVisibility(vp, vn, sunView, proj, glm::vec2(float(x), float(y)),
                                                          depthAt, params, glm::vec2(float(renderW), float(renderH)));
            panelMap[at] = shade(h, mapped);
            panelContact[at] = shade(h, contact);
            panelBoth[at] = shade(h, mapped * contact);

            // Measurement. The "contact band" is every sun-facing pixel with an occluder inside the march's reach: the
            // strip where a contact is supposed to be resolved. The two reported numbers are how much of that band the
            // pass darkens, and how many lit pixels it wrongly touches (what a bad bias or an over-long ray inflates).
            const float facing = glm::dot(glm::normalize(h.n), kSun);
            const bool litBySun = !sunBlocked(h.p);
            if (facing > 0.0f && litBySun) {
                ++litPixels;
                if (contact < 0.9f) ++litDarkened;
            }
            if (facing > 0.0f && sunDistance(h.p) < params.length) {
                ++contactPixels;
                if (contact < 0.9f) ++contactDarkened;
            }
        }
    }

    const double bandDarkened = contactPixels ? 100.0 * double(contactDarkened) / double(contactPixels) : 0.0;
    const double falsePositive = litPixels ? 100.0 * double(litDarkened) / double(litPixels) : 0.0;

    // Sheet.
    Image sheet;
    sheet.reset(outW, outH);
    fillRect(sheet, 0, 0, outW, outH, glm::vec3(0.035f, 0.040f, 0.050f), 1.0f);

    const glm::vec3* panels[3] = {panelMap.data(), panelContact.data(), panelBoth.data()};
    const char* names[3] = {"SHADOW MAP ONLY", "CONTACT SHADOWS ONLY", "BOTH"};
    const char* notes[3] = {"32 X 32 TEXELS, 20 CM BIAS", "THE DEPTH-BUFFER MARCH", "WHAT A FRAME LOOKS LIKE"};
    // Reduce each panel to its cell size once: the 2 x 2 box average stands in for the four sub-pixel samples a
    // temporal filter would have collected, and the insets read from the reduced image so they show that average.
    std::vector<glm::vec3> small[3];
    for (int i = 0; i < 3; ++i) {
        small[i].resize(size_t(panelW) * panelH);
        for (int y = 0; y < panelH; ++y) {
            const int sy0 = y * renderH / panelH;
            const int sy1 = std::max(sy0 + 1, (y + 1) * renderH / panelH);
            for (int x = 0; x < panelW; ++x) {
                const int sx0 = x * renderW / panelW;
                const int sx1 = std::max(sx0 + 1, (x + 1) * renderW / panelW);
                glm::vec3 sum(0.0f);
                for (int sy = sy0; sy < sy1; ++sy)
                    for (int sx = sx0; sx < sx1; ++sx) sum += panels[i][size_t(sy) * renderW + sx];
                small[i][size_t(y) * panelW + x] = sum / float((sy1 - sy0) * (sx1 - sx0));
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        const int cellX = (i % 2) * panelW;
        const int cellY = strip + (i / 2) * panelH;
        for (int y = 0; y < panelH; ++y)
            for (int x = 0; x < panelW; ++x)
                sheet.color[size_t(cellY + y) * outW + (cellX + x)] = toSrgb(small[i][size_t(y) * panelW + x]);
        const int boxW = std::max(textWidth(names[i], 2), textWidth(notes[i], 2)) + 24;
        fillRect(sheet, cellX + 14, cellY + 14, boxW, 62, glm::vec3(0.02f, 0.02f, 0.03f), 0.68f);
        drawText(sheet, cellX + 26, cellY + 24, names[i], 2, glm::vec3(0.95f, 0.96f, 0.99f));
        drawText(sheet, cellX + 26, cellY + 46, notes[i], 2, glm::vec3(0.62f, 0.68f, 0.76f));

        // 2x crop of the crate's base and the nearest props: the scale the whole figure is about.
        const glm::vec4 c = proj * view * glm::vec4(0.86f, 0.06f, 0.34f, 1.0f);
        const glm::vec2 ndc = glm::vec2(c.x, c.y) / c.w;
        const int px = int((ndc.x * 0.5f + 0.5f) * panelW);
        const int py = int((1.0f - (ndc.y * 0.5f + 0.5f)) * panelH);
        const int cropW = 190, cropH = 107, zoom = 2;
        const int cx = std::clamp(px - cropW / 2, 0, panelW - cropW);
        const int cy = std::clamp(py - cropH / 2, 0, panelH - cropH);
        const int insetX = cellX + panelW - cropW * zoom - 22;
        const int insetY = cellY + panelH - cropH * zoom - 22;
        drawInset(sheet, small[i].data(), panelW, panelH, cx, cy, cropW, cropH, insetX, insetY, zoom);
        fillRect(sheet, insetX + 8, insetY + 8, 44, 22, glm::vec3(0.02f, 0.02f, 0.03f), 0.72f);
        drawText(sheet, insetX + 14, insetY + 14, "2 X", 2, glm::vec3(0.85f, 0.89f, 0.94f));
    }

    // Legend cell: what the figure shows, in the space the fourth panel would have taken.
    {
        const int cellX = panelW, cellY = strip + panelH;
        fillRect(sheet, cellX + 1, cellY, panelW - 1, panelH, glm::vec3(0.055f, 0.062f, 0.075f), 1.0f);
        const glm::vec3 title(0.92f, 0.94f, 0.97f);
        const glm::vec3 body(0.60f, 0.67f, 0.76f);
        const glm::vec3 dim(0.42f, 0.48f, 0.57f);
        int y = cellY + 34;
        drawText(sheet, cellX + 34, y, "READING THE FIGURE", 2, title);
        y += 40;
        drawText(sheet, cellX + 34, y, "THE MAP STORES ONE DEPTH PER 13.8 CM TEXEL, SO", 2, body);
        y += 20;
        drawText(sheet, cellX + 34, y, "IT NEEDS 20 CM OF BIAS. THE PROPS CAST A BLOCKY", 2, body);
        y += 20;
        drawText(sheet, cellX + 34, y, "SQUARE AND THE CRATE'S SHADOW PULLS AWAY FROM", 2, body);
        y += 20;
        drawText(sheet, cellX + 34, y, "ITS BASE, WHERE THE EYE READS THE GAP.", 2, body);
        y += 40;
        drawText(sheet, cellX + 34, y, "THE PASS MARCHES THE DEPTH BUFFER THAT IS", 2, body);
        y += 20;
        drawText(sheet, cellX + 34, y, "ALREADY BOUND TO THE EFFECTS SHADER, SO A", 2, body);
        y += 20;
        drawText(sheet, cellX + 34, y, "CONTACT IS RESOLVED AT PIXEL WIDTH AND THE", 2, body);
        y += 20;
        drawText(sheet, cellX + 34, y, "COST IS 24 DEPTH SAMPLES PER PIXEL.", 2, body);
        y += 44;
        char line[256];
        std::snprintf(line, sizeof(line), "CONTACT BAND: %ld PX, %.0f%% DARKENED", contactPixels, bandDarkened);
        drawText(sheet, cellX + 34, y, line, 2, glm::vec3(0.78f, 0.85f, 0.93f));
        y += 20;
        std::snprintf(line, sizeof(line), "LIT PIXELS TOUCHED: %.2f%%", falsePositive);
        drawText(sheet, cellX + 34, y, line, 2, dim);
        y += 20;
        std::snprintf(line, sizeof(line), "LENGTH %.2f M - THICKNESS %.2f M - %d STEPS", params.length,
                      params.thickness, params.steps);
        drawText(sheet, cellX + 34, y, line, 2, dim);
    }

    for (int x = 0; x < outW; ++x) fillRect(sheet, x, strip - 2, 1, 2, glm::vec3(0.16f, 0.19f, 0.24f), 1.0f);
    for (int y = strip; y < outH; ++y) fillRect(sheet, panelW - 1, y, 2, 2, glm::vec3(0.16f, 0.19f, 0.24f), 1.0f);
    for (int x = 0; x < outW; ++x) fillRect(sheet, x, strip + panelH - 1, 1, 2, glm::vec3(0.16f, 0.19f, 0.24f), 1.0f);

    drawText(sheet, 26, 18, "SCREEN-SPACE CONTACT SHADOWS", 3, glm::vec3(0.95f, 0.96f, 0.99f));
    drawText(sheet, 26, 48, "GH-118045 - THE MARCH IN SRC/ENGINE/CONTACT_SHADOW.H - DOCS/CONTACT_SHADOWS.MD", 2,
             glm::vec3(0.55f, 0.62f, 0.72f));
    char line[256];
    std::snprintf(line, sizeof(line), "CONTACT BAND: %ld PX - THE PASS DARKENS %.0f%% OF IT", contactPixels, bandDarkened);
    drawText(sheet, outW - 26 - textWidth(line, 2), 26, line, 2, glm::vec3(0.78f, 0.85f, 0.93f));
    std::snprintf(line, sizeof(line), "LIT PIXELS DARKENED BY MISTAKE: %.2f%% OF %ld", falsePositive, litPixels);
    drawText(sheet, outW - 26 - textWidth(line, 2), 52, line, 2, glm::vec3(0.45f, 0.52f, 0.62f));
    std::snprintf(line, sizeof(line), "LENGTH %.2f M - THICKNESS %.2f M - %d STEPS", params.length, params.thickness,
                  params.steps);
    drawText(sheet, outW - 26 - textWidth(line, 2), 78, line, 2, glm::vec3(0.40f, 0.46f, 0.56f));

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
    std::printf("wrote %s (%dx%d)  contact band %ld, darkened %.0f%%  false positives %.2f%%\n", outPath.c_str(), outW,
                outH, contactPixels, bandDarkened, falsePositive);
    return 0;
}
