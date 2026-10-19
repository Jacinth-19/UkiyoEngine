// Real-time diffuse GI: the accumulation pass.
//
// The trace pass fires eight rays per half-resolution pixel. Eight rays are noisy, but the camera moves slowly and
// the light does not move at all, so the samples are accumulated over frames: an exponential average with a weight
// that starts at 1 and settles at 1/16, which is sixteen frames of samples for a fifth of the cost of baking them.
// A depth-discontinuity test throws the history away where the surface moved, so a cut or a fast pan does not smear.
//
// Inputs:  uTex0 this frame's trace, uTex1 last frame's accumulator, uDepth the *full-resolution* scene depth.
// Output:  the accumulated half-resolution GI.
// pc.p0.x = blend weight for the new sample (1 resets), pc.p1.xy = 1 / full resolution.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
TEX(0, 2) uniform sampler2D uTex1;
TEX(0, 4) uniform sampler2D uDepth;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 cur = texture(uTex0, vUV);
    if (pc.p0.x >= 0.999) { outColor = cur; return; }
    float d = texture(uDepth, vUV).r;
    // Where the depth moved, the history belongs to another surface. Four taps around the pixel, so a one-pixel
    // silhouette does not poison the accumulation for sixteen frames.
    float diff = 0.0;
    vec2 px = pc.p1.xy;
    diff += step(0.002, abs(texture(uDepth, vUV + vec2(px.x, 0.0)).r - d));
    diff += step(0.002, abs(texture(uDepth, vUV - vec2(px.x, 0.0)).r - d));
    diff += step(0.002, abs(texture(uDepth, vUV + vec2(0.0, px.y)).r - d));
    diff += step(0.002, abs(texture(uDepth, vUV - vec2(0.0, px.y)).r - d));
    float weight = mix(max(pc.p0.x, 1.0 / 16.0), 1.0, clamp(diff / 4.0, 0.0, 1.0));
    vec4 prev = texture(uTex1, vUV);
    outColor = mix(prev, cur, weight);
    outColor.a = 1.0;
}
