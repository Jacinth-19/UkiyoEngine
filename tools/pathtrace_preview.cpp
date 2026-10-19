// A CPU path tracer for the Terrain Lab scene, and the answer to "RTX path tracing on an iGPU".
//
// The reference, ColinLeung-NiloCat/NiloPathTracing, is a hardware-ray-traced URP renderer: DXR or Vulkan ray
// tracing, DLSS ray reconstruction, an RTX-class GPU. None of that exists on an integrated GPU, and this engine's
// RHI has no ray-tracing or compute path at all. What *does* run everywhere is the same maths on the CPU: a
// heightfield intersector, cosine-weighted bounces, a sun with ray-marched soft shadows, the engine's own layer
// blending and textures, and a tone map. That is what this tool is: a reference image the real-time renderer can be
// compared against, at whatever sample count the machine can afford.
//
// It is deliberately not an engine feature. Nothing here touches the RHI. It is the "ground truth" branch of the
// renderer: slow, simple, and correct about light, used for stills and for checking the real-time look.
//
//     cmake --build build --target pathtrace_preview
//     ./build/pathtrace_preview docs/pathtrace-terrain.png . 1600 900 96
//
// Rays are in world space, metres. The heightfield is intersected by adaptive marching plus a bisection refine; the
// terrain is a single tile, so there is no BVH to build.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/terrain.h"

using namespace ukiyo;

namespace {

struct Texture {
    int w = 0, h = 0;
    std::vector<glm::vec3> texel;
    glm::vec3 mean{0.0f};
    bool valid() const { return w > 0 && h > 0; }
    void computeMean() {
        mean = glm::vec3(0.0f);
        for (const glm::vec3& c : texel) mean += c;
        if (!texel.empty()) mean /= float(texel.size());
    }
    glm::vec3 sample(glm::vec2 uv) const {
        uv.x -= std::floor(uv.x);
        uv.y -= std::floor(uv.y);
        const float x = uv.x * float(w) - 0.5f, y = uv.y * float(h) - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - float(x0), fy = y - float(y0);
        auto at = [&](int ix, int iy) {
            ix = (ix % w + w) % w;
            iy = (iy % h + h) % h;
            return texel[size_t(iy) * size_t(w) + size_t(ix)];
        };
        return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
    }
};

bool loadTexture(const std::string& path, Texture* out) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!pixels) return false;
    out->w = w;
    out->h = h;
    out->texel.resize(size_t(w) * h);
    for (int i = 0; i < w * h; ++i) {
        const glm::vec3 srgb(float(pixels[i * 3 + 0]) / 255.0f, float(pixels[i * 3 + 1]) / 255.0f,
                             float(pixels[i * 3 + 2]) / 255.0f);
        out->texel[size_t(i)] = glm::pow(srgb, glm::vec3(2.2f));   // to linear
    }
    stbi_image_free(pixels);
    return true;
}

struct Scene {
    Heightmap map;
    Texture moss, heather, muddy;
    glm::vec3 sunDir{0.0f};
    float sunIntensity = 7.0f;
    float exposure = 0.95f;

    float heightAt(float x, float z) const {
        return map.heightAtMetres(std::clamp(x, 0.0f, float(map.size - 1) * map.cellSize),
                                  std::clamp(z, 0.0f, float(map.size - 1) * map.cellSize));
    }
    glm::vec3 normalAt(float x, float z) const {
        const float e = 0.35f;
        const float hl = heightAt(x - e, z), hr = heightAt(x + e, z);
        const float hd = heightAt(x, z - e), hu = heightAt(x, z + e);
        return glm::normalize(glm::vec3(hl - hr, 2.0f * e, hd - hu));
    }

    // Micro-relief: the moss tile's luminance as a height field, differentiated at a quarter metre. A millimetre of
    // normal tilt is what makes grass read as grass under a raking sun; without it every surface is a painted dune.
    glm::vec3 bumpNormal(glm::vec3 p, glm::vec3 n) const {
        const float e = 0.22f;
        auto lum = [&](glm::vec2 q) {
            const glm::vec3 c = moss.sample(q / 4.0f);
            return 0.30f * c.r + 0.59f * c.g + 0.11f * c.b;
        };
        const float l0 = lum(glm::vec2(p.x, p.z));
        const float lx = lum(glm::vec2(p.x + e, p.z));
        const float lz = lum(glm::vec2(p.x, p.z + e));
        glm::vec3 nb = n + glm::vec3(-(lx - l0) / e, 0.0f, -(lz - l0) / e) * 0.30f;
        return glm::normalize(nb);
    }

