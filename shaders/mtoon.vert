// Skinned or static mesh vertex shader (VRM / MToon materials).
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
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
    vWorldPos = wp.xyz;
    vNormal = mat3(M) * n;
    vUV = inUV;
    gl_Position = frame.viewProj * wp;
}
