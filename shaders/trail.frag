// Trail ribbon colour: the gradient's colour and alpha, softened towards both edges of the strip.
//
// p0.x = edge softness (0 keeps the ribbon's edges crisp; 1 fades half the width on each side)
// p0.y = opacity multiplier (a global effects-intensity slider)
// A trail is untextured on purpose: the look comes from the gradient along the strip and the soft edge across it, so
// one draw needs no material and no texture binding. A caller that wants a texture can bind one at set 0 binding 1
// (the frame set already carries a sampler there) without changing the geometry.
#include "common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float alpha = vColor.a * pc.p0.y;
    const float soft = pc.p0.x;
    if (soft > 0.0) {
        // v runs 0..1 across the strip. smoothstep never gets a reversed pair of edges (undefined in GLSL), so the far
        // side is the one's complement rather than an inverted call.
        const float half = soft * 0.5;
        alpha *= smoothstep(0.0, half, vUV.y) * (1.0 - smoothstep(1.0 - half, 1.0, vUV.y));
    }
    if (alpha <= 0.002) discard;   // a fully faded tail leaves no depth or blending work behind it
    outColor = vec4(vColor.rgb, alpha);
}
