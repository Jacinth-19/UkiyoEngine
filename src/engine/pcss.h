// Percentage-closer soft shadows (Flax 1.13's PCSS): a penumbra whose width comes from the light's own size.
//
// A shadow map answers "is this point lit" with one depth per texel, so every shadow it casts has an edge of one texel,
// whatever the light is. A fluorescent strip, a window, a sun two degrees wide and a point light all shadow the same.
// PCSS replaces the fixed filter radius with a two-step estimate taken from the map itself:
//
//   1. Blocker search. Around the receiver's shadow texel, average the depth of the texels that are nearer to the light
//      than the receiver. That is the distance to the thing casting the shadow at this point.
//   2. Penumbra. A light of diameter L, a blocker at distance dB and a receiver at dR put the edge of the shadow across
//      the width L * (dR - dB) / dB. Filter with that radius.
//
// The width falls to zero where the receiver touches the blocker and grows with the gap: contact hardening, for free,
// which is the reason the technique exists. This header is the CPU definition of both steps, shared by
// tests/pcss_test.cpp and tools/pcss_preview.cpp. shaders/shadow_sample.glsl runs the same two steps on the shadow map,
// with the tap pattern below, so the reference and the renderer cannot drift apart silently.
#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace ukiyo {

struct PcssParams {
    // 0 = one tap (a hard shadow), 1 = PCF with a fixed radius, 2 = PCSS. The reference and the figures use all three.
    int mode = 2;
    // Angular radius of a directional light's disc, as a tangent. The sun is 0.00465 (0.27 degrees); a stylised sun is
    // 0.03-0.1, and that is the difference between a crisp game shadow and a soft one.
    float sunTanRadius = 0.0175f;
    // Radius of a local light, in metres, for point and spot lights.
    float lightRadius = 0.25f;
    // True for the sun, false for a local light: the penumbra formula's denominator is the light's own distance.
    bool directional = true;
    // A penumbra wider than this reads as a smudge rather than a shadow, and it costs the same either way.
    float maxPenumbra = 0.4f;
    // Depth difference, in metres, below which a texel is the same surface and not a blocker: the map's own bias.
    float bias = 0.02f;
    // How much of the receiver's own surface the light sees at a slant: sqrt(1 - (N.L)^2), 0 for a surface facing the
    // light and 1 for one edge-on. A tap a metre across the light's plane lands on the receiver's plane a `slope` metre
    // nearer or farther, which is not a blocker and must not be counted as one. Without this a floor at a low sun
    // shadows itself with its own neighbours and every penumbra comes out several times too wide.
    float receiverSlope = 0.0f;
    // Radius, in metres, across the receiver plane that the blocker search reads. Too small misses a tall occluder's
    // contribution; too large lets a distant wall cast a shadow it does not.
    float searchRadius = 0.6f;
    int blockerSamples = 12;
    int pcfSamples = 24;
    // Mode 1's filter radius, in metres; PCSS ignores it.
    float fixedRadius = 0.12f;
};

// How many metres of penumbra a blocker at `blockerDistance` casts onto a receiver at `receiverDistance`. The contact
// case (the two equal) is zero, which is what makes an object sit on the floor instead of floating over a soft blob.
inline float penumbraWidth(const PcssParams& p, float blockerDistance, float receiverDistance) {
    const float gap = std::max(receiverDistance - blockerDistance, 0.0f);
    if (blockerDistance <= 1e-4f) return 0.0f;
    const float width = p.directional ? 2.0f * p.sunTanRadius * gap : p.lightRadius * gap / blockerDistance;
    return std::min(width, p.maxPenumbra);
}

// The exact visibility of a straight shadow edge under a disc light, for a receiver at `receiverDistance` behind an
// occluder edge at `blockerDistance`, `x` metres into the lit side. This is what PCSS is approximating: the fraction of
// the light's disc whose ray clears the edge, which is the area of a disc cut by a chord.
//
//   u   = x / ((receiverDistance - blockerDistance) * tanRadius)   -- the edge's position in disc radii
//   lit = (pi/2 + asin(u) + u * sqrt(1 - u^2)) / pi
//
// Tests compare PCSS against this, so a regression is measured against the answer rather than against another guess.
inline float analyticEdgeVisibility(float x, float blockerDistance, float receiverDistance, float tanRadius) {
    const float span = std::max(receiverDistance - blockerDistance, 1e-5f) * std::max(tanRadius, 1e-6f);
    const float u = glm::clamp(x / span, -1.0f, 1.0f);
    const float lit = (1.5707963267948966f + std::asin(u) + u * std::sqrt(std::max(1.0f - u * u, 0.0f))) /
                      3.1415926535897932f;
    return glm::clamp(lit, 0.0f, 1.0f);
}

