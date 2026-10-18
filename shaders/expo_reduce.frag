// Auto exposure. Mode 0 measures the log-average luminance of the frame on a 16x16 grid, then moves the
// exposure multiplier towards key / average (clamped to 0.25..4) at the given speed. Mode 1 copies the
// 1x1 input. The multiplier is stored as log2 in 8 bits: stored = (log2(m) + 2) / 4, so 0.5 means 1.0.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uIn;     // mode 0: the frame; mode 1: the 1x1 state to copy
TEX(0, 2) uniform sampler2D uPrev;   // the previous state (mode 0)
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    if (pc.p0.w > 0.5) {
        outColor = texture(uIn, vec2(0.5));
        return;
    }
    float sum = 0.0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / 16.0;
            sum += log(max(dot(texture(uIn, uv).rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
        }
    }
    float avg = exp(sum / 256.0);
    float prevM = exp2(texture(uPrev, vec2(0.5)).r * 4.0 - 2.0);
    float target = clamp(pc.p0.x / avg, 0.25, 4.0);
    float m = pc.p0.y > 0.5 ? mix(prevM, target, pc.p0.z) : 1.0;
    outColor = vec4(clamp((log2(m) + 2.0) / 4.0, 0.0, 1.0), 0.0, 0.0, 1.0);
}
