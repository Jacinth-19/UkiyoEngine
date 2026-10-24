// Offline terrain preview: a software rasteriser that renders the Terrain Lab tile with the engine's own heightmap,
// layer weights, grass scatter and grass mesh, and the vendored textures, so a still can be made on a machine with no
// GPU (this sandbox has no GL or Vulkan at all).
//
// It is a preview tool, not the engine. The engine draws the same scene through shaders/terrain.frag and
// shaders/grass.{vert,frag}; the two files are kept in step by hand, and the numbers here are copied from the scene
// that src/engine/project_template.cpp writes for the Terrain Lab template:
//     heightScale 14, four layers (grass moss, rock, pale rock, heather), detail 1.7 m at 0.5,
//     grass 4 tufts per square metre, 0.44 m tall, eight blades of four segments, bands 26 / 55 / 110 m,
//     fog 0.0035 per metre with sun in-scatter 0.6, Custom Anime tonemap at exposure 0.84.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party tools/terrain_preview.cpp src/engine/terrain.cpp
//         src/engine/terrain_layers.cpp src/engine/grass.cpp -o build/terrain_preview
// Run (camera, sun, fog, exposure and the grass shape can be overridden with UKIYO_*; see docs/TERRAIN.md):
//     ./build/terrain_preview docs/terrain-hero.png
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/grass.h"
#include "engine/terrain.h"
#include "engine/terrain_layers.h"

using namespace ukiyo;

namespace {

// ---------------------------------------------------------------- textures

struct Texture {
    int w = 0, h = 0, levels = 1;
    std::vector<std::vector<glm::vec4>> px;   // one level after another, RGBA floats

    glm::vec4 texel(const std::vector<glm::vec4>& lv, int lw, int lh, int x, int y) const {
        x = ((x % lw) + lw) % lw;
        y = ((y % lh) + lh) % lh;
        return lv[size_t(y) * lw + x];
    }

    // Bilinear inside one level, wrapped.
    glm::vec4 sampleLevel(int level, glm::vec2 uv) const {
        const int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
        const float fx = uv.x * lw - 0.5f, fy = uv.y * lh - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        const auto& lv = px[level];
        const glm::vec4 a = texel(lv, lw, lh, x0, y0), b = texel(lv, lw, lh, x0 + 1, y0);
        const glm::vec4 c = texel(lv, lw, lh, x0, y0 + 1), d = texel(lv, lw, lh, x0 + 1, y0 + 1);
        return glm::mix(glm::mix(a, b, tx), glm::mix(c, d, tx), ty);
    }

    // Trilinear. `lod` is in texels per screen pixel; 1 keeps level 0, 2 halves it, and so on.
    glm::vec4 sample(glm::vec2 uv, float lod) const {
        lod = std::max(0.0f, lod);
        const int l0 = std::min(int(lod), levels - 1);
        const int l1 = std::min(l0 + 1, levels - 1);
        const float f = std::min(1.0f, lod - float(l0));
        const glm::vec4 a = sampleLevel(l0, uv), b = sampleLevel(l1, uv);
        return glm::mix(a, b, f);
    }
};

bool loadTexture(const std::string& path, bool srgbToLinear, Texture* out) {
    int w = 0, h = 0, c = 0;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!p) {
        std::fprintf(stderr, "could not read %s\n", path.c_str());
        return false;
    }
    out->w = w;
    out->h = h;
    out->px.clear();
    std::vector<glm::vec4> base(size_t(w) * h);
    for (size_t i = 0; i < base.size(); ++i) {
        glm::vec4 v(p[i * 4] / 255.0f, p[i * 4 + 1] / 255.0f, p[i * 4 + 2] / 255.0f, p[i * 4 + 3] / 255.0f);
        if (srgbToLinear) {
            for (int k = 0; k < 3; ++k) {
                const float s = v[k];
                v[k] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
            }
        }
        base[i] = v;
    }
    stbi_image_free(p);
    out->px.push_back(std::move(base));
    // Box-filtered mips, so a distant field averages instead of shimmering.
    int lw = w, lh = h;
    while (lw > 1 || lh > 1) {
        const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        const auto& src = out->px.back();
        std::vector<glm::vec4> dst(size_t(nw) * nh);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                glm::vec4 sum(0.0f);
                int n = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(lw - 1, x * 2 + dx), sy = std::min(lh - 1, y * 2 + dy);
                        sum += src[size_t(sy) * lw + sx];
                        ++n;
                    }
                dst[size_t(y) * nw + x] = sum / float(n);
            }
        }
        out->px.push_back(std::move(dst));
        lw = nw;
        lh = nh;
    }
    out->levels = int(out->px.size());
    return true;
}

// ---------------------------------------------------------------- scene

struct Layer {
    TerrainLayer settings;
    Texture albedo;     // linear
    Texture normal;
    Texture ao;
    bool textured = false;
};

struct Camera {
    glm::vec3 eye{0.0f}, fwd{0.0f, 0.0f, 1.0f}, right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f};
    float focus = 1.0f;
};

