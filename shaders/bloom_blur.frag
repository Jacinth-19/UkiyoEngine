// 9-tap separable Gaussian, from Kaze's UnityGenshinPostProcessing (GaussianBlur).
// p0.xy = direction * spacing * radius, in texels (spacing 2 for the downsampling step of each level).
// p1.xy = 1 / source size.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float kOff[9] = float[9](-4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0);
const float kW[9] = float[9](0.01621622, 0.05405405, 0.12162162, 0.19459459, 0.22702703,
                             0.19459459, 0.12162162, 0.05405405, 0.01621622);

void main() {
    vec2 off = pc.p0.xy * pc.p1.xy;
    vec3 col = vec3(0.0);
    for (int i = 0; i < 9; ++i) col += kW[i] * texture(uTex0, vUV + kOff[i] * off).rgb;
    outColor = vec4(col, 1.0);
}
