// Inverted-hull outline (MToon outline, world or screen-space width).
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
#include "material.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
void main() {
    vec4 p = vec4(inPos, 1.0);
    vec3 n = inNormal;
    mat4 M = pc.model;
    if (pc.p0.x > 0.5) {
        mat4 S = inWeights.x * skin.joints[inJoints.x]
               + inWeights.y * skin.joints[inJoints.y]
               + inWeights.z * skin.joints[inJoints.z]
               + inWeights.w * skin.joints[inJoints.w];
        p = S * p;
        n = mat3(S) * n;
        M = mat4(1.0);
    }
    vec4 wp = M * p;
    vec3 wn = normalize(mat3(M) * n);
    float w = mtl.outline.w * pc.p0.y;               // p0.y = global outline scale
    int mode = int(mtl.flags2.x + 0.5);
    if (mode == 2) {
        float dist = length(frame.camPos.xyz - wp.xyz);
        w *= dist * 2.0 / frame.proj[1][1];         // width as a fraction of screen height
    }
    wp.xyz += wn * w;
    gl_Position = frame.viewProj * wp;
}
