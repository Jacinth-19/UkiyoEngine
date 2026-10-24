// Grass shading (item 19). A toon ramp with the same edges the terrain uses, so blades and ground agree in the sun;
// a root-to-tip gradient with fake ambient occlusion at the root; and sun through the blade (translucency), which is
// what makes a field read as grass rather than as painted cards.
//
// The far edge of the last distance band is dithered rather than cut, matching the reference's dither LOD: the blade
// stipple fades the field into the ground instead of drawing a circle around the camera.
#include "common.glsl"
#include "frame.glsl"
#include "shadow_sample.glsl"

UBO(1, 15) GrassRows {
    vec4 row[3 * 256];
    vec4 base;   // rgb root colour, a tip darkening
    vec4 tip;    // rgb tip colour, a translucency
} grassBlock;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vN;
layout(location = 2) in float vHeight;
layout(location = 3) in float vFade;
layout(location = 4) in float vRand;

layout(location = 0) out vec4 outColor;

// 4 by 4 ordered dither in [0, 1). Flat array, so no dynamic matrix indexing.
float bayer4(vec2 p) {
    const float m[16] = float[16](0.0, 8.0, 2.0, 10.0,
                                  12.0, 4.0, 14.0, 6.0,
                                  3.0, 11.0, 1.0, 9.0,
                                  15.0, 7.0, 13.0, 5.0);
    ivec2 c = ivec2(mod(p, 4.0));
    return (m[(c.y & 3) * 4 + (c.x & 3)] + 0.5) / 16.0;
}

void main() {
    if (vFade < bayer4(gl_FragCoord.xy)) discard;

    vec3 n = normalize(vN);
    vec3 L = normalize(frame.sunDir.xyz);
    float ndl = max(dot(n, L), 0.0);
    // The terrain's ramp, a little wider: a blade's normal is mostly up, and a hard edge on every blade reads as noise.
    float ramp = smoothstep(0.08, 0.52, ndl);
    // Hills, trees and the character cut the sun, so a field under a ridge is lit by the sky alone.
    float shadow = shadowFactor(vWorld, n);
    float sun = ramp * shadow;
    vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * n.y);

    float h = clamp(vHeight, 0.0, 1.0);
    vec3 albedo = mix(grassBlock.base.rgb, grassBlock.tip.rgb, smoothstep(0.05, 0.9, h));
    albedo *= mix(1.0, 1.0 - grassBlock.base.w, pow(1.0 - h, 1.5));   // the root sits in its own shadow
    albedo *= 0.88 + 0.24 * vRand;                                   // no two blades the same green

    // Sun through the tip. Strongest when looking towards the light and when the blade is thin, which is the tip.
    vec3 V = normalize(frame.camPos.xyz - vWorld);
    float back = pow(max(dot(-V, L), 0.0), 2.0);
    float through = grassBlock.tip.w * back * h * h;

    vec3 colour = albedo * (hemi + frame.sunColor.rgb * sun) +
                  grassBlock.tip.rgb * frame.sunColor.rgb * through * 0.4 * shadow;
    outColor = vec4(colour, 1.0);
}
