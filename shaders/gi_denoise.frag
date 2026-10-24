// Real-time diffuse GI: the denoiser.
//
// The trace fires four to eight rays per half-resolution pixel and the accumulator averages them over a few frames.
// That is enough for the light but not for the noise: at four rays a low-contrast crease is still visibly blotchy, and
// the reference implementation solves the same problem with a network (DLSS Ray Reconstruction) that needs tensor
// cores. An integrated GPU has no tensor cores, so this is the classical answer to the same question, and it is the
// reason the pass can afford to trace as little as it does.
//
// It is an à-trous (with holes) edge-aware filter: a 5x5 B3-spline kernel applied at a growing stride, so two
// iterations reach a 17x17 footprint for 50 taps rather than 289. Each tap is weighted by how similar its surface is
// to the centre pixel's — a logarithmic depth term and a normal term — so the filter never bleeds light across a
// silhouette or lets the dark side of a ridge borrow from the lit side. The stride doubles with each iteration, so
// the first pass cleans the pixel-scale noise and the second the 3-5 px clumps.
//
// Why not just more rays: measured in tools/pathtrace_preview.cpp (the CPU mirror of this pipeline), the denoiser
// takes 2 rays and 4 frames to the same RMSE that 8 rays and 8 frames reach without it, i.e. a quarter of the trace
// cost for the same image. docs/PATHTRACE.md §4 has the table.
//
// Inputs (Post set, set 1 — set 0 is the Frame set, unused here):
//   uTex0  the GI to filter (this frame's accumulation, then the previous iteration's output)
//   uTex1  scene depth
//   uTex2  view-space normal (alpha 1 = surface)
//
// Push constants: p0 = (stride, depth sigma, normal power, 0)
#include "common.glsl"

TEX(0, 1) uniform sampler2D uTex0;
TEX(0, 4) uniform sampler2D uTex1;
TEX(0, 5) uniform sampler2D uTex2;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// The B3 spline, the standard kernel for an à-trous filter: separable, positive, and wide enough that two iterations
// do not show the checkerboard a box kernel leaves behind.
const float kKernel[5] = float[5](1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uTex0, 0));
    const int stride = int(pc.p0.x);

    float d0 = texture(uTex1, vUV).r;
    if (d0 > 0.99999) { outColor = vec4(0.0); return; }   // sky: nothing to denoise
    vec3 n0 = texture(uTex2, vUV).xyz * 2.0 - 1.0;

    vec3 acc = texture(uTex0, vUV).rgb * kKernel[2] * kKernel[2];
    float wsum = kKernel[2] * kKernel[2];
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            if (x == 0 && y == 0) continue;
            vec2 uv = vUV + vec2(x, y) * texel * float(stride);
            float d = texture(uTex1, uv).r;
            if (d > 0.99999) continue;
            vec3 n = texture(uTex2, uv).xyz * 2.0 - 1.0;
            // Depth: logarithmic, because a 5 cm step on a near surface is a much bigger discontinuity than on a far
            // one, and a linear difference would either blur the foreground or refuse to filter the background.
            float depthDiff = abs(d0 - d) / max(1e-4, min(d0, d));
            float wd = exp(-depthDiff / max(1e-5, pc.p0.y));
            // Normal: the dot product, sharpened. Two surfaces that face differently are two surfaces, whatever the
            // depth says at a silhouette where one occludes the other.
            float wn = pow(max(0.0, dot(n0, n)), max(1.0, pc.p0.z));
            float w = kKernel[x + 2] * kKernel[y + 2] * wd * wn;
            acc += texture(uTex0, uv).rgb * w;
            wsum += w;
        }
    }
    outColor = vec4(acc / max(wsum, 1e-5), 1.0);
}