constexpr float kSunIntensity = 1.25f;
float kFogDensity = 0.0035f;
constexpr float kFogSun = 0.6f;
float kExposure = 0.84f;
float kSaturation = 1.18f;
float kContrast = 1.12f;
constexpr float kVignette = 0.12f;

glm::vec3 kSunDir = glm::normalize(glm::vec3(0.46f, 0.38f, 0.80f));

// Environment overrides, so a shot can be art-directed without a rebuild: UKIYO_SUN_ELEV / _AZ (degrees),
// UKIYO_FOG, UKIYO_EXPOSURE, UKIYO_EYE="x,z", UKIYO_EYE_H, UKIYO_TARGET="x,z", UKIYO_TARGET_H.
int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : fallback;
}

float envFloat(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v ? float(std::atof(v)) : fallback;
}
glm::vec2 envVec2(const char* name, glm::vec2 fallback) {
    const char* v = std::getenv(name);
    if (!v) return fallback;
    float x = 0, z = 0;
    if (std::sscanf(v, "%f,%f", &x, &z) != 2) return fallback;
    return glm::vec2(x, z);
}
const glm::vec3 kSunColor = glm::vec3(1.0f, 0.95f, 0.88f) * kSunIntensity;
const glm::vec3 kSkyColor = glm::vec3(0.62f, 0.72f, 0.92f) * 0.9f;
const glm::vec3 kGroundColor = glm::vec3(0.50f, 0.45f, 0.48f);
const glm::vec3 kFogColor = glm::vec3(0.74f, 0.83f, 0.95f);

// The grass colour block the engine pushes for the Terrain Lab scene.
const glm::vec3 kGrassBase(0.095f, 0.165f, 0.052f);
const glm::vec3 kGrassTip(0.46f, 0.60f, 0.235f);
constexpr float kGrassTranslucency = 0.5f;
constexpr float kGrassTipDarkening = 0.35f;

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Height band times slope band, exactly as layerWeightsAtSample does it.
float band(float v, float lo, float hi, float fade) {
    const float a = fade > 1e-5f ? smoothstep(lo - fade, lo, v) : (v >= lo ? 1.0f : 0.0f);
    const float b = fade > 1e-5f ? 1.0f - smoothstep(hi, hi + fade, v) : (v <= hi ? 1.0f : 0.0f);
    return a * b;
}

std::array<float, 8> weightsAt(const Heightmap& map, const std::vector<Layer>& layers, const glm::vec3& p) {
    std::array<float, 8> w{};
    const float h = p.y / map.heightScale;
    const float e = 0.35f;
    const float hl = map.heightAtMetres(p.x - e, p.z), hr = map.heightAtMetres(p.x + e, p.z);
    const float hd = map.heightAtMetres(p.x, p.z - e), hu = map.heightAtMetres(p.x, p.z + e);
    const glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * e, hd - hu));
    const float slope = glm::degrees(std::acos(std::clamp(n.y, -1.0f, 1.0f)));
    float total = 0.0f;
    for (size_t k = 0; k < layers.size() && k < 8; ++k) {
        const TerrainLayer& L = layers[k].settings;
        const float v = band(h, L.heightMin, L.heightMax, L.heightFade) *
                        band(slope, L.slopeMin, L.slopeMax, L.slopeFade);
        w[k] = v;
        total += v;
    }
    if (total > 1e-6f) {
        for (float& v : w) v /= total;
    } else {
        w[0] = 1.0f;
    }
    return w;
}

// ---------------------------------------------------------------- raster

struct Framebuffer {
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

struct Vertex {
    glm::vec3 view;    // camera space, z forward, metres
    glm::vec3 world;
    glm::vec3 normal;
    float t = 0.0f;    // 0 at the root of a blade, 1 at the tip
    float hash = 0.0f;
};

struct Persp {
    float w0, w1, w2;   // perspective-correct barycentrics
    float z;            // view depth
};

template <typename Shade>
void rasterTri(Framebuffer& fb, const Camera& cam, const Vertex& a, const Vertex& b, const Vertex& c, Shade shade,
               bool depthTest, bool depthWrite) {
    auto screen = [&](const glm::vec3& v, glm::vec2& out) {
        const float iz = 1.0f / std::max(v.z, 1e-4f);
        out = glm::vec2(v.x * iz * cam.focus + fb.w * 0.5f, fb.h * 0.5f - v.y * iz * cam.focus);
    };
    glm::vec2 p0, p1, p2;
    screen(a.view, p0);
    screen(b.view, p1);
    screen(c.view, p2);
    const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::fabs(area) < 1e-9f) return;
    const float inv = 1.0f / area;
    const int x0 = std::max(0, int(std::floor(std::min({p0.x, p1.x, p2.x}))));
    const int x1 = std::min(fb.w - 1, int(std::ceil(std::max({p0.x, p1.x, p2.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({p0.y, p1.y, p2.y}))));
    const int y1 = std::min(fb.h - 1, int(std::ceil(std::max({p0.y, p1.y, p2.y}))));
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; ++y) {
        const float py = float(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = float(x) + 0.5f;
            float l0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) * inv;
            float l1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) * inv;
            float l2 = 1.0f - l0 - l1;
            if (l0 < -1e-5f || l1 < -1e-5f || l2 < -1e-5f) continue;
            const float iz = l0 / a.view.z + l1 / b.view.z + l2 / c.view.z;
            if (iz <= 0.0f) continue;
            const float z = 1.0f / iz;
            const size_t idx = size_t(y) * fb.w + x;
            if (depthTest && z >= fb.depth[idx]) continue;
            Persp q{l0 / a.view.z / iz, l1 / b.view.z / iz, l2 / c.view.z / iz, z};
            if (depthWrite) fb.depth[idx] = z;
            fb.color[idx] = shade(q, idx);
        }
    }
}

