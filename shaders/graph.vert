// Vertex stage for generated (Shader Graph) materials. Same inputs as mtoon.vert,
// same skinning; outputs the names the generated fragment shader reads.
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in vec4 inColor;   // COLOR_0 (white when the mesh has none)
layout(location = 6) in vec2 inUV1;     // TEXCOORD_1 (copy of inUV when the mesh has none)
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_world;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec2 v_uv1;
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
    v_normal = mat3(M) * n;
    v_world = wp.xyz;
    v_uv = inUV;
    v_color = inColor;
    v_uv1 = inUV1;
    gl_Position = frame.viewProj * wp;
}
