// Alpha-masked materials punch holes in the shadow map.
#include "common.glsl"
#include "fade_discard.glsl"
#include "material.glsl"
layout(location = 0) in vec2 vUV;
void main() {
    distanceFadeDiscard();
    int mode = int(mtl.flags.x + 0.5);
    if (mode == 1) {
        float a = texture(uBaseTex, vUV).a * mtl.baseColor.a;
        if (a < mtl.flags.y) discard;
    }
}
