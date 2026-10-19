// Real-time diffuse GI: the accumulation pass.
//
// The trace pass fires four to eight rays per half-resolution pixel. That many rays are noisy, but the light does not
// move and the camera moves slowly, so the samples are accumulated over frames: an exponential average with a weight
// that starts at 1 and settles at 1/16 — sixteen frames of samples for a fifth of the cost of tracing them all.
//
// The history is reprojected with the engine's screen-space velocity buffer (shaders/velocity.frag writes current
// minus previous UV), so a pan rotates the old samples with the scene instead of smearing them. Where the reprojected
// depth disagrees with this pixel's, the history belongs to another surface: the pixel falls back to the new sample.
//
// Inputs:  uTex0 this frame's trace, uTex1 last frame's accumulator, uDepth scene depth, uVel screen-space velocity.
// Output:  the accumulated half-resolution GI (the delta written by gi_trace.frag).
// pc.p0.x = blend weight for the new sample (1 resets).
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
TEX(0, 2) uniform sampler2D uTex1;
TEX(0, 4) uniform sampler2D uDepth;
TEX(0, 5) uniform sampler2D uVel;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 cur = texture(uTex0, vUV);
    vec2 prevUV = vUV - texture(uVel, vUV).xy;
    if (pc.p0.x >= 0.999 || any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        outColor = cur;
        return;
    }
    // A different surface behind this pixel: the depth the history was accumulated at is not this pixel's depth.
    const float d = texture(uDepth, vUV).r;
    const float dp = texture(uDepth, prevUV).r;
    if (abs(dp - d) > 0.01) {
        outColor = cur;
        return;
    }
    outColor = mix(texture(uTex1, prevUV), cur, max(pc.p0.x, 1.0 / 16.0));
    outColor.a = 1.0;
}
