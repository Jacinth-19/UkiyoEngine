// Screen-space contact shadows (Godot 4.8's GH-118045): the short shadows a shadow map cannot resolve.
//
// A cascaded shadow map answers "is this point lit?" at its own resolution. Near where two surfaces meet — a crate on a
// floor, a foot on the ground — the map's texels are much bigger than the gap, so the contact darkening either leaks or
// is lost, and the object looks like it is floating. A screen-space pass fixes exactly that scale: from the visible
// surface, march a few centimetres towards the sun through the depth buffer, and if the ray passes behind geometry that
// is only a hand's width in front of it, the point is in contact shadow.
//
// This is the Bend Studio approach Godot adopted ("Inside Bend: Screen Space Shadows"), evaluated per pixel in
// shaders/effects.frag rather than in a compute pass over light-space epipolar lines. The per-pixel march is the same
// maths and needs no extra target, no separate resolve and no change to the shadow atlas; it costs `steps` depth
// fetches on the pixels whose normal faces the sun.
//
// This header is the CPU definition of that march, shared by tests/contact_shadow_test.cpp and
// tools/contact_shadow_preview.cpp. `effects.frag` runs the same steps in the same order; the constants live in one
// place here so the two cannot drift apart silently.
#pragma once

#include <cmath>
#include <functional>

#include <glm/glm.hpp>

namespace ukiyo {

struct ContactShadowParams {
    // How far the ray travels, in metres. A contact shadow is a contact-scale effect: 10-40 cm is where a shadow map
    // usually stops being able to help, and anything longer is the map's job (and would smear the ambient occlusion).
    float length = 0.35f;
    // How thick an occluder has to be to count, in metres. Too small and the depth buffer's own precision punches
    // holes in the shadow; too large and a thin wall casts one.
    float thickness = 0.08f;
    // 0 disables the pass; 1 is full darkening.
    float strength = 1.0f;
    // Depth fetches per pixel. 24 is the figure's setting; 12 is enough at 1080p for a still frame, and 48 removes
    // the last of the banding on a grazing surface.
    int steps = 24;
    // Fraction of the screen, at each border, where the effect fades out: the depth buffer has no information past the
    // edge, and a shadow that stops dead at the border reads as a bug.
    float edgeFade = 0.08f;
    // A ray that starts exactly on the surface z-fights with it; this much lift, growing along the ray, keeps the
    // start clean without lengthening the shadow visibly.
    float bias = 0.015f;
};

// Interleaved gradient noise: the per-pixel jitter the pass uses to trade banding for noise, which TAA then resolves.
// Same constants as the shader.
inline float interleavedGradientNoise(const glm::vec2& pixel) {
    return std::fmod(52.9829189f * std::fmod(0.06711056f * pixel.x + 0.00583715f * pixel.y, 1.0f), 1.0f);
}

// Projects a view-space point to UV. Returns false when it is behind the camera.
inline bool projectToUv(const glm::mat4& proj, const glm::vec3& viewPos, glm::vec2* uv) {
    const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
    if (clip.w <= 1e-6f) return false;
    const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
    *uv = ndc * 0.5f + 0.5f;
    return true;
}

// The visibility in [1 - strength, 1] for one pixel.
//
// `viewPos` is the surface point in view space, `viewNormal` its normal, `sunView` the direction *towards* the sun in
// view space. `eyeDepthAtUv` returns the positive view-space depth (distance along -z) of whatever the depth buffer
// holds at that UV; a value of 0 or less means "no geometry", and the march skips that step.
//
// The march is from the surface towards the sun: at each step the ray's own depth is compared with the depth buffer's,
// and a sample that is in front of the ray but behind it by less than `thickness` is an occluder. That is the whole
// test; the rest is the bias, the jitter, the edge fade and the surface-facing early out.
inline float contactShadowVisibility(const glm::vec3& viewPos, const glm::vec3& viewNormal, const glm::vec3& sunView,
                                     const glm::mat4& proj, const glm::vec2& pixel,
                                     const std::function<float(const glm::vec2&)>& eyeDepthAtUv,
                                     const ContactShadowParams& p, const glm::vec2& screenSize) {
    if (p.strength <= 0.0f || p.steps <= 0) return 1.0f;
    const glm::vec3 N = glm::normalize(viewNormal);
    const glm::vec3 L = glm::normalize(sunView);
    // A surface turned away from the sun is already unlit; the shadow map and the material's own N.L decide it, and a
    // contact shadow there would only darken an ambient term it does not belong to.
    if (glm::dot(N, L) <= 0.0f) return 1.0f;

    const float stepLength = p.length / float(p.steps);
    // Start half a step out, plus the pixel's jitter, so the samples of neighbouring pixels do not line up into bands.
    const float jitter = interleavedGradientNoise(pixel);
    float travelled = stepLength * (0.5f + jitter);
    float occluded = 0.0f;

    for (int i = 0; i < p.steps; ++i, travelled += stepLength) {
        const glm::vec3 ray = viewPos + L * travelled;
        glm::vec2 uv;
        if (!projectToUv(proj, ray, &uv)) break;
        if (uv.x <= 0.0f || uv.x >= 1.0f || uv.y <= 0.0f || uv.y >= 1.0f) break;

        const float rayDepth = -ray.z;
        const float sceneDepth = eyeDepthAtUv(uv);
        if (sceneDepth <= 0.0f) continue;   // sky: nothing to hit, and nothing behind the ray either
        // The ray has to be behind the surface the buffer holds, by more than the start bias, and the gap has to be
        // thinner than the accepted occluder thickness.
        const float bias = p.bias * (1.0f + travelled / std::max(p.length, 1e-4f));
        const float gap = rayDepth - sceneDepth;
        if (gap > bias && gap < p.thickness) {
            occluded += 1.0f;
            // One hit is enough for a binary contact shadow, but the shader keeps counting so the strength can be
            // shaped by how much of the ray was blocked.
        }
    }
    float hit = occluded / float(p.steps);
    // Contrast: one hit out of 24 is a ray that grazed something, not a contact. The square root keeps the near edge
    // soft and the deep contact solid.
    hit = std::sqrt(glm::clamp(hit * 4.0f, 0.0f, 1.0f));

    // Fade at the screen border, where the depth buffer runs out and a shadow would stop dead.
    float fade = 1.0f;
    if (p.edgeFade > 0.0f) {
        glm::vec2 suv(0.5f);
        if (!projectToUv(proj, viewPos, &suv)) return 1.0f;
        const float edge = glm::min(glm::min(suv.x, 1.0f - suv.x), glm::min(suv.y, 1.0f - suv.y));
        const float fadePixels = p.edgeFade * glm::max(screenSize.x, screenSize.y);
        fade = glm::clamp(edge * glm::min(screenSize.x, screenSize.y) / glm::max(fadePixels, 1e-4f), 0.0f, 1.0f);
    }
    return 1.0f - glm::clamp(p.strength * hit * fade, 0.0f, 1.0f);
}

} // namespace ukiyo
