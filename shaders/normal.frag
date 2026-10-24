// Two targets, in one pass:
//   location 0: the view-space normal, encoded to 0..1. Alpha 1 marks an opaque surface.
//   location 1: the surface base colour, linear. Alpha 1 marks that this pixel has an albedo; the multi-bounce AO
//               approximation in effects.frag reads it and falls back to single-bounce AO where alpha is 0 (terrain,
//               foliage, grass and graph materials are drawn outside this pre-pass).
// The albedo is sampled at vUV rather than through toonBaseUV: the eye parallax shifts a few pixels of an iris, and an
// occlusion bounce does not need that precision.
#include "common.glsl"
#include "material.glsl"
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outAlbedo;
void main() {
    vec3 n = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    outNormal = vec4(n * 0.5 + 0.5, 1.0);
    outAlbedo = vec4(mtl.baseColor.rgb * texture(uBaseTex, vUV).rgb, 1.0);
}
