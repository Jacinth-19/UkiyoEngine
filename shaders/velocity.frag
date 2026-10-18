// Writes the screen-space motion of this pixel's surface, in UV units: current minus previous.
// TAA reads it as prevUV = uv - velocity.
#include "common.glsl"
layout(location = 0) in vec4 vCur;
layout(location = 1) in vec4 vPrev;
layout(location = 0) out vec4 outVel;
void main() {
    vec2 c = vCur.xy / vCur.w;
    vec2 p = vPrev.xy / vPrev.w;
    outVel = vec4((c - p) * 0.5, 0.0, 1.0);
}
