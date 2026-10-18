// Kaze's bloom upsample: the weighted sum of the four blurred levels, then the glow tint.
// p0 = level weights (levels 0..3, finest first; Kaze default 0.25 each).
// model[0].rgb = glow tint, model[0].w = 1 when the tint is on (identity leaves it white).
// The intensity is applied in tonemap (bloomStrength), as Kaze's ColorGrading does.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uL0;
TEX(0, 2) uniform sampler2D uL1;
TEX(0, 3) uniform sampler2D uL2;
TEX(0, 4) uniform sampler2D uL3;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 col = texture(uL0, vUV).rgb * pc.p0.x
             + texture(uL1, vUV).rgb * pc.p0.y
             + texture(uL2, vUV).rgb * pc.p0.z
             + texture(uL3, vUV).rgb * pc.p0.w;
    if (pc.model[0].w > 0.5) col *= max(pc.model[0].rgb, vec3(0.0));
    outColor = vec4(col, 1.0);
}
