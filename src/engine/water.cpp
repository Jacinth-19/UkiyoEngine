#include "engine/water.h"

#include <cmath>

namespace ukiyo {

namespace {
constexpr float kGravity = 9.81f;
constexpr float kTwoPi = 6.28318530718f;
constexpr int kWaveCount = 4;
}

glm::vec3 waterOffset(const WaterVolume& v, float x0, float z0, float t, glm::vec3* normalOut) {
    // Partial derivatives of the displaced position with respect to the rest point.
    glm::vec3 offset(0.0f);
    glm::vec3 dPdx(1.0f, 0.0f, 0.0f);
    glm::vec3 dPdz(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < kWaveCount; ++i) {
        const WaterWave& w = v.waves[i];
        glm::vec2 d = w.dir;
        const float dl = std::sqrt(d.x * d.x + d.y * d.y);
        d = dl > 1e-6f ? d / dl : glm::vec2(1.0f, 0.0f);
        const float a = w.amplitude > 0.0f ? w.amplitude : 0.0f;
        const float k = kTwoPi / (w.wavelength > 0.01f ? w.wavelength : 0.01f);
        const float omega = std::sqrt(kGravity * k) * w.speed;
        const float steep = w.steepness < 0.0f ? 0.0f : (w.steepness > 1.0f ? 1.0f : w.steepness);
        // Q = steep / (k A N) keeps the sum of Q k A over the waves at or below `steep`.
        const float ka = k * a;
        const float q = ka > 1e-6f ? steep / (ka * float(kWaveCount)) : 0.0f;
        const float phase = k * (d.x * x0 + d.y * z0) - omega * t;
        const float c = std::cos(phase);
        const float s = std::sin(phase);
        offset.x += q * a * d.x * c;
        offset.z += q * a * d.y * c;
        offset.y += a * s;
        dPdx.x -= q * ka * d.x * d.x * s;
        dPdx.y += ka * d.x * c;
        dPdx.z -= q * ka * d.x * d.y * s;
        dPdz.x -= q * ka * d.x * d.y * s;
        dPdz.y += ka * d.y * c;
        dPdz.z -= q * ka * d.y * d.y * s;
    }
    if (normalOut) {
        glm::vec3 n = glm::cross(dPdz, dPdx);
        const float len = glm::length(n);
        *normalOut = len > 1e-6f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return offset;
}

WaterSurface waterSurface(const WaterVolume& v, float x, float z, float t) {
    // Find the rest point r such that r + offset(r).xz == (x, z). Each step moves r by the offset error.
    glm::vec2 r(x, z);
    glm::vec3 n(0.0f, 1.0f, 0.0f);
    glm::vec3 off(0.0f);
    for (int it = 0; it < 4; ++it) {
        off = waterOffset(v, r.x, r.y, t, &n);
        r = glm::vec2(x - off.x, z - off.z);
    }
    off = waterOffset(v, r.x, r.y, t, &n);
    WaterSurface s;
    s.pos = glm::vec3(x, v.level + off.y, z);
    s.normal = n;
    float sumA = 0.0f;
    for (const WaterWave& w : v.waves) sumA += w.amplitude > 0.0f ? w.amplitude : 0.0f;
    const float h = sumA > 1e-4f ? 0.5f + 0.5f * off.y / sumA : 0.5f;
    // Smooth ramp: the top 18% of the height range becomes crest foam (matches water.vert).
    const float u = (h - 0.82f) / 0.16f;
    const float c = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    s.crest = c * c * (3.0f - 2.0f * c);
    return s;
}

float waterHeight(const WaterVolume& v, float x, float z, float t) {
    return waterSurface(v, x, z, t).pos.y;
}

glm::vec3 waterNormal(const WaterVolume& v, float x, float z, float t) {
    return waterSurface(v, x, z, t).normal;
}

bool waterContainsXZ(const WaterVolume& v, float x, float z) {
    return std::fabs(x - v.centre.x) <= v.half.x && std::fabs(z - v.centre.y) <= v.half.y;
}

} // namespace ukiyo
