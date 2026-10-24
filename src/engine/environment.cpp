// The maths behind environment lighting; the design contract is in environment.h.
//
// Two conventions are load-bearing and tested from both sides:
//   * The cubemap layout is the GPU's (face order +X,-X,+Y,-Y,+Z,-Z; the mapping in faceDirection matches what a
//     samplerCube does with the bytes we upload), so a bake needs no swizzle on the way to the device.
//   * The SH basis is the standard real basis with the documented normalisations, and the coefficients hold
//     irradiance in colour units: a uniform white environment evaluates to (1, 1, 1) at every direction, so a scene
//     switching the ambient on does not change brightness, only directionality.
#include "engine/environment.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ukiyo::env {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// The real SH basis normalisations.
constexpr float kY00 = 0.282095f;
constexpr float kY1 = 0.488603f;      // L1-1, L10, L11
constexpr float kY2 = 1.092548f;      // L2-2, L2-1, L21
constexpr float kY20 = 0.315392f;     // L20: 0.315392 * (3z^2 - 1)
constexpr float kY22 = 0.546274f;     // L22: 0.546274 * (x^2 - y^2)

// Ramamoorthi's cosine-convolution band scales over pi, because coefficients hold irradiance in colour units:
// a uniform white radiance cubemap (radiance 1, irradiance pi) must evaluate to 1.
constexpr float kBandScale[3] = {1.0f, 2.0f / 3.0f, 0.25f};