// The tap pattern: a Vogel spiral, deterministic and evenly spread for any count, so the CPU reference and the shader
// produce the same offsets. Unit disc; scale by the radius wanted.
inline glm::vec2 vogelTap(int i, int n) {
    constexpr float kGoldenAngle = 2.39996323f;
    const float r = std::sqrt((float(i) + 0.5f) / float(std::max(n, 1)));
    const float a = float(i) * kGoldenAngle;
    return glm::vec2(r * std::cos(a), r * std::sin(a));
}

// The depth of whatever the shadow map holds at a UV, in metres from the light's plane; 0 or less means nothing there.
using ShadowDepthAt = float (*)(const void* map, glm::vec2 uv);

// One shadow query.
//
// `uv` is the receiver's shadow-map coordinate and `receiverDistance` its distance from the light along the light
// direction, in metres. `map`/`depthAt` are the map. `uvPerMetre` converts a distance on the light's own plane into
// shadow-map UV (1 / the cascade's world width); the shader computes the same value from its matrix.
//
// A world distance across the receiver is not the same distance on the light's plane. Along the sun's azimuth a metre on
// the floor foreshortens to |N.L| metres as the light sees it -- at a low sun almost the whole floor line lands on one
// shadow texel -- while across the azimuth it does not foreshorten at all. Filtering with the unfiltered world width
// over-filters a horizontal floor by 1/|N.L| (1.6x at this sun), which is why a soft shadow here used to be *worse* than
// a well-tuned fixed filter. The filter uses |N.L|, the direction the shadow edge of an overhead caster actually runs.
inline float pcssVisibility(glm::vec2 uv, float receiverDistance, const void* map, ShadowDepthAt depthAt,
                            float uvPerMetre, const PcssParams& p) {
    const float facing = std::sqrt(std::max(0.0f, 1.0f - p.receiverSlope * p.receiverSlope));
    const float scale = std::max(uvPerMetre, 1e-12f) * std::max(facing, 1e-3f);
    auto sampleAt = [&](glm::vec2 metres) {
        return depthAt(map, uv + metres * scale);   // <= 0 means sky: nothing to occlude, and the tap is lit
    };
    // A tap's own surface is `receiverSlope * |offset|` off the receiver's depth; a texel nearer than that is this
    // surface, not something in front of it.
    auto sameSurface = [&](float d, glm::vec2 offset) {
        const float slack = p.bias + p.receiverSlope * glm::length(offset);
        return d > 0.0f && d >= receiverDistance - slack;
    };

    if (p.mode == 0) {
        const glm::vec2 zero(0.0f);
        const float d = sampleAt(zero);
        return (d <= 0.0f || sameSurface(d, zero)) ? 1.0f : 0.0f;
    }

    if (p.mode == 1) {
        int lit = 0;
        for (int i = 0; i < p.pcfSamples; ++i) {
            const glm::vec2 offset = vogelTap(i, p.pcfSamples) * p.fixedRadius;
            const float d = sampleAt(offset);
            if (d <= 0.0f || sameSurface(d, offset)) ++lit;
        }
        return float(lit) / float(std::max(p.pcfSamples, 1));
    }

    // 1. Blockers: the average depth of the texels that are in front of the receiver's own surface.
    float sum = 0.0f;
    int found = 0;
    for (int i = 0; i < p.blockerSamples; ++i) {
        const glm::vec2 offset = vogelTap(i, p.blockerSamples) * p.searchRadius;
        const float d = sampleAt(offset);
        if (d > 0.0f && !sameSurface(d, offset)) {
            sum += d;
            ++found;
        }
    }
    if (found == 0) return 1.0f;   // nothing between this point and the light
    const float blockerDistance = sum / float(found);

    // 2. The penumbra that blocker casts here, filtered with the same disc.
    const float width = penumbraWidth(p, blockerDistance, receiverDistance);
    if (width <= 1e-5f) {
        const glm::vec2 zero(0.0f);
        const float d = sampleAt(zero);
        return (d <= 0.0f || sameSurface(d, zero)) ? 1.0f : 0.0f;
    }
    int lit = 0;
    for (int i = 0; i < p.pcfSamples; ++i) {
        const glm::vec2 offset = vogelTap(i, p.pcfSamples) * width;
        const float d = sampleAt(offset);
        if (d <= 0.0f || sameSurface(d, offset)) ++lit;
    }
    return float(lit) / float(std::max(p.pcfSamples, 1));
}

// The shadow map a preview or a test hands in: a square grid of depths in metres, 0 = nothing. Bilinear-free: PCSS
// reads texels, and a nearest texel is what the GPU's comparison sampler would give for a filtered tap too.
struct PcssDepthMap {
    int size = 0;
    const float* depth = nullptr;
};

inline float pcssMapDepth(const void* map, glm::vec2 uv) {
    const auto* m = static_cast<const PcssDepthMap*>(map);
    if (!m || !m->depth || m->size <= 0) return 0.0f;
    if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) return 0.0f;
    const int x = std::min(int(uv.x * float(m->size)), m->size - 1);
    const int y = std::min(int(uv.y * float(m->size)), m->size - 1);
    return m->depth[size_t(y) * size_t(m->size) + size_t(x)];
}

} // namespace ukiyo
