// MMD-style edge: inverted hull with a fixed width (the effect's Edge_VS_NoTessellation,
// position += normal * EdgeWeight * EdgeWidth * 0.015). Here EdgeWeight = 1, and the
// width is kMmdEdgeWidth metres scaled by the global outline scale in p0.y.
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
#include "material.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
const float kMmdEdgeWidth = 0.0015;
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
    wp.xyz += wn * kMmdEdgeWidth * pc.p0.y;
    gl_Position = frame.viewProj * wp;
}