// ---------------------------------------------------------------- shadows

struct ShadowMap {
    int size = 2048;
    std::vector<float> depth;
    glm::mat4 matrix{1.0f};   // world -> light clip, [-1, 1] in xy, [0, 1] in z
    glm::vec3 centre{0.0f};
    float radius = 1.0f;

    glm::vec3 lightSpace(const glm::vec3& p) const {
        const glm::vec4 c = matrix * glm::vec4(p, 1.0f);
        return glm::vec3(c) / c.w;
    }
    bool occluded(const glm::vec3& p, const glm::vec3& n) const;
};

bool ShadowMap::occluded(const glm::vec3& p, const glm::vec3& n) const {
    const float ndl = std::clamp(glm::dot(n, kSunDir), 0.0f, 1.0f);
    // The same slope-scaled receiver offset as shaders/shadow_sample.glsl: a height field lit at a grazing angle
    // shadows itself in stripes unless the sample is pushed along the light by a few texels.
    const float texelWorld = 2.0f * radius / float(size);
    const glm::vec3 q = p + n * (0.02f * (1.0f - ndl)) +
                        kSunDir * (texelWorld * 2.0f / std::max(ndl, 0.25f) + texelWorld * 0.5f);
    const glm::vec3 ls = lightSpace(q);
    const glm::vec2 uv = glm::vec2(ls.x, ls.y) * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return false;
    const float z = ls.z * 0.5f + 0.5f - 0.0008f;
    const int cx = int(uv.x * size), cy = int(uv.y * size);
    int lit = 0, total = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int sx = std::clamp(cx + dx, 0, size - 1), sy = std::clamp(cy + dy, 0, size - 1);
            lit += depth[size_t(sy) * size + sx] >= z ? 1 : 0;
            ++total;
        }
    return lit > 0 && lit < total;
}

long long g_shadowed = 0, g_shaded = 0;
float shadowFactorAt(const ShadowMap& sm, const glm::vec3& p, const glm::vec3& n) {
    const bool occ = sm.occluded(p, n);
    ++g_shaded;
    g_shadowed += occ ? 1 : 0;
    return occ ? 0.0f : 1.0f;
}

// ---------------------------------------------------------------- sky and shading

float hash21(glm::vec2 p) {
    p = glm::fract(p * glm::vec2(123.34f, 456.21f));
    p += glm::vec2(glm::dot(p, p + 45.32f));
    return glm::fract(p.x * p.y);
}
float vnoise(glm::vec2 p) {
    const glm::vec2 i = glm::floor(p), f = glm::fract(p);
    const glm::vec2 u = f * f * (3.0f - 2.0f * f);
    const float a = hash21(i), b = hash21(i + glm::vec2(1, 0)), c = hash21(i + glm::vec2(0, 1)),
                d = hash21(i + glm::vec2(1, 1));
    return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
}
float fbm(glm::vec2 p) {
    float v = 0.0f, a = 0.5f;
    for (int i = 0; i < 5; ++i) {
        v += a * vnoise(p);
        p = p * 2.03f + glm::vec2(17.1f, 9.2f);
        a *= 0.5f;
    }
    return v;
}

// shaders/sky.frag, ported.
glm::vec3 skyColor(const glm::vec3& rd, float time) {
    const float y = rd.y;
    const glm::vec3 zenith(0.20f, 0.46f, 0.92f);
    const glm::vec3 horizon(0.80f, 0.91f, 1.00f);
    const glm::vec3 S = kSunDir;
    const float t = std::pow(std::clamp(y, 0.0f, 1.0f), 0.45f);
    const float sunAmt = std::pow(std::max(glm::dot(rd, S), 0.0f), 3.0f);
    glm::vec3 col = glm::mix(horizon, zenith, t);
    col = glm::mix(col, glm::vec3(1.0f, 0.86f, 0.72f), sunAmt * 0.35f * (1.0f - t));
    const float sd = std::max(glm::dot(rd, S), 0.0f);
    const float disc = smoothstep(0.9992f, 0.9997f, sd);
    col += glm::vec3(1.0f, 0.97f, 0.88f) * disc * 6.0f;
    col += glm::vec3(1.0f, 0.82f, 0.58f) * (std::pow(sd, 28.0f) * 0.8f + std::pow(sd, 5.0f) * 0.20f);
    if (y > 0.0f) {
        const glm::vec2 cuv = glm::vec2(rd.x, rd.z) / (y + 0.14f) * 1.1f + glm::vec2(time * 0.012f, 0.0f);
        const float n = fbm(cuv * 1.3f);
        const float c = smoothstep(0.50f, 0.60f, n);
        const float nl = fbm(cuv * 1.3f + glm::vec2(S.x, S.z) * 0.10f);
        const float lit = n - nl - 0.012f >= 0.0f ? 1.0f : 0.0f;
        const glm::vec3 cloud = glm::mix(glm::vec3(0.72f, 0.80f, 0.94f), glm::vec3(1.00f, 0.985f, 0.96f) * 1.15f, lit);
        col = glm::mix(col, cloud, c * smoothstep(0.02f, 0.30f, y));
    }
    return col;
}

