// Soft round particle. Alpha falls off from the centre of the quad.
#include "common.glsl"

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float d = length(vCorner);
    float edge = 1.0 - smoothstep(0.6, 1.0, d);
    if (edge <= 0.0) discard;
    outColor = vec4(vColor.rgb, vColor.a * edge);
}
