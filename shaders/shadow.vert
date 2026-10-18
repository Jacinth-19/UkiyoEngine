// Depth-only shadow caster. p0.x = skinned flag, p0.y = cascade index.
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 0) out vec2 vUV;
void main() {
    vec4 p = vec4(inPos, 1.0);
    mat4 M = pc.model;
    if (pc.p0.x > 0.5) {
        mat4 S = inWeights.x * skin.joints[inJoints.x]
               + inWeights.y * skin.joints[inJoints.y]
               + inWeights.z * skin.joints[inJoints.z]
               + inWeights.w * skin.joints[inJoints.w];
        p = S * p;
        M = mat4(1.0);
    }
    int cascade = int(pc.p0.y + 0.5);
    vUV = inUV;
    gl_Position = frame.shadowMatrix[cascade] * (M * p);
}
