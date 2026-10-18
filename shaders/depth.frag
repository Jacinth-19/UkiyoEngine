// Depth prepass for the toon materials. It runs the same discards, in the same order, as the start of mtoon.frag
// and mmdtoon.frag (dither fade, distance fade, character dissolve, alpha cutout), so the depth it writes matches
// the colour pass exactly. Keep this block in step with those two shaders.
#include "common.glsl"
#include "fade_discard.glsl"
#include "frame.glsl"
#include "material.glsl"
#include "toon_extras.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

void main() {
    toonDitherFade(vWorldPos);
    distanceFadeDiscard();
    toonCharacterDiscard(vWorldPos);
    vec3 N0 = gl_FrontFacing ? normalize(vNormal) : -normalize(vNormal);
    vec4 tex = texture(uBaseTex, toonBaseUV(vUV, vWorldPos, N0, normalize(frame.camPos.xyz - vWorldPos)));
    vec4 base = mtl.baseColor * tex;
    int mode = int(mtl.flags.x + 0.5);
    if (mode == 1 && base.a < mtl.flags.y) discard;
}
