// Procedural anime sky: gradient, sun disc + halo, cel-shaded cloud layer.
#include "common.glsl"
#include "frame.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1, 0));
    float c = hash21(i + vec2(0, 1)), d = hash21(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * vnoise(p);
        p = p * 2.03 + vec2(17.1, 9.2);
        a *= 0.5;
    }
    return v;
}

void main() {
    vec2 ndc = gl_FragCoord.xy / frame.screenParams.xy * 2.0 - 1.0;
    vec4 far = frame.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rd = normalize(far.xyz / far.w - frame.camPos.xyz);
    float y = rd.y;

    // The colours come from the scene (frame.skyZenith / skyHorizon), so the same shader draws noon and midnight.
    vec3 zenith  = frame.skyZenith.rgb;
    vec3 horizon = frame.skyHorizon.rgb;
    vec3 S = normalize(frame.sunDir.xyz);

    // Gradient with a slight warm lift toward the sun.
    float t = pow(clamp(y, 0.0, 1.0), 0.45);
    float sunAmt = pow(max(dot(rd, S), 0.0), 3.0);
    vec3 col = mix(horizon, zenith, t);
    col = mix(col, vec3(1.0, 0.86, 0.72), sunAmt * 0.35 * (1.0 - t));

    // Sun disc and halo (HDR so bloom picks it up). A night sky sets the strength to 0: a moonlit sky must not carry a
    // second sun, and the halo would otherwise be a bright disc over the lanterns.
    float sd = max(dot(rd, S), 0.0);
    float disc = smoothstep(0.9992, 0.9997, sd);
    col += vec3(1.0, 0.97, 0.88) * disc * 6.0 * frame.skyParams.x;
    col += vec3(1.0, 0.82, 0.58) * (pow(sd, 28.0) * 0.8 + pow(sd, 5.0) * 0.20) * frame.skyParams.x;

    // Stars: a hashed lattice over the view direction, thresholded so only the brightest cells light up, and faded near
    // the horizon where the fog and the ground take over. Drawn before the clouds, so an overcast night shows none.
    float stars = frame.skyHorizon.w;
    if (stars > 0.0) {
        vec2 sp = vec2(atan(rd.z, rd.x), asin(clamp(rd.y, -1.0, 1.0))) * vec2(48.0, 96.0);
        float cell = hash21(floor(sp));
        float bright = smoothstep(0.994, 1.0, cell);
        // A pin-point needs sub-cell position, or every star is a whole cell wide.
        vec2 sub = fract(sp) - 0.5;
        // smoothstep needs edge0 < edge1; the inverted form is undefined behaviour on some drivers.
        float point = (1.0 - smoothstep(0.0, 0.30, length(sub))) * bright;
        float twinkle = 0.75 + 0.25 * sin(frame.screenParams.z * 1.7 + cell * 43.0);
        col += vec3(0.86, 0.90, 1.00) * point * stars * twinkle * smoothstep(0.02, 0.35, y) * 2.2;
    }

    // Cel-shaded clouds on a plane above the horizon.
    if (y > 0.0) {
        vec2 cuv = rd.xz / (y + 0.14) * 1.1 + vec2(frame.screenParams.z * 0.012, 0.0);
        float n = fbm(cuv * 1.3);
        float c = smoothstep(0.50, 0.60, n);
        // Lit tops: compare with a sample shifted toward the sun.
        float nl = fbm(cuv * 1.3 + S.xz * 0.10);
        float lit = step(0.0, n - nl - 0.012);
        vec3 cloudLit = vec3(1.00, 0.985, 0.96) * 1.15;
        vec3 cloudShade = vec3(0.72, 0.80, 0.94);
        vec3 cloud = mix(cloudShade, cloudLit, lit);
        float fade = smoothstep(0.02, 0.30, y);
        col = mix(col, cloud, c * fade * frame.skyZenith.w);
    }
    outColor = vec4(col, 0.0);   // alpha 0: environment (character mask)
}
