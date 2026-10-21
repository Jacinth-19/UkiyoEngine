// HUD fragment: one glyph atlas, tinted per vertex. Coverage lives in alpha, so solid fills sample the reserved white
// texel at (0,0) of the page and come out as their own colour at full alpha.
#include "common.glsl"
TEX(0, 0) uniform sampler2D uAtlas;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() {
    float coverage = texture(uAtlas, vUV).a;
    outColor = vec4(vColor.rgb, vColor.a * coverage);
}