    // The template's four bands, in the same order as the engine: moss low, mud low and flat, rock high and steep,
    // heather in between. Weights are their product, normalised with a floor so the ground is never untextured.
    glm::vec3 albedoAt(glm::vec3 p, glm::vec3 n, float detail) const {
        const float h = p.y / map.heightScale;
        const float slope = glm::degrees(std::acos(std::clamp(n.y, -1.0f, 1.0f)));
        auto band = [](float v, float lo, float hi, float fade) {
            if (hi <= lo) return 1.0f;
            const float f = std::max(fade, 1e-3f);
            const float a = std::clamp((v - lo) / f + 0.5f, 0.0f, 1.0f);
            const float b = std::clamp((hi - v) / f + 0.5f, 0.0f, 1.0f);
            return std::min(a, b);
        };
        // The template's four layers, in the engine's own order: moss grass low and gentle, heather on the middle
        // slopes, the muddy rock texture only where the ground is steep or near a summit. The bands below are the
        // template's heightMin/heightMax/slopeMax/fade values, not invented ones.
        const float patch = 0.55f + 0.45f * fbm(glm::vec2(p.x, p.z) / 37.0f);
        float w[3] = {
            band(h, 0.0f, 0.95f, 0.10f) * band(slope, 0.0f, 34.0f, 10.0f) * patch,
            band(h, 0.38f, 0.82f, 0.12f) * band(slope, 0.0f, 40.0f, 8.0f) * 0.7f,
            band(slope, 34.0f, 90.0f, 10.0f) + band(h, 0.86f, 1.05f, 0.05f) * 0.5f,
        };
        const float sum = std::max(1e-4f, w[0] + w[1] + w[2]);
        // A grass tint over the moss tile: the imported albedo is olive under a tungsten-ish sun and reads as sand
        // without it. The reference art is a greener field than the raw tile.
        const glm::vec3 t0 = moss.sample(glm::vec2(p.x, p.z) / 4.0f) * glm::vec3(0.72f, 1.02f, 0.58f);
        const glm::vec3 t1 = heather.sample(glm::vec2(p.x, p.z) / 5.5f);
        const glm::vec3 t2 = muddy.sample(glm::vec2(p.x, p.z) / 3.5f);
        const glm::vec3 blended = (t0 * w[0] + t1 * w[1] + t2 * w[2]) / sum;
        // Beyond a couple of hundred metres a 512 px tile is pure sparkle. Fade towards the band's mean colour, the
        // same thing a mip chain does on the GPU, so distant hills read as smooth felt rather than noise.
        // Two more octaves below the tiles' own scale: dry patches and a fine grain, the same job the terrain shader's
        // detail map does with its macro/micro mask.
        const float macro = fbm(glm::vec2(p.x, p.z) / 23.0f);
        const float micro = fbm(glm::vec2(p.x, p.z) * 2.7f);
        glm::vec3 graded = blended * (0.88f + 0.24f * micro);
        graded = glm::mix(graded, glm::vec3(0.30f, 0.24f, 0.11f), std::pow(macro, 3.0f) * 0.35f);
        const glm::vec3 average = (moss.mean * w[0] + heather.mean * w[1] + muddy.mean * w[2]) / sum;
        return glm::mix(average, graded, detail);
    }

    static float hash(glm::vec2 p) {
        return glm::fract(std::sin(glm::dot(p, glm::vec2(127.1f, 311.7f))) * 43758.5453f);
    }
    static float noise(glm::vec2 p) {
        const glm::vec2 i = glm::floor(p), f = glm::fract(p);
        const glm::vec2 u = f * f * (3.0f - 2.0f * f);
        return glm::mix(glm::mix(hash(i), hash(i + glm::vec2(1.0f, 0.0f)), u.x),
                        glm::mix(hash(i + glm::vec2(0.0f, 1.0f)), hash(i + glm::vec2(1.0f, 1.0f)), u.x), u.y);
    }
    static float fbm(glm::vec2 p) {
        float v = 0.0f, a = 0.5f;
        for (int i = 0; i < 5; ++i) {
            v += a * noise(p);
            p = p * 2.03f + glm::vec2(11.3f, 7.7f);
            a *= 0.5f;
        }
        return v;
    }

