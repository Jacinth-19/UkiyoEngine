#include "common.glsl"
#include "fade_discard.glsl"
#include "frame.glsl"
#include "material.glsl"
layout(location = 0) out vec4 outColor;
float dashHash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
    distanceFadeDiscard();
    // RealToon outline noise (sketch): drop 3 px cells at random so the hull reads as a dashed pen line.
    // frame.toonC.z is the project amount (0 = solid). Fraction kept = 1 - 0.6 * amount.
    float amt = frame.toonC.z;
    if (amt > 0.0 && dashHash(floor(gl_FragCoord.xy / 3.0)) > 1.0 - 0.6 * amt) discard;
    outColor = vec4(mtl.outline.rgb, 1.0);
}
