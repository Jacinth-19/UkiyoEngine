// Ambient occlusion, and the multi-bounce approximation Godot 4.8 added (GH-115426).
//
// A single-bounce AO term says "this point sees 40 % of the sky", and multiplying the ambient light by 0.4 is only right
// for a surface that reflects nothing. Real corners are brighter than that, because the occluded hemisphere is filled by
// light that bounced off the very surfaces doing the occluding. The cheap fix — from *Practical Real-Time Strategies for
// Accurate Indirect Occlusion* (Activision, SIGGRAPH 2016), and what Godot 4.8 ships — is a cubic in AO whose
// coefficients depend on the surface albedo:
//
//     a = 2.0404 · albedo − 0.3324
//     b = −4.7951 · albedo + 0.6417
//     c = 2.7552 · albedo + 0.6903
//     visibility = max(ao, ((ao·a + b)·ao + c)·ao)
//
// Two things fall out of it, and both are the point: a bright surface keeps more of its ambient light in a corner than a
// dark one, and a *coloured* surface gives back light of its own colour, so a saturated wall no longer reads as grey
// where it meets the floor. At ao = 1 the expression is 0.0005·albedo + 0.9996, i.e. 1 within the fit's own error.
//
// One difference from Godot: the fit is valid for the *ambient share* of a forward-shaded surface, where it can exceed 1
// safely because the surface also has direct light. This engine applies AO to the composed frame in a post pass, and
// there a factor above 1 would brighten the direct light too. So the result is clamped into [ao, 1]: the approximation
// may only restore light the single-bounce term removed, never add light the frame did not have. The fit never dips
// below `ao` on its own — Godot's `max` already guarantees that — so in practice the upper clamp is the only one that
// ever fires. The clamp is a deliberate, documented difference, and `tests/ao_test.cpp` checks that it is the only one.
//
// This file is the CPU definition, used by tests/ao_test.cpp and tools/ao_preview.cpp; it is the same arithmetic
// shaders/effects.frag performs per fragment. Keeping it here is what makes the curve checkable without a GPU.
#pragma once

#include <glm/glm.hpp>

namespace ukiyo {

// The curve's constants, named so a test can cite them.
struct MultiBounceFit {
    static constexpr float kA = 2.0404f;
    static constexpr float kB = -4.7951f;
    static constexpr float kC = 2.7552f;
    static constexpr float kD = -0.3324f;
    static constexpr float kE = 0.6417f;
    static constexpr float kF = 0.6903f;
};

// The per-channel coefficients for one albedo.
inline glm::vec3 multiBounceACoefficient(const glm::vec3& albedo) {
    return MultiBounceFit::kA * albedo + glm::vec3(MultiBounceFit::kD);
}
inline glm::vec3 multiBounceBCoefficient(const glm::vec3& albedo) {
    return MultiBounceFit::kB * albedo + glm::vec3(MultiBounceFit::kE);
}
inline glm::vec3 multiBounceCCoefficient(const glm::vec3& albedo) {
    return MultiBounceFit::kC * albedo + glm::vec3(MultiBounceFit::kF);
}

// The visibility to apply to ambient light. `ao` is 1 for unoccluded, 0 for fully occluded. The result is clamped to
// [ao, 1]: the approximation may only restore light, never remove more than the single-bounce term asked for, and never
// add above unoccluded. `albedo` is the linear base colour (0..1).
inline glm::vec3 multiBounceVisibility(float ao, const glm::vec3& albedo) {
    const float x = glm::clamp(ao, 0.0f, 1.0f);
    const glm::vec3 alb = glm::clamp(albedo, glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec3 v = ((x * multiBounceACoefficient(alb) + multiBounceBCoefficient(alb)) * x +
                         multiBounceCCoefficient(alb)) *
                        x;
    return glm::clamp(v, glm::vec3(x), glm::vec3(1.0f));
}

// The scalar case, for a caller with no colour (and the value the shader uses where the albedo buffer has no surface).
inline float multiBounceVisibility(float ao, float albedo) {
    return multiBounceVisibility(ao, glm::vec3(albedo)).x;
}

// The single-bounce term the engine applied before this: `ambient * ao`. Kept so the renderer can switch, and so the
// preview can show the two side by side.
inline float singleBounceVisibility(float ao) { return glm::clamp(ao, 0.0f, 1.0f); }

} // namespace ukiyo