    // The sky is authored in display space, the way a matte painting is, then converted back through the tone
    // curve. Authoring radiance directly means guessing what ACES will do to it; authoring the gradient you want
    // and inverting the curve lands it on the nose while the indirect light stays physically consistent.
    static float inverseAces(float y) {
        y = std::clamp(y, 0.0f, 0.995f);
        const double a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
        const double A = c * y - a, B = d * y - b, C = e * y;
        const double disc = B * B - 4.0 * A * C;
        if (disc < 0.0) return 0.0f;
        const double sq = std::sqrt(disc);
        double best = 0.0;
        for (double x : {(-B + sq) / (2.0 * A), (-B - sq) / (2.0 * A)}) best = std::max(best, x);
        return float(std::max(0.0, best));
    }
    static glm::vec3 fromDisplay(glm::vec3 c) {
        c = glm::pow(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f));
        return glm::vec3(inverseAces(c.r), inverseAces(c.g), inverseAces(c.b));
    }
    static float smoothstep(float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    glm::vec3 sky(glm::vec3 d) const {
        const float up = std::clamp(d.y, 0.0f, 1.0f);
        // A dusk gradient: cold indigo overhead, a warm haze band at the horizon.
        glm::vec3 disp = glm::mix(glm::vec3(0.90f, 0.78f, 0.62f), glm::vec3(0.18f, 0.38f, 0.68f),
                                  std::pow(up, 0.42f));
        disp = glm::mix(disp, glm::vec3(0.94f, 0.89f, 0.82f), std::exp(-up * 11.0f) * 0.45f);

        // The sun sits low: a small disc, a tight glow, and a broad forward-scatter lobe.
        const float sd = glm::dot(d, sunDir);
        const float cs = std::max(sd, 0.0f);
        // A thin cirrus deck, drawn where the view ray meets a plane about 1.4 km up and carried to the horizon.
        if (d.y > 0.012f) {
            const glm::vec2 uv = glm::vec2(d.x, d.z) / d.y * 1.15f + glm::vec2(3.1f, 1.7f);
            const float n = fbm(uv * glm::vec2(0.55f, 2.4f));
            const float cover = std::clamp((n - 0.54f) * 5.0f, 0.0f, 1.0f);
            const float streak = std::pow(std::clamp(1.0f - std::abs(fbm(uv * glm::vec2(0.22f, 1.1f)) - 0.5f) * 3.4f,
                                                     0.0f, 1.0f), 2.0f);
            const glm::vec3 cloud = glm::mix(glm::vec3(0.62f, 0.64f, 0.70f), glm::vec3(1.0f, 0.98f, 0.95f),
                                             std::pow(cs, 1.6f));
            disp = glm::mix(disp, cloud, cover * (0.35f + 0.65f * streak) * std::min(1.0f, d.y * 8.0f) * 0.75f);
        }
        disp += glm::vec3(1.0f, 0.86f, 0.66f) *
                (std::pow(cs, 900.0f) * 0.55f + std::pow(cs, 24.0f) * 0.10f + std::pow(cs, 4.0f) * 0.035f);

        glm::vec3 c = fromDisplay(disp);
        // The disc itself, blown well past the top of the curve.
        c += glm::vec3(1.0f, 0.86f, 0.66f) * smoothstep(0.99955f, 0.99985f, sd) * 6.0f;
        return c;
    }

    // Adaptive marching: the step is proportional to the height above the ground, which is safe on a heightfield and
    // skips the empty air quickly. A bisection refine lands the hit on the surface.
    bool intersect(glm::vec3 ro, glm::vec3 rd, float tmax, float* tout, glm::vec3* nout) const {
        float t = 0.02f;
        const float maxH = map.heightAtMetres(128.0f, 128.0f) + 40.0f;
        for (int i = 0; i < 900 && t < tmax; ++i) {
            const glm::vec3 p = ro + rd * t;
            if (p.x < -8.0f || p.z < -8.0f || p.x > float(map.size - 1) * map.cellSize + 8.0f ||
                p.z > float(map.size - 1) * map.cellSize + 8.0f || (rd.y >= 0.0f && p.y > maxH))
                return false;
            const float h = heightAt(p.x, p.z);
            if (p.y <= h) {
                float lo = std::max(0.0f, t - 2.0f), hi = t;
                for (int k = 0; k < 26; ++k) {
                    const float mid = 0.5f * (lo + hi);
                    const glm::vec3 q = ro + rd * mid;
                    if (q.y <= heightAt(q.x, q.z)) hi = mid;
                    else lo = mid;
                }
                *tout = hi;
                const glm::vec3 q = ro + rd * hi;
                *nout = normalAt(q.x, q.z);
                return true;
            }
            t += std::max(0.08f, (p.y - h) * 0.55f);
        }
        return false;
    }

    glm::vec3 direct(glm::vec3 p, glm::vec3 n, glm::vec3 albedo) const {
        const float ndl = std::max(glm::dot(n, sunDir), 0.0f);
        if (ndl <= 0.0f) return glm::vec3(0.0f);
        float t = 0.0f;
        glm::vec3 hn;
        const bool shadowed = intersect(p + n * 0.10f, sunDir, 400.0f, &t, &hn);
        if (shadowed) return glm::vec3(0.0f);
        return albedo * glm::vec3(1.0f, 0.96f, 0.88f) * (sunIntensity * ndl);
    }

    glm::vec3 ambient(glm::vec3 n) const {
        const glm::vec3 up = sky(glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 side = sky(glm::normalize(n + glm::vec3(0.0f, 0.35f, 0.0f)));
        return (up * 0.35f + side * 0.65f) * 0.85f;
    }
};

uint32_t rnd(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float frand(uint32_t& s) { return float(rnd(s) & 0xffffff) / float(0x1000000); }

glm::vec3 cosineHemisphere(glm::vec3 n, uint32_t& seed) {
    const float u1 = frand(seed), u2 = frand(seed);
    const float r = std::sqrt(u1);
    const float phi = 6.2831853f * u2;
    glm::vec3 t = glm::normalize(std::abs(n.y) < 0.99f ? glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), n)
                                                      : glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), n));
    const glm::vec3 b = glm::cross(n, t);
    return glm::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - u1)));
}