struct Shader {
    const Heightmap& map;
    const std::vector<Layer>& layers;
    const Texture& detail;
    const ShadowMap& shadow;
    const Camera& cam;
    float detailMetres = 1.7f;
    float detailStrength = 0.35f;
    float detailMacro = 0.28f;
    float pixelScale = 0.001f;   // view-space metres per pixel at one metre of depth

    glm::vec3 terrain(glm::vec3 p, float viewZ) const {
        const std::array<float, 8> w = weightsAt(map, layers, p);
        glm::vec3 albedo(0.0f);
        float rough = 0.0f, ao = 0.0f;
        // Geometric normal from the height field.
        const float e = 0.35f;
        const float hl = map.heightAtMetres(p.x - e, p.z), hr = map.heightAtMetres(p.x + e, p.z);
        const float hd = map.heightAtMetres(p.x, p.z - e), hu = map.heightAtMetres(p.x, p.z + e);
        const glm::vec3 n0 = glm::normalize(glm::vec3(hl - hr, 2.0f * e, hd - hu));
        const glm::vec3 T = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f) - n0 * n0.x);
        const glm::vec3 B = glm::cross(n0, T);
        glm::vec3 nSum(0.0f);
        // Metres per pixel across the ground, and the texel footprint that gives per layer. This is the engine's
        // implicit derivative, made explicit because the preview rasteriser has no dFdx.
        const float mpp = viewZ * pixelScale / std::max(0.2f, n0.y);
        for (size_t k = 0; k < layers.size() && k < 8; ++k) {
            const float weight = w[k];
            if (weight < 0.001f) continue;
            const Layer& L = layers[k];
            const float tile = std::max(0.5f, L.settings.tileMetres);
            const glm::vec2 uv = glm::vec2(p.x, p.z) / tile;
            const float lod = std::log2(std::max(1.0f, mpp * 512.0f / tile));
            if (L.textured) {
                const glm::vec4 d = L.albedo.sample(uv, lod);
                albedo += weight * L.settings.colour * glm::vec3(d);
                rough += weight * L.settings.roughness;
                ao += weight * L.ao.sample(uv, lod).r;
                const glm::vec3 nTs = glm::vec3(L.normal.sample(uv, lod)) * 2.0f - 1.0f;
                nSum += weight * glm::normalize(T * nTs.x + B * nTs.y + n0 * nTs.z);
            } else {
                albedo += weight * L.settings.colour;
                rough += weight * L.settings.roughness;
                ao += weight;
                nSum += weight * n0;
            }
        }
        // Detail overlay, mirrored from shaders/terrain.frag.
        const float detailLod = std::log2(std::max(1.0f, mpp * 512.0f / detailMetres));
        const glm::vec3 det = glm::vec3(detail.sample(glm::vec2(p.x, p.z) / detailMetres, detailLod));
        const float detLum = glm::dot(det, glm::vec3(0.299f, 0.587f, 0.114f));
        const float viewDist = glm::distance(cam.eye, p);
        const float close = 1.0f - smoothstep(18.0f, 45.0f, viewDist);
        albedo *= glm::mix(glm::vec3(1.0f), glm::vec3(0.55f + 0.9f * detLum), close * detailStrength);
        const float macroA = detail.sample(glm::vec2(p.x, p.z) * 0.013f + glm::vec2(0.31f, 0.77f), 0.0f).g;
        const float macroB = detail.sample(glm::vec2(p.x, p.z) * 0.047f - glm::vec2(0.11f, 0.43f), 0.0f).r;
        albedo *= 1.0f + (macroA * 0.6f + macroB * 0.4f - 0.5f) * detailMacro;

        const glm::vec3 n = glm::normalize(nSum);
        const float ndl = std::max(glm::dot(n, kSunDir), 0.0f);
        const float edge = glm::mix(0.03f, 0.25f, std::clamp(rough, 0.0f, 1.0f));
        const float ramp = smoothstep(0.15f - edge, 0.15f + edge, ndl);
        glm::vec3 hemi = glm::mix(kGroundColor, kSkyColor, 0.5f + 0.5f * n.y) * (0.45f + 0.55f * std::clamp(ao, 0.0f, 1.0f));
        const float sun = ramp * shadowFactorAt(shadow, p, n);
        return albedo * (hemi + kSunColor * sun);
    }

    glm::vec3 grass(glm::vec3 p, glm::vec3 n, float t, float h) const {
        const glm::vec3 albedo = glm::mix(kGrassBase, kGrassTip, smoothstep(0.05f, 0.9f, t)) *
                                 glm::mix(1.0f, 1.0f - kGrassTipDarkening, std::pow(1.0f - t, 1.5f)) *
                                 (0.88f + 0.24f * h);
        const float ndl = std::max(glm::dot(n, kSunDir), 0.0f);
        const float ramp = smoothstep(0.08f, 0.52f, ndl);
        const glm::vec3 hemi = glm::mix(kGroundColor, kSkyColor, 0.5f + 0.5f * n.y);
        const float lit = shadowFactorAt(shadow, p, n);
        const glm::vec3 V = glm::normalize(cam.eye - p);
        const float back = std::pow(std::max(glm::dot(-V, kSunDir), 0.0f), 2.0f);
        const float through = kGrassTranslucency * back * t * t;
        return albedo * (hemi + kSunColor * (ramp * lit)) + kGrassTip * kSunColor * through * 0.4f * lit;
    }
};

