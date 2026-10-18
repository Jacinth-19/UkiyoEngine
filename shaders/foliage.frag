// Foliage (item 18, step 3). A flat tint lit like the terrain: the same toon ramp, with a fixed edge, and the sky and
// ground ambient. The tint is in the push constant p1.rgb. No textures yet, and no shadows.
#include "common.glsl"
#include "frame.glsl"

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vN;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(vN);
    vec3 L = normalize(frame.sunDir.xyz);
    float ndl = max(dot(n, L), 0.0);
    float ramp = smoothstep(0.15 - 0.2, 0.15 + 0.2, ndl);
    vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * n.y);
    outColor = vec4(pc.p1.rgb * (hemi + frame.sunColor.rgb * ramp), 1.0);
}