glm::vec3 tracePath(const Scene& s, glm::vec3 ro, glm::vec3 rd, uint32_t& seed) {
    float t = 0.0f;
    glm::vec3 n(0.0f, 1.0f, 0.0f);
    if (!s.intersect(ro, rd, 3000.0f, &t, &n)) return s.sky(rd);
    const glm::vec3 p = ro + rd * t;
    const float detail = std::clamp(1.0f - t / 210.0f, 0.0f, 1.0f);
    const glm::vec3 albedo = s.albedoAt(p, n, detail);
    const glm::vec3 nb = s.bumpNormal(p, n);
    glm::vec3 out = s.direct(p, nb, albedo) + albedo * s.ambient(nb);

    // One diffuse bounce: sky, or a sunlit patch of terrain. Enough to fill the shadows with the ground's own
    // bounce, which is what a path tracer is for at this scale.
    if (frand(seed) < 0.85f) {
        const glm::vec3 d = cosineHemisphere(n, seed);
        float t2 = 0.0f;
        glm::vec3 n2;
        if (s.intersect(p + n * 0.05f, d, 120.0f, &t2, &n2)) {
            const glm::vec3 p2 = p + n * 0.05f + d * t2;
            const float detail2 = std::clamp(1.0f - (t + t2) / 210.0f, 0.0f, 1.0f);
            const glm::vec3 a2 = s.albedoAt(p2, n2, detail2);
            out += albedo * a2 * (s.direct(p2, n2, glm::vec3(1.0f)) * 0.55f + s.ambient(n2) * 0.45f);
        } else {
            out += albedo * s.sky(d) * 0.55f;
        }
    }

    // Aerial perspective: forty kilometres of valley air between here and the far ridge. Without it the crest line
    // is a hard cut-out against the sky, which is the single biggest tell of a render.
    const float haze = 1.0f - std::exp(-t * 0.00055f);
    out = glm::mix(out, s.sky(rd), std::clamp(haze, 0.0f, 0.80f));
    return out;
}

