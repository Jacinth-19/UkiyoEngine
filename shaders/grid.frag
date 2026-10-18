// Infinite ground: the view ray is intersected analytically with the y=0 plane,
// so the floor reaches the horizon without any geometry. Anti-aliased lines use
// screen-space derivatives. Receives cascaded shadows from the scene.
// Technique: Ben Golus, "The Best Darn Grid Shader (Yet)" (2022), re-implemented.
#include "common.glsl"
#include "frame.glsl"
#include "shadow_sample.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float gridLine(vec2 coord, float scale, float px) {
    vec2 c = coord / scale;
    vec2 d = max(fwidth(c), vec2(1e-6));
    vec2 g = abs(fract(c - 0.5) - 0.5) / d;      // distance to nearest line in pixels
    float l = min(g.x, g.y);
    return 1.0 - clamp(l - px * 0.5, 0.0, 1.0);
}

void main() {
    vec2 ndc = gl_FragCoord.xy / frame.screenParams.xy * 2.0 - 1.0;
    vec4 nearP = frame.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farP  = frame.invViewProj * vec4(ndc, 1.0, 1.0);
    nearP.xyz /= nearP.w;
    farP.xyz /= farP.w;
    vec3 ro = nearP.xyz;
    vec3 rd = normalize(farP.xyz - nearP.xyz);
    if (abs(rd.y) < 1e-5) discard;
    float t = -ro.y / rd.y;
    if (t <= 0.0) discard;
    vec3 P = ro + rd * t;

    vec4 clip = frame.viewProj * vec4(P, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);

    float dist = length(P.xz - frame.camPos.xz);
    float fade = 1.0 - smoothstep(60.0, 220.0, dist);

    vec2 d = max(fwidth(P.xz), vec2(1e-6));
    float minor = gridLine(P.xz, 1.0, 1.1);
    float major = gridLine(P.xz, 10.0, 1.5);
    float axisX = 1.0 - clamp(abs(P.z) / d.y - 0.9, 0.0, 1.0);   // red line along X
    float axisZ = 1.0 - clamp(abs(P.x) / d.x - 0.9, 0.0, 1.0);   // blue line along Z

    vec3 floorCol = vec3(0.56, 0.61, 0.71);
    vec3 lineCol = vec3(0.22, 0.27, 0.40);
    {
        vec3 col = floorCol;
        col = mix(col, lineCol, minor * 0.55 * fade);
        col = mix(col, vec3(0.30, 0.35, 0.50), major * 0.65 * fade);
        col = mix(col, vec3(0.92, 0.32, 0.40), axisX * 0.9 * fade);
        col = mix(col, vec3(0.30, 0.50, 0.98), axisZ * 0.9 * fade);

        float sh = shadowFactor(P, vec3(0.0, 1.0, 0.0));
        col *= mix(0.62, 1.0, sh);                       // cast shadows on the floor

        // Atmospheric fade into the sky horizon colour.
        vec3 horizon = vec3(0.74, 0.84, 0.96);
        col = mix(col, horizon, smoothstep(90.0, 260.0, dist));
        outColor = vec4(col, 0.0);   // alpha 0: environment (character mask)
    }
}