// A seeded hash2/vnoise/fbm, the same constants shaders/sky.frag uses, so the CPU bake and the screen sky agree.
float hash21(glm::vec2 p) {
    p = glm::fract(p * glm::vec2(123.34f, 456.21f));
    p += glm::dot(p, p + 45.32f);
    return glm::fract(p.x * p.y);
}
float vnoise(glm::vec2 p) {
    glm::vec2 i = glm::floor(p), f = glm::fract(p);
    glm::vec2 u = f * f * (3.0f - 2.0f * f);
    float a = hash21(i), b = hash21(i + glm::vec2(1, 0));
    float c = hash21(i + glm::vec2(0, 1)), d = hash21(i + glm::vec2(1, 1));
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

// 2D Halton (bases 2, 3) for the prefilter's low-discrepancy samples.
float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

// GGX NDF sample around +Z (Walter et al.): theta from the inverse CDF of D(h). alpha is the perceptually-squared
// roughness, as the split-sum reference prescribes.
glm::vec3 ggxH(float u1, float u2, float alpha) {
    const float phi = 2.0f * kPi * u1;
    const float cosT = std::sqrt((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
    const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    return glm::vec3(sinT * std::cos(phi), sinT * std::sin(phi), cosT);
}

} // namespace

// ---------------------------------------------------------------- Cubemaps

glm::vec3 faceDirection(int faceIndex, float u, float v) {
    // Fractions in [0, 1] (0 = the left/top edge) to face space [-1, 1], v running down from the top row.
    const float a = 2.0f * u - 1.0f;
    const float b = 2.0f * v - 1.0f;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    switch (faceIndex) {
    case 0: x =  1.0f; y = -b; z = -a; break;   // +X
    case 1: x = -1.0f; y = -b; z =  a; break;   // -X
    case 2: x =  a; y =  1.0f; z =  b; break;   // +Y
    case 3: x =  a; y = -1.0f; z = -b; break;   // -Y
    case 4: x =  a; y = -b; z =  1.0f; break;   // +Z
    default: x = -a; y = -b; z = -1.0f; break;  // -Z
    }
    return glm::normalize(glm::vec3(x, y, z));
}

bool dirToFace(const glm::vec3& dir, int* faceIndex, float* u, float* v) {
    const float ax = std::abs(dir.x), ay = std::abs(dir.y), az = std::abs(dir.z);
    float sc = 0.0f, tc = 0.0f, ma = 0.0f;
    if (ax >= ay && ax >= az) {
        if (dir.x > 0) { *faceIndex = 0; sc = -dir.z; tc = -dir.y; }
        else           { *faceIndex = 1; sc =  dir.z; tc = -dir.y; }
        ma = ax;
    } else if (ay >= az) {
        if (dir.y > 0) { *faceIndex = 2; sc =  dir.x; tc =  dir.z; }
        else           { *faceIndex = 3; sc =  dir.x; tc = -dir.z; }
        ma = ay;
    } else {
        if (dir.z > 0) { *faceIndex = 4; sc =  dir.x; tc = -dir.y; }
        else           { *faceIndex = 5; sc = -dir.x; tc = -dir.y; }
        ma = az;
    }
    if (ma <= 0.0f) return false;
    *u = (sc / ma + 1.0f) * 0.5f;
    *v = (tc / ma + 1.0f) * 0.5f;
    return true;
}

glm::vec4 Cubemap::sample(const glm::vec3& dir) const {
    int f = 0; float u = 0.0f, v = 0.0f;
    if (!dirToFace(dir, &f, &u, &v)) return glm::vec4(0.0f);
    const float x = std::clamp(u * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
    const float y = std::clamp(v * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const std::vector<glm::vec4>& t = face[f];
    const int n = size;
    const glm::vec4 c00 = t[static_cast<size_t>(y0 * n + x0)], c10 = t[static_cast<size_t>(y0 * n + x1)];
    const glm::vec4 c01 = t[static_cast<size_t>(y1 * n + x0)], c11 = t[static_cast<size_t>(y1 * n + x1)];
    return glm::mix(glm::mix(c00, c10, fx), glm::mix(c01, c11, fx), fy);
}

float texelSolidAngle(int size, float u, float v) {
    // Exact for a face texel: dw = 4 da db / (1 + a^2 + b^2)^(3/2) over the texel's footprint in face space.
    const float step = 2.0f / static_cast<float>(size);
    const float a = 2.0f * u - 1.0f;
    const float b = 2.0f * v - 1.0f;
    const float len = std::sqrt(a * a + b * b + 1.0f);
    return step * step / (len * len * len);   // the Jacobian of the face mapping, no constant
}

glm::vec2 dirToEquirect(const glm::vec3& dir, int width, int height) {
    const float az = std::atan2(dir.z, dir.x);
    const float u = az / (2.0f * kPi) + 0.5f;
    const float v = std::acos(std::clamp(dir.y, -1.0f, 1.0f)) / kPi;
    return glm::vec2(u * static_cast<float>(width) - 0.5f, v * static_cast<float>(height) - 0.5f);
}

glm::vec3 skyRadiance(const glm::vec3& zenith, const glm::vec3& horizon, const glm::vec3& sunDir, float sunDisc,
                      float cloudCover, const glm::vec3& rd) {
    // shaders/sky.frag, texel for texel: gradient, warm lift, disc and halo; then the cloud layer with its cover.
    const float y = rd.y;
    const glm::vec3 S = glm::normalize(sunDir);
    const float t = std::pow(std::clamp(y, 0.0f, 1.0f), 0.45f);
    const float sunAmt = std::pow(std::max(glm::dot(rd, S), 0.0f), 3.0f);
    glm::vec3 col = glm::mix(horizon, zenith, t);
    col = glm::mix(col, glm::vec3(1.0f, 0.86f, 0.72f), sunAmt * 0.35f * (1.0f - t));

    const float sd = std::max(glm::dot(rd, S), 0.0f);
    const float disc = glm::smoothstep(0.9992f, 0.9997f, sd);
    col += glm::vec3(1.0f, 0.97f, 0.88f) * disc * 6.0f * sunDisc;
    col += glm::vec3(1.0f, 0.82f, 0.58f) * (std::pow(sd, 28.0f) * 0.8f + std::pow(sd, 5.0f) * 0.20f) * sunDisc;

    if (y > 0.0f && cloudCover > 0.0f) {
        glm::vec2 cuv = glm::vec2(rd.x, rd.z) / (y + 0.14f) * 1.1f;
        const float n = fbm(cuv * 1.3f);
        const float c = glm::smoothstep(0.50f, 0.60f, n);
        const float nl = fbm(cuv * 1.3f + glm::vec2(S.x, S.z) * 0.10f);
        const float lit = n - nl - 0.012f >= 0.0f ? 1.0f : 0.0f;
        const glm::vec3 cloud = glm::mix(glm::vec3(0.72f, 0.80f, 0.94f), glm::vec3(1.00f, 0.985f, 0.96f) * 1.15f, lit);
        const float fade = glm::smoothstep(0.02f, 0.30f, y);
        col = glm::mix(col, cloud, c * fade * cloudCover);
    }
    return glm::min(col, glm::vec3(kEnvRadianceClamp));
}

Cubemap fromEquirect(const uint8_t* rgba8, int width, int height, int size) {
    Cubemap out;
    if (!rgba8 || width <= 0 || height <= 0) return out;
    out.size = std::max(4, size);
    const auto fetch = [&](float u01, float v01) {
        const float x = std::clamp(u01 * static_cast<float>(width) - 0.5f, 0.0f, static_cast<float>(width - 1));
        const float y = std::clamp(v01 * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
        const auto px = [&](int xx, int yy) {
            const uint8_t* p = rgba8 + (static_cast<size_t>(yy) * width + xx) * 4;
            return glm::vec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
        };
        return glm::mix(glm::mix(px(x0, y0), px(x1, y0), fx), glm::mix(px(x0, y1), px(x1, y1), fx), fy);
    };
    for (int f = 0; f < kCubeFaces; ++f) {
        out.face[f].assign(static_cast<size_t>(out.size) * out.size, glm::vec4(0.0f));
        for (int y = 0; y < out.size; ++y)
            for (int x = 0; x < out.size; ++x) {
                const glm::vec3 dir = faceDirection(f, (x + 0.5f) / static_cast<float>(out.size),
                                                    (y + 0.5f) / static_cast<float>(out.size));
                const glm::vec2 uv = dirToEquirect(dir, width, height);
                out.face[f][static_cast<size_t>(y * out.size + x)] =
                    glm::min(fetch((uv.x + 0.5f) / static_cast<float>(width),
                                   (uv.y + 0.5f) / static_cast<float>(height)),
                             glm::vec4(kEnvRadianceClamp, kEnvRadianceClamp, kEnvRadianceClamp, 1.0f));
            }
    }
    return out;
}

Cubemap bakeSkyCubemap(const glm::vec3& zenith, const glm::vec3& horizon, const glm::vec3& sunDir, float sunDisc,
                       float cloudCover, int size) {
    Cubemap env;
    env.size = std::max(4, size);
    for (int f = 0; f < kCubeFaces; ++f) {
        env.face[f].assign(static_cast<size_t>(env.size) * env.size, glm::vec4(0.0f));
        for (int y = 0; y < env.size; ++y)
            for (int x = 0; x < env.size; ++x) {
                const glm::vec3 rd = faceDirection(f, (x + 0.5f) / static_cast<float>(env.size),
                                                   (y + 0.5f) / static_cast<float>(env.size));
                env.face[f][static_cast<size_t>(y * env.size + x)] =
                    glm::vec4(skyRadiance(zenith, horizon, sunDir, sunDisc, cloudCover, rd), 1.0f);
            }
    }
    return env;
}

std::vector<std::vector<uint8_t>> packMipChain(const Cubemap& base, const std::vector<Cubemap>& prefiltered) {
    std::vector<std::vector<uint8_t>> chain;
    if (base.empty()) return chain;
    int levels = 0;
    while ((base.size >> levels) > 0) ++levels;
    auto toBytes = [](const Cubemap& c) {
        std::vector<uint8_t> bytes(static_cast<size_t>(kCubeFaces) * c.size * c.size * 4);
        for (int f = 0; f < kCubeFaces; ++f)
            for (int i = 0; i < c.size * c.size; ++i) {
                const glm::vec4 px = glm::clamp(c.face[f][static_cast<size_t>(i)], glm::vec4(0.0f), glm::vec4(1.0f));
                uint8_t* dst = &bytes[(static_cast<size_t>(f) * c.size * c.size + static_cast<size_t>(i)) * 4];
                dst[0] = static_cast<uint8_t>(px.r * 255.0f + 0.5f);
                dst[1] = static_cast<uint8_t>(px.g * 255.0f + 0.5f);
                dst[2] = static_cast<uint8_t>(px.b * 255.0f + 0.5f);
                dst[3] = static_cast<uint8_t>(px.a * 255.0f + 0.5f);
            }
        return bytes;
    };
    chain.push_back(toBytes(base));
    Cubemap prev = base;
    for (int l = 1; l < levels; ++l) {
        Cubemap level;
        if (static_cast<int>(prefiltered.size()) > l) {
            // `prefiltered` is the full chain (entry 0 is the base, already pushed above), so mip l is entry l.
            level = prefiltered[l];
        } else {
            // No prefilter supplied: a 2x2 box downsample of the previous level keeps the chain complete.
            const int s = std::max(1, base.size >> l), sp = std::max(1, base.size >> (l - 1));
            level.size = s;
            for (int f = 0; f < kCubeFaces; ++f) {
                level.face[f].assign(static_cast<size_t>(s) * s, glm::vec4(0.0f));
                for (int y = 0; y < s; ++y)
                    for (int x = 0; x < s; ++x) {
                        glm::vec4 sum(0.0f);
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                sum += prev.face[f][static_cast<size_t>(
                                    std::min(y * 2 + dy, sp - 1) * sp + std::min(x * 2 + dx, sp - 1))];
                        level.face[f][static_cast<size_t>(y * s + x)] = sum * 0.25f;
                    }
            }
        }
        prev = level;
        chain.push_back(toBytes(level));
    }
    return chain;
}

// ------------------------------------------------------------ Spherical harmonics

Sh Sh::zero() { return Sh{}; }

void Sh::addScaled(const Sh& other, float scale) {
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 9; ++i) c[ch][i] += other.c[ch][i] * scale;
}

Sh Sh::lerp(const Sh& a, const Sh& b, float t) {
    Sh out;
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 9; ++i) out.c[ch][i] = a.c[ch][i] + (b.c[ch][i] - a.c[ch][i]) * t;
    return out;
}

void shBasis(const glm::vec3& n, float out[9]) {
    out[0] = kY00;
    out[1] = kY1 * n.y;
    out[2] = kY1 * n.z;
    out[3] = kY1 * n.x;
    out[4] = kY2 * n.x * n.y;
    out[5] = kY2 * n.y * n.z;
    out[6] = kY20 * (3.0f * n.z * n.z - 1.0f);
    out[7] = kY2 * n.x * n.z;
    out[8] = kY22 * (n.x * n.x - n.y * n.y);
}

Sh projectIrradiance(const std::function<glm::vec3(const glm::vec3&)>& radiance, int samples) {
    const int n = std::max(64, samples);
    Sh out;
    // Fibonacci sphere: even coverage without a random seed, so a bake is byte-stable.
    const float ga = kPi * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < n; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const float z = 1.0f - 2.0f * t;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float a = ga * static_cast<float>(i);
        const glm::vec3 dir(r * std::cos(a), z, r * std::sin(a));
        const glm::vec3 v = radiance(dir);
        float b[9];
        shBasis(dir, b);
        const float w = 4.0f * kPi / static_cast<float>(n);
        for (int ch = 0; ch < 3; ++ch)
            for (int k = 0; k < 9; ++k) out.c[ch][k] += v[ch] * b[k] * w;
    }
    return out;
}

Sh irradianceFromCubemap(const Cubemap& env) {
    Sh out;
    if (env.empty()) return out;
    for (int f = 0; f < kCubeFaces; ++f)
        for (int y = 0; y < env.size; ++y)
            for (int x = 0; x < env.size; ++x) {
                const glm::vec4 px = env.face[f][static_cast<size_t>(y * env.size + x)];
                const glm::vec3 dir = faceDirection(f, (x + 0.5f) / static_cast<float>(env.size),
                                                    (y + 0.5f) / static_cast<float>(env.size));
                float b[9];
                shBasis(dir, b);
                const float w = texelSolidAngle(env.size, (x + 0.5f) / static_cast<float>(env.size),
                                                (y + 0.5f) / static_cast<float>(env.size));
                for (int ch = 0; ch < 3; ++ch)
                    for (int k = 0; k < 9; ++k) out.c[ch][k] += px[ch] * b[k] * w;
            }
    // Radiance to irradiance, in colour units (the band scales over pi).
    for (int ch = 0; ch < 3; ++ch)
        for (int k = 0; k < 9; ++k) out.c[ch][k] *= kBandScale[k / 3];
    return out;
}

glm::vec3 evalSh(const Sh& sh, const glm::vec3& n) {
    float b[9];
    shBasis(n, b);
    glm::vec3 v(0.0f);
    for (int ch = 0; ch < 3; ++ch) {
        float s = 0.0f;
        for (int k = 0; k < 9; ++k) s += sh.c[ch][k] * b[k];
        v[ch] = s;
    }
    return v;
}

glm::vec3 equalizedGi(const Sh& sh, const glm::vec3& n, float equalization) {
    const glm::vec3 raw = evalSh(sh, n);
    const glm::vec3 flat =
        (evalSh(sh, glm::vec3(0.0f, 1.0f, 0.0f)) + evalSh(sh, glm::vec3(0.0f, -1.0f, 0.0f))) * 0.5f;
    return glm::mix(raw, flat, std::clamp(equalization, 0.0f, 1.0f));
}

void addLightToSh(Sh* sh, const glm::vec3& p, const EnvLight& light) {
    if (!sh || !light.enabled || light.intensity <= 0.0f) return;
    if (light.type == 0) {
        // Directional: the light's irradiance function max(0, n . -direction), projected.
        const glm::vec3 l = -glm::normalize(light.direction);
        const glm::vec3 colour = light.color * light.intensity;
        sh->addScaled(projectIrradiance([&](const glm::vec3& n) {
            const float ndl = glm::dot(n, l);
            return ndl > 0.0f ? colour * ndl : glm::vec3(0.0f);
        }), 1.0f);
        return;
    }
    // Point and spot: the windowed inverse-square the toon shader draws, times max(0, n . l) and the cone.
    const glm::vec3 toLight = light.position - p;
    const float dist = glm::length(toLight);
    if (dist < 1e-4f) return;
    const glm::vec3 l = toLight / dist;
    const float window = std::clamp(1.0f - dist / std::max(light.radius, 1e-3f), 0.0f, 1.0f);
    float att = window * window / (1.0f + dist * dist);
    if (att <= 0.0f) return;
    if (light.type == 2) {
        const float cosOuter = std::cos(std::clamp(light.spotAngleDeg, 1.0f, 179.0f) * 0.5f * kPi / 180.0f);
        const float soft = std::clamp(light.spotSoftness, 0.0f, 0.95f);
        const float cosInner = cosOuter + soft * (1.0f - cosOuter);
        const float cd = glm::dot(-l, glm::normalize(light.direction));
        if (cd < cosOuter) return;
        att *= glm::smoothstep(cosOuter, std::max(cosInner, cosOuter + 1e-3f), cd);
    }
    const glm::vec3 colour = light.color * light.intensity * att;
    sh->addScaled(projectIrradiance([&](const glm::vec3& n) {
        const float ndl = glm::dot(n, l);
        return ndl > 0.0f ? colour * ndl : glm::vec3(0.0f);
    }), 1.0f);
}

// ------------------------------------------------------------------ Probes

glm::vec3 ProbeVolume::probePosition(int i, int j, int k) const {
    const auto at = [](int index, int count, float half) {
        if (count <= 1) return 0.0f;
        return (static_cast<float>(index) / static_cast<float>(count - 1) * 2.0f - 1.0f) * half;
    };
    return center + glm::vec3(at(i, divX, halfExtent.x), at(j, divY, halfExtent.y), at(k, divZ, halfExtent.z));
}

Sh ProbeVolume::sample(const glm::vec3& p) const {
    Sh out;
    if (probes.empty()) return out;
    const int nx = std::max(1, divX), ny = std::max(1, divY), nz = std::max(1, divZ);
    // Local coordinates in [0, n-1], clamped at the box (Unity's volumes clamp too).
    const auto axis = [](float world, float centre, float half, int count) {
        if (count <= 1) return 0.0f;
        const float t = (world - centre) / std::max(half * 2.0f, 1e-6f) + 0.5f;
        return std::clamp(t, 0.0f, 1.0f) * static_cast<float>(count - 1);
    };
    const float fx = axis(p.x, center.x, halfExtent.x, nx);
    const float fy = axis(p.y, center.y, halfExtent.y, ny);
    const float fz = axis(p.z, center.z, halfExtent.z, nz);
    const int x0 = std::min(static_cast<int>(fx), nx - 1), y0 = std::min(static_cast<int>(fy), ny - 1),
              z0 = std::min(static_cast<int>(fz), nz - 1);
    const int x1 = std::min(x0 + 1, nx - 1), y1 = std::min(y0 + 1, ny - 1), z1 = std::min(z0 + 1, nz - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0), tz = fz - static_cast<float>(z0);
    const auto at = [&](int i, int j, int k) -> const Sh& {
        return probes[static_cast<size_t>((static_cast<size_t>(k) * ny + j) * nx + i)];
    };
    for (int ch = 0; ch < 3; ++ch)
        for (int c = 0; c < 9; ++c) {
            const float c00 = glm::mix(at(x0, y0, z0).c[ch][c], at(x1, y0, z0).c[ch][c], tx);
            const float c10 = glm::mix(at(x0, y1, z0).c[ch][c], at(x1, y1, z0).c[ch][c], tx);
            const float c01 = glm::mix(at(x0, y0, z1).c[ch][c], at(x1, y0, z1).c[ch][c], tx);
            const float c11 = glm::mix(at(x0, y1, z1).c[ch][c], at(x1, y1, z1).c[ch][c], tx);
            out.c[ch][c] = glm::mix(glm::mix(c00, c10, ty), glm::mix(c01, c11, ty), tz);
        }
    return out;
}

void bakeVolume(ProbeVolume* volume, const Sh& envIrradiance, const std::vector<EnvLight>& lights) {
    if (!volume || volume->divX < 1 || volume->divY < 1 || volume->divZ < 1) return;
    const int nx = volume->divX, ny = volume->divY, nz = volume->divZ;
    volume->probes.assign(static_cast<size_t>(nx) * ny * nz, envIrradiance);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                Sh& probe = volume->probes[static_cast<size_t>((static_cast<size_t>(k) * ny + j) * nx + i)];
                const glm::vec3 p = volume->probePosition(i, j, k);
                for (const EnvLight& l : lights) addLightToSh(&probe, p, l);
            }
}

// ------------------------------------------------------------------ Filter

Cubemap prefilter(const Cubemap& env, float roughness, int samples) {
    Cubemap out;
    if (env.empty()) return out;
    out.size = env.size;
    const float alpha = std::max(roughness * roughness, 1e-3f);
    const int k = std::max(8, samples);
    for (int f = 0; f < kCubeFaces; ++f) {
        out.face[f].assign(static_cast<size_t>(env.size) * env.size, glm::vec4(0.0f));
        for (int y = 0; y < env.size; ++y)
            for (int x = 0; x < env.size; ++x) {
                const glm::vec3 n = faceDirection(f, (x + 0.5f) / static_cast<float>(env.size),
                                                  (y + 0.5f) / static_cast<float>(env.size));
                const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                            : glm::vec3(1.0f, 0.0f, 0.0f);
                const glm::vec3 tx = glm::normalize(glm::cross(up, n));
                const glm::vec3 ty = glm::cross(n, tx);
                glm::vec4 sum(0.0f);
                float weight = 0.0f;
                for (int i = 1; i <= k; ++i) {
                    const glm::vec3 h = ggxH(halton(i, 2), halton(i, 3), alpha);
                    const glm::vec3 hWorld = glm::normalize(tx * h.x + ty * h.y + n * h.z);
                    const glm::vec3 l = glm::normalize(2.0f * glm::dot(n, hWorld) * hWorld - n);
                    const float nl = glm::dot(n, l);
                    if (nl <= 0.0f) continue;
                    sum += env.sample(l) * nl;
                    weight += nl;
                }
                out.face[f][static_cast<size_t>(y * env.size + x)] =
                    weight > 0.0f ? sum / weight : env.sample(n);
            }
    }
    return out;
}

std::vector<Cubemap> prefilteredChain(const Cubemap& base) {
    std::vector<Cubemap> chain;
    if (base.empty()) return chain;
    // The full mip count — the RHI allocates the same chain (floor(log2(size)) + 1 levels, the last 1 texel).
    int levels = 0;
    while ((base.size >> levels) > 0) ++levels;
    chain.reserve(static_cast<size_t>(levels));
    chain.push_back(base);
    for (int l = 1; l < levels; ++l) {
        // Prefilter at the source size, then box-downsample to this mip's size: the GGX lobe is defined against the
        // full-resolution environment, and the texel footprint shrinks with it.
        const Cubemap pre = prefilter(base, static_cast<float>(l) / static_cast<float>(levels - 1));
        const int s = std::max(1, base.size >> l);
        Cubemap level;
        level.size = s;
        for (int f = 0; f < kCubeFaces; ++f) {
            level.face[f].assign(static_cast<size_t>(s) * s, glm::vec4(0.0f));
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x) {
                    // A 2x2 ladder: each coarse texel averages the four covering texels of the previous size.
                    const int ps = s * 2;
                    glm::vec4 sum(0.0f);
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            sum += pre.face[f][static_cast<size_t>(std::min(y * 2 + dy, ps - 1) * ps +
                                                                           std::min(x * 2 + dx, ps - 1))];
                    level.face[f][static_cast<size_t>(y * s + x)] = sum * 0.25f;
                }
        }
        chain.push_back(level);
    }
    return chain;
}

// ------------------------------------------------------------------ Pack

FrameEnv packFrameEnv(const std::vector<ProbeVolume>& volumes, const Sh& wholeScene, const glm::vec3& focus,
                      float intensity, bool cubePresent, int mipCount) {
    Sh probe = wholeScene;
    for (const ProbeVolume& v : volumes)
        if (!v.probes.empty() && glm::all(glm::greaterThanEqual(focus, v.center - v.halfExtent)) &&
            glm::all(glm::lessThanEqual(focus, v.center + v.halfExtent))) {
            probe = v.sample(focus);
            break;   // later volumes win where boxes overlap
        }
    FrameEnv out;
    for (int i = 0; i < 9; ++i) out.sh[i] = glm::vec4(probe.c[0][i], probe.c[1][i], probe.c[2][i], 0.0f);
    out.params = glm::vec4(intensity > 0.0f ? 1.0f : 0.0f, cubePresent ? 1.0f : 0.0f, std::max(intensity, 0.0f),
                           static_cast<float>(std::max(1, mipCount)));
    return out;
}

} // namespace ukiyo::env