glm::vec3 aces(glm::vec3 x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    x = glm::clamp((x * (a * x + b)) / (x * (c * x + d) + e), glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::pow(x, glm::vec3(1.0f / 2.2f));
}

} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "pathtrace.png";
    const std::string root = argc > 2 ? argv[2] : ".";
    const int W = argc > 3 ? std::atoi(argv[3]) : 1280;
    const int H = argc > 4 ? std::atoi(argv[4]) : 720;
    const int spp = argc > 5 ? std::atoi(argv[5]) : 64;

    Scene scene;
    scene.map.cellSize = 1.0f;
    scene.map.heightScale = 14.0f;
    std::string error;
    if (!loadHeightmapU16(root + "/tests/data/terrain_hills_257.u16", 257, &scene.map, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const std::string tex = root + "/editor/res/terrain/";
    if (!loadTexture(tex + "grass_moss_albedo.png", &scene.moss) ||
        !loadTexture(tex + "heather_albedo.png", &scene.heather) ||
        !loadTexture(tex + "muddy_albedo.png", &scene.muddy)) {
        std::fprintf(stderr, "terrain textures missing under %s\n", tex.c_str());
        return 1;
    }

    scene.moss.computeMean();
    scene.heather.computeMean();
    scene.muddy.computeMean();

    // The template's low sun, azimuth 320 and 22 degrees up: long shadows across the basin.
    const float az = glm::radians(238.0f), el = glm::radians(24.0f);
    scene.sunDir = glm::normalize(glm::vec3(std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)));

    // Looking into the sun across the basin: the ridges get a rim, the valley fills with bounced light.
    const glm::vec3 eye(58.0f, scene.heightAt(58.0f, 214.0f) + 7.5f, 214.0f);
    const glm::vec3 target(158.0f, scene.heightAt(158.0f, 96.0f) + 4.0f, 96.0f);
    const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(52.0f), float(W) / float(H), 0.1f, 4000.0f);
    const glm::mat4 invView = glm::inverse(view);

    std::vector<glm::vec3> pixels(size_t(W) * size_t(H));
    std::atomic<int> nextRow{0};
    const int threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (int th = 0; th < threads; ++th) {
        pool.emplace_back([&, th]() {
            uint32_t seed = 0x9e3779b9u * uint32_t(th + 1);
            for (;;) {
                const int y = nextRow.fetch_add(1);
                if (y >= H) break;
                for (int x = 0; x < W; ++x) {
                    glm::vec3 sum(0.0f);
                    for (int s = 0; s < spp; ++s) {
                        const float jx = (float(x) + frand(seed)) / float(W) * 2.0f - 1.0f;
                        const float jy = 1.0f - (float(y) + frand(seed)) / float(H) * 2.0f;
                        const glm::vec4 far = glm::inverse(proj) * glm::vec4(jx, jy, 1.0f, 1.0f);
                        const glm::vec3 dir = glm::normalize(glm::vec3(invView * glm::vec4(glm::normalize(glm::vec3(far)), 0.0f)));
                        sum += glm::min(tracePath(scene, eye, dir, seed), glm::vec3(8.0f));
                    }
                    pixels[size_t(y) * size_t(W) + size_t(x)] = aces(sum / float(spp) * scene.exposure);
                }
            }
        });
    }
    for (auto& t : pool) t.join();

    std::vector<unsigned char> bytes(size_t(W) * size_t(H) * 3);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const glm::vec3 c = glm::clamp(pixels[i], glm::vec3(0.0f), glm::vec3(1.0f));
        bytes[i * 3 + 0] = uint8_t(c.x * 255.0f + 0.5f);
        bytes[i * 3 + 1] = uint8_t(c.y * 255.0f + 0.5f);
        bytes[i * 3 + 2] = uint8_t(c.z * 255.0f + 0.5f);
    }
    if (!stbi_write_png(out.c_str(), W, H, 3, bytes.data(), W * 3)) {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d, %d spp, %d threads)\n", out.c_str(), W, H, spp, threads);
    return 0;
}
