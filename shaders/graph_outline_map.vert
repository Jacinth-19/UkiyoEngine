// Inverted-hull outline for generated (Shader Graph) materials. Same inputs and skinning as graph.vert.
// Width and offsets follow Kaze's GetOutlinePosition / GetOutlineWidth (see research/kaze_toon_gap.md).
// Material slots (set 1, binding 15):
//   o0.rgb outline colour; o1.x base width (world units); o2.x alpha (0 = off)
//   o8.xy width range in Kaze's fov-corrected view depth; o9.xy width factors at the range's near and far end
//   o10.xy screen offset (clip units, Kaze's _ScreenOffset); o11.x Z offset (0.01 per step, away from the camera)
//   o12.x 1 = scale the width by COLOR_0.a; o13.x 1 = use the smooth normal; o14.x 1 = Kaze's view-plane extrusion
//   (0 = the original world-normal extrusion, the default).
#include "common.glsl"
#include "frame.glsl"
#include "skin.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in vec4 inColor;    // COLOR_0
layout(location = 7) in vec4 inSmooth;   // smooth normal (xyz), zero = none
layout(location = 0) out vec2 v_uv;

layout(set = 1, binding = 15, std140) uniform GraphOutlineMaterial {
    vec4 o0;
    vec4 o1;
    vec4 o2;
    vec4 o3;   // outlineMapColor0..4 (five-colour variant)
    vec4 o4;
    vec4 o5;
    vec4 o6;
    vec4 o7;
    vec4 o8;
    vec4 o9;
    vec4 o10;
    vec4 o11;
    vec4 o12;
    vec4 o13;
    vec4 o14;
};

void main() {
    v_uv = inUV;
    vec4 p = vec4(inPos, 1.0);
    bool useSmooth = o13.x > 0.5 && dot(inSmooth.xyz, inSmooth.xyz) > 0.25;
    vec3 n = useSmooth ? inSmooth.xyz : inNormal;
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

    // Kaze's width: the view depth (fov-corrected) picks a point in the width range; the factor scales the base width.
    vec4 vp = frame.view * wp;
    float fovFactor = 2.414 / max(abs(frame.proj[1][1]), 1e-4);
    float zk = abs(vp.z * fovFactor);
    float span = o8.y - o8.x;
    float k = span > 1e-4 ? clamp((zk - o8.x) / span, 0.0, 1.0) : 0.0;
    float factor = span > 1e-4 ? mix(o9.x, o9.y, k) : 1.0;
    float vertexAlpha = o12.x > 0.5 ? inColor.a : 1.0;
    float width = o1.x * factor * vertexAlpha;

    vec3 pVS;
    if (o14.x > 0.5) {
        // Kaze: extrude in view space along the normal's view-plane direction.
        pVS = vp.xyz;
        float zl = length(pVS);
        if (zl > 1e-5) pVS += (0.01 * o11.x) * (pVS / zl);
        vec3 nVS = mat3(frame.view) * wn;
        float nl = length(nVS.xy);
        if (nl > 1e-5) pVS += width * vec3(nVS.xy / nl, 0.0);
    } else {
        // Original: extrude in world space along the world normal.
        wp.xyz += wn * width;
        pVS = (frame.view * wp).xyz;
        float zl = length(pVS);
        if (zl > 1e-5) pVS += (0.01 * o11.x) * (pVS / zl);
    }
    gl_Position = frame.proj * vec4(pVS, 1.0);
    gl_Position.xy += o10.xy * gl_Position.w;
}
