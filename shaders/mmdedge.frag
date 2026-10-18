// Edge colour for the MMD-style toon look (the effect's Edge_PS: a flat edge colour).
#include "common.glsl"
#include "fade_discard.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    distanceFadeDiscard();
    outColor = vec4(0.06, 0.04, 0.05, 1.0);
}
