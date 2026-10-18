// Copies the scene before the water into the half-size refraction target (item 16). rgb = colour, a = linear view
// depth in metres, so the water pass never samples the depth buffer it is writing.
#include "common.glsl"
#include "frame.glsl"

TEX(0, 1) uniform sampler2D uHdr;
TEX(0, 4) uniform sampler2D uDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float d = texture(uDepth, vUV).r;
    // Perspective depth to view distance. For a projection with [2][2] = A and [3][2] = B, z = B / (d + A).
    float A = frame.proj[2][2];
    float B = frame.proj[3][2];
    float viewZ = clamp(B / (d + A), 0.0, 1000.0);
    outColor = vec4(texture(uHdr, vUV).rgb, viewZ);
}
