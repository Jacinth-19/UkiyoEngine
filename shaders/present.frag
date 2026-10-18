// Copies the final image to the presentation target. Bilinear sampling makes
// 2x supersampled images downsample with an exact 2x2 box filter.
// p0.x = 1 when presenting to a Vulkan swapchain (flip Y).
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 uv = vUV;
    if (pc.p0.x > 0.5) uv.y = 1.0 - uv.y;
    outColor = vec4(texture(uTex0, uv).rgb, 1.0);
}
