// Normal pre-pass: view-space normals for the screen-space effects. Same transform as velocity.vert,
// so the depths match the main pass exactly.
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 0) out vec3 vNormal;   // view space
void main() {
    vec4 pr = vec4(inPos, 1.0);
    vec4 cur = pc.model * pr;
    mat3 nm = mat3(pc.model);
    if (pc.p0.x > 0.5) {
        mat4 S = inWeights.x * skin.joints[inJoints.x]
               + inWeights.y * skin.joints[inJoints.y]
               + inWeights.z * skin.joints[inJoints.z]
               + inWeights.w * skin.joints[inJoints.w];
        cur = S * pr;
        nm = mat3(S);
    }
    vNormal = mat3(frame.view) * (nm * inNormal);
    gl_Position = frame.viewProj * cur;
}