// ---------------------------------------------------------------- post

glm::vec3 srgb(glm::vec3 c) {
    c = glm::clamp(c, 0.0f, 1.0f);
    return glm::vec3(c.r <= 0.0031308f ? c.r * 12.92f : 1.055f * std::pow(c.r, 1.0f / 2.4f) - 0.055f,
                     c.g <= 0.0031308f ? c.g * 12.92f : 1.055f * std::pow(c.g, 1.0f / 2.4f) - 0.055f,
                     c.b <= 0.0031308f ? c.b * 12.92f : 1.055f * std::pow(c.b, 1.0f / 2.4f) - 0.055f);
}

// Custom Anime tonemap, shaders/tonemap.frag: a hue-preserving shoulder and a small toe.
glm::vec3 animeCurve(glm::vec3 c, float knee, float toe) {
    const float m = std::max(c.r, std::max(c.g, c.b));
    if (m <= 0.0f) return c;
    float out = m <= knee ? m : knee + (1.0f - knee) * (1.0f - std::exp(-(m - knee) / (1.0f - knee)));
    out += toe * (1.0f - smoothstep(0.0f, 0.06f, out));
    return c * (out / m);
}

struct Rgb8 { unsigned char r, g, b; };

} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "terrain-hero.png";
    const std::string root = argc > 2 ? argv[2] : ".";
    const int W = argc > 3 ? std::atoi(argv[3]) : 1600;
    const int H = argc > 4 ? std::atoi(argv[4]) : 900;
    const int SS = argc > 5 ? std::atoi(argv[5]) : 2;
    const float time = 12.7f;
    {
        const float az = glm::radians(envFloat("UKIYO_SUN_AZ", 60.0f));
        const float el = glm::radians(envFloat("UKIYO_SUN_ELEV", 24.0f));
        kSunDir = glm::normalize(glm::vec3(std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)));
        kFogDensity = envFloat("UKIYO_FOG", kFogDensity);
        kExposure = envFloat("UKIYO_EXPOSURE", kExposure);
    }

    Heightmap map;
    std::string err;
    map.cellSize = 1.0f;
    map.heightScale = 14.0f;
    if (!loadHeightmapU16(root + "/tests/data/terrain_hills_257.u16", 257, &map, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // The Terrain Lab layers, with the vendored textures.
    std::vector<Layer> layers;
    layers.reserve(4);
    auto addLayer = [&](const TerrainLayer& s, const char* stem, bool textured) {
        Layer L;
        L.settings = s;
        if (textured) {
            L.textured = loadTexture(root + "/editor/res/terrain/" + stem + "_albedo.png", false, &L.albedo) &&
                         loadTexture(root + "/editor/res/terrain/" + stem + "_normal.png", false, &L.normal) &&
                         loadTexture(root + "/editor/res/terrain/" + stem + "_ao.png", false, &L.ao);
            if (!L.textured) std::fprintf(stderr, "note: %s has no textures, using the flat colour\n", stem);
        }
        layers.push_back(std::move(L));
    };
    std::vector<TerrainLayer> defaults = defaultTerrainLayers();
    TerrainLayer heather;
    heather.name = "heather";
    heather.colour = glm::vec3(1.0f);
    heather.roughness = 0.85f;
    heather.heightMin = 0.38f;
    heather.heightMax = 0.82f;
    heather.heightFade = 0.12f;
    heather.slopeMax = 40.0f;
    heather.slopeFade = 8.0f;
    heather.tileMetres = 5.5f;
    defaults.push_back(heather);
    const char* stems[4] = {"grass_moss", "muddy", nullptr, "heather"};
    const float tiles[4] = {4.0f, 3.5f, 8.0f, 5.5f};
    defaults[2].colour = glm::vec3(0.46f, 0.44f, 0.40f);   // exposed rock, not a snow cap
    defaults[2].heightMin = 0.86f;
    defaults[2].heightFade = 0.05f;
    for (int k = 0; k < 4; ++k) {
        TerrainLayer s = defaults[size_t(k)];
        s.tileMetres = tiles[k];
        if (stems[k]) s.colour = glm::vec3(1.0f);
        addLayer(s, stems[k] ? stems[k] : "", stems[k] != nullptr);
    }

    Texture detail;
    if (!loadTexture(root + "/editor/res/terrain/plant_detail.png", false, &detail)) return 1;

    // Grass: the template's settings, scattered over the same tile with the same layer masks.
    GrassSettings grass;
    grass.enabled = true;
    grass.density = envFloat("UKIYO_GRASS_DENSITY", 4.0f);   // the template's meadow; see docs/GRASS.md
    grass.height = envFloat("UKIYO_GRASS_HEIGHT", 0.44f);
    grass.width = envFloat("UKIYO_GRASS_WIDTH", 0.018f);
    grass.blades = envInt("UKIYO_GRASS_BLADES", 8);
    grass.segments = 4;
    grass.scaleMin = 0.8f;
    grass.scaleMax = 1.4f;
    grass.bandNear = 26.0f;
    grass.bandMid = 55.0f;
    grass.cullDistance = 110.0f;
    grass.includeLayers = (1u << 0) | (1u << 3);
    grass.includeThreshold = 0.25f;
    grass.windStrength = 0.24f;
    grass.windFrequency = 0.42f;
    grass.windScale = 0.10f;
    grass.flutter = 0.05f;
    std::vector<TerrainLayer> layerSettings;
    for (const Layer& L : layers) layerSettings.push_back(L.settings);
    const std::vector<GrassInstance> tufts = scatterGrass(map, layerSettings, {}, grass, {});
    const GrassMesh mesh = buildGrassMesh(grass);
    std::printf("tufts %zu, tuft mesh %zu vertices %zu triangles\n", tufts.size(), mesh.positions.size(),
                mesh.indices.size() / 3);

    // Camera: on the shoulder of the south ridge, looking north-east across the basin.
    Camera cam;
    const glm::vec2 eyeXZ = envVec2("UKIYO_EYE", glm::vec2(46.0f, 206.0f));
    cam.eye = glm::vec3(eyeXZ.x, map.heightAtMetres(eyeXZ.x, eyeXZ.y) + envFloat("UKIYO_EYE_H", 2.6f), eyeXZ.y);
    const glm::vec2 targetXZ = envVec2("UKIYO_TARGET", glm::vec2(150.0f, 118.0f));
    const float targetY = map.heightAtMetres(targetXZ.x, targetXZ.y) + envFloat("UKIYO_TARGET_H", 2.0f);
    cam.fwd = glm::normalize(glm::vec3(targetXZ.x, targetY, targetXZ.y) - cam.eye);
    cam.right = glm::normalize(glm::cross(cam.fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    cam.up = glm::cross(cam.right, cam.fwd);
    cam.focus = 0.5f * float(H) * SS / std::tan(glm::radians(55.0f) * 0.5f);

    // Shadow map: an orthographic fit around the ground the camera can see.
    ShadowMap sm;
    sm.size = 2048;
    sm.depth.assign(size_t(sm.size) * sm.size, 1e30f);
    sm.centre = glm::vec3(128.0f, 8.0f, 128.0f);
    sm.radius = 150.0f;
    {
        const glm::vec3 up = std::fabs(kSunDir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::mat4 view = glm::lookAt(sm.centre + kSunDir * 200.0f, sm.centre, up);
        const glm::mat4 proj = glm::ortho(-sm.radius, sm.radius, -sm.radius, sm.radius, 1.0f, 400.0f);
        sm.matrix = proj * view;
    }
    // Depth from the sun: the height field only, as the engine's terrain shadow pass does.
    Framebuffer sfb;
    sfb.w = sm.size;
    sfb.h = sm.size;
    sfb.depth = sm.depth;
    Camera lightCam;
    lightCam.eye = sm.centre + kSunDir * 200.0f;
    lightCam.fwd = -kSunDir;
    lightCam.right = glm::normalize(glm::cross(lightCam.fwd, glm::vec3(0, 1, 0)));
    lightCam.up = glm::cross(lightCam.right, lightCam.fwd);
    // The shadow raster is in light-clip space, so use a camera whose screen equals light clip xy.
    Framebuffer lfb;
    lfb.w = sm.size;
    lfb.h = sm.size;
    lfb.depth.assign(size_t(sm.size) * sm.size, 1e30f);
    auto rasterShadow = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        glm::vec3 pa = sm.lightSpace(a), pb = sm.lightSpace(b), pc = sm.lightSpace(c);
        const glm::vec2 p0(pa.x * 0.5f + 0.5f, pa.y * 0.5f + 0.5f), p1(pb.x * 0.5f + 0.5f, pb.y * 0.5f + 0.5f),
            p2(pc.x * 0.5f + 0.5f, pc.y * 0.5f + 0.5f);
        const float z0 = pa.z * 0.5f + 0.5f, z1 = pb.z * 0.5f + 0.5f, z2 = pc.z * 0.5f + 0.5f;
        const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (std::fabs(area) < 1e-12f) return;
        const float inv = 1.0f / area;
        const int x0 = std::max(0, int(std::floor(std::min({p0.x, p1.x, p2.x}) * sm.size)));
        const int x1 = std::min(sm.size - 1, int(std::ceil(std::max({p0.x, p1.x, p2.x}) * sm.size)));
        const int y0 = std::max(0, int(std::floor(std::min({p0.y, p1.y, p2.y}) * sm.size)));
        const int y1 = std::min(sm.size - 1, int(std::ceil(std::max({p0.y, p1.y, p2.y}) * sm.size)));
        for (int y = y0; y <= y1; ++y) {
            const float py = (float(y) + 0.5f) / sm.size;
            for (int x = x0; x <= x1; ++x) {
                const float px = (float(x) + 0.5f) / sm.size;
                const float l0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) * inv;
                const float l1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) * inv;
                const float l2 = 1.0f - l0 - l1;
                if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) continue;
                const float z = l0 * z0 + l1 * z1 + l2 * z2;
                float& d = lfb.depth[size_t(y) * sm.size + x];
                d = std::min(d, z);
            }
        }
    };
    for (int j = 0; j < 256; ++j)
        for (int i = 0; i < 256; ++i) {
            auto at = [&](int x, int z) {
                return glm::vec3(float(x), map.heightAtMetres(float(x), float(z)), float(z));
            };
            const glm::vec3 a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            rasterShadow(a, b, c);
            rasterShadow(a, c, d);
        }
    sm.depth = std::move(lfb.depth);

    Framebuffer fb;
    fb.reset(W * SS, H * SS);
    // Sky first; geometry overwrites it where it covers.
    const float tanHalf = std::tan(glm::radians(55.0f) * 0.5f);
    for (int y = 0; y < fb.h; ++y) {
        const float sy = (1.0f - 2.0f * (float(y) + 0.5f) / float(fb.h)) * tanHalf;
        for (int x = 0; x < fb.w; ++x) {
            const float sx = (2.0f * (float(x) + 0.5f) / float(fb.w) - 1.0f) * tanHalf * float(fb.w) / float(fb.h);
            const glm::vec3 rd = glm::normalize(cam.fwd + cam.right * sx + cam.up * sy);
            fb.color[size_t(y) * fb.w + x] = skyColor(rd, time);
        }
    }

    Shader shader{map, layers, detail, sm, cam};
    shader.pixelScale = 2.0f * tanHalf / float(fb.h);

    // Terrain.
    int terrainTris = 0;
    for (int j = 0; j < 256; ++j)
        for (int i = 0; i < 256; ++i) {
            auto world = [&](int x, int z) {
                return glm::vec3(float(x), map.heightAtMetres(float(x), float(z)), float(z));
            };
            Vertex v[4];
            const glm::vec3 p[4] = {world(i, j), world(i + 1, j), world(i + 1, j + 1), world(i, j + 1)};
            for (int k = 0; k < 4; ++k) {
                v[k].world = p[k];
                v[k].view = glm::vec3(glm::dot(p[k] - cam.eye, cam.right), glm::dot(p[k] - cam.eye, cam.up),
                                      glm::dot(p[k] - cam.eye, cam.fwd));
            }
            auto shade = [&](Persp q, size_t) {
                const glm::vec3 wp = v[0].world * q.w0 + v[1].world * q.w1 + v[2].world * q.w2;
                return shader.terrain(wp, q.z);
            };
            rasterTri(fb, cam, v[0], v[1], v[2], shade, true, true);
            auto shade2 = [&](Persp q, size_t) {
                const glm::vec3 wp = v[0].world * q.w0 + v[2].world * q.w1 + v[3].world * q.w2;
                return shader.terrain(wp, q.z);
            };
            rasterTri(fb, cam, v[0], v[2], v[3], shade2, true, true);
            terrainTris += 2;
        }
    std::printf("terrain %d triangles, shadowed %.1f%%\n", terrainTris,
                g_shaded ? 100.0 * double(g_shadowed) / double(g_shaded) : 0.0);

    // Grass: instances whose distance the shader would keep, with the same wind bend.
    const glm::vec2 dir = glm::normalize(grass.windDir + glm::vec2(1e-4f, 0.0f));
    const glm::vec2 across(-dir.y, dir.x);
    int kept = 0;
    for (const GrassInstance& inst : tufts) {
        const glm::vec3 root = inst.pos;
        const float dist = glm::distance(cam.eye, root);
        const float keep = grassBladeHash(glm::vec2(root.x, root.z));
        if (keep > grassDensityAt(dist, grass)) continue;
        ++kept;
        // Instance basis, as terrain_gpu.cpp packs it: yaw about the surface normal, scaled.
        const glm::vec3 up = inst.up;
        glm::vec3 side = glm::normalize(glm::cross(up, glm::vec3(0.0f, 0.0f, 1.0f)));
        const float c = std::cos(inst.yaw), s = std::sin(inst.yaw);
        const glm::vec3 x = side * c + glm::cross(up, side) * s;
        const glm::vec3 z = glm::cross(x, up) * inst.scale;
        const glm::vec3 xx = x * inst.scale, yy = up * inst.scale;
        const float phase = time * grass.windFrequency * 6.2831853f;
        const float w1 = std::sin(glm::dot(glm::vec2(root.x, root.z), dir) * grass.windScale + phase);
        const float w2 = std::sin(glm::dot(glm::vec2(root.x, root.z), across) * grass.windScale * 1.7f - phase * 0.62f +
                                  keep * 6.2831853f);
        const float gust = glm::mix(0.30f, 1.0f, (w1 * 0.5f + 0.5f) * (w1 * 0.5f + 0.5f));
        const glm::vec2 sway = dir * (w1 * grass.windStrength * gust) + across * (w2 * (grass.windStrength * 0.45f + grass.flutter));
        const float widen = 1.0f + std::max(0.0f, dist - 8.0f) * 0.0035f;
        for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3) {
            Vertex v[3];
            for (int k = 0; k < 3; ++k) {
                const uint32_t index = mesh.indices[tri + k];
                const glm::vec3 local = mesh.positions[index];
                glm::vec3 off = xx * local.x + yy * local.y + z * local.z;
                const float t = mesh.uvs[index].y;
                const glm::vec3 bent = off + glm::vec3(sway.x, 0.0f, sway.y) * smoothstep(0.15f, 1.0f, t);
                const float len = glm::length(off);
                off = len > 1e-4f ? glm::normalize(bent) * len : bent;
                glm::vec3 wp = root + off;
                wp.x = root.x + (wp.x - root.x) * widen;
                wp.z = root.z + (wp.z - root.z) * widen;
                v[k].world = wp;
                v[k].normal = glm::normalize(xx * mesh.normals[index].x + yy * mesh.normals[index].y +
                                            z * mesh.normals[index].z);
                v[k].t = t;
                v[k].hash = keep;
                v[k].view = glm::vec3(glm::dot(wp - cam.eye, cam.right), glm::dot(wp - cam.eye, cam.up),
                                      glm::dot(wp - cam.eye, cam.fwd));
            }
            auto shade = [&](Persp q, size_t) {
                const glm::vec3 wp = v[0].world * q.w0 + v[1].world * q.w1 + v[2].world * q.w2;
                const glm::vec3 n = glm::normalize(v[0].normal * q.w0 + v[1].normal * q.w1 + v[2].normal * q.w2);
                return shader.grass(wp, n, v[0].t * q.w0 + v[1].t * q.w1 + v[2].t * q.w2, v[0].hash);
            };
            rasterTri(fb, cam, v[0], v[1], v[2], shade, true, true);
        }
    }
    std::printf("grass tufts drawn %d\n", kept);

    // Resolve: box downsample, fog, tonemap, grade, encode.
    std::vector<Rgb8> image(size_t(W) * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            glm::vec3 c(0.0f);
            float z = 0.0f;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    const size_t i = size_t(y * SS + sy) * fb.w + (x * SS + sx);
                    c += fb.color[i];
                    z += fb.depth[i] < 1e29f ? fb.depth[i] : 0.0f;
                }
            c /= float(SS * SS);
            z /= float(SS * SS);
            if (z > 0.0f) {
                // Aerial perspective, effects.frag.
                const float sy = (1.0f - 2.0f * (float(y) + 0.5f) / float(H)) * tanHalf;
                const float sx = (2.0f * (float(x) + 0.5f) / float(W) - 1.0f) * tanHalf * float(W) / float(H);
                const glm::vec3 rd = glm::normalize(cam.fwd + cam.right * sx + cam.up * sy);
                const float fogAmt = 1.0f - std::exp(-z * kFogDensity);
                const float phase = std::pow(std::max(glm::dot(rd, kSunDir), 0.0f), 8.0f);
                const glm::vec3 fogCol = kFogColor + glm::vec3(1.0f, 0.95f, 0.85f) * phase * kFogSun;
                c = glm::mix(c, fogCol, fogAmt);
            }
            c *= kExposure;
            c = animeCurve(glm::max(c, glm::vec3(0.0f)), 0.45f, 0.03f);
            c = srgb(c);
            const glm::vec3 lumW(0.2126f, 0.7152f, 0.0722f);
            const float l = glm::dot(c, lumW);
            const float mx = std::max(c.r, std::max(c.g, c.b)), mn = std::min(c.r, std::min(c.g, c.b));
            const float chroma = mx > 1e-4f ? (mx - mn) / mx : 0.0f;
            c = glm::mix(glm::vec3(l), c, kSaturation * (1.0f + 0.35f * (1.0f - chroma)));
            c = (c - 0.5f) * kContrast + 0.5f;
            c += (1.0f - smoothstep(0.0f, 0.45f, l)) * glm::vec3(-0.012f, -0.004f, 0.022f);
            c += smoothstep(0.55f, 1.0f, l) * glm::vec3(0.010f, 0.004f, -0.012f);
            const glm::vec2 q(float(x) / float(W) - 0.5f, float(y) / float(H) - 0.5f);
            c *= std::clamp(1.0f - kVignette * glm::dot(q, q) * 2.2f, 0.0f, 1.0f);
            c = glm::clamp(c, 0.0f, 1.0f);
            image[size_t(y) * W + x] = Rgb8{static_cast<unsigned char>(std::lround(c.r * 255.0f)),
                                            static_cast<unsigned char>(std::lround(c.g * 255.0f)),
                                            static_cast<unsigned char>(std::lround(c.b * 255.0f))};
        }
    }
    if (!stbi_write_png(out.c_str(), W, H, 3, image.data(), W * 3)) {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d x %d)\n", out.c_str(), W, H);
    return 0;
}
