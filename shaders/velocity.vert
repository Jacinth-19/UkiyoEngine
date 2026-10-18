// Per-object motion vectors. Positions use the same transform as mtoon.vert, so depths match the
// main pass exactly. The previous-frame transform is in set 3 (MotionBlock): the previous joint
// palette for skinned draws, or the previous model matrix in joints[0] for rigid draws.
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
UBO(3, 10) MotionBlock {
    mat4 joints[128];    // previous frame (see above)
    mat4 prevViewProj;   // previous frame, unjittered
} motion;
layout(location = 0) in vec3 inPos;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 0) out vec4 vCur;    // current clip position (jittered, same as the main pass)
layout(location = 1) out vec4 vPrev;   // previous clip position (unjittered)
void main() {
    vec4 pr = vec4(inPos, 1.0);
    vec4 cur = pc.model * pr;
    vec4 prev = motion.joints[0] * pr;
    if (pc.p0.x > 0.5) {
        mat4 S = inWeights.x * skin.joints[inJoints.x]
               + inWeights.y * skin.joints[inJoints.y]
               + inWeights.z * skin.joints[inJoints.z]
               + inWeights.w * skin.joints[inJoints.w];
        mat4 SP = inWeights.x * motion.joints[inJoints.x]
                + inWeights.y * motion.joints[inJoints.y]
                + inWeights.z * motion.joints[inJoints.z]
                + inWeights.w * motion.joints[inJoints.w];
        cur = S * pr;
        prev = SP * pr;
    }
    vCur = frame.viewProj * cur;
    vPrev = motion.prevViewProj * prev;
    gl_Position = vCur;
}
