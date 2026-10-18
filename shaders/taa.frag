// Temporal anti-aliasing resolve. Reprojects the previous resolved frame with the scene depth
// and the camera's previous view-projection, clamps the history to the current 3x3 neighbourhood
// (YCoCg AABB), and blends in the current jittered sample.
//
// Inputs: uTex0 current HDR (jittered), uTex1 previous resolved HDR, uDepth scene depth (0..1).
// pc.model   reprojection: previous unjittered viewProj * inverse(current jittered viewProj)
// pc.p0.x    blend factor for the current frame (0.1 typical); 1.0 resets the history
// pc.p1.xy   1 / resolution
// uVel       per-object motion from the velocity pass (UV units, zero where no opaque geometry).
//            Where it is non-zero it is used; elsewhere the camera reprojection in pc.model applies.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
TEX(0, 2) uniform sampler2D uTex1;
TEX(0, 4) uniform sampler2D uDepth;
TEX(0, 5) uniform sampler2D uVel;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 toYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 fromYCoCg(vec3 y) {
    return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
}

void main() {
    vec3 cur = texture(uTex0, vUV).rgb;
    if (pc.p0.x >= 0.999) {            // history reset: take the current frame as is
        outColor = vec4(cur, 1.0);
        return;
    }
    vec2 px = pc.p1.xy;

    // Current 3x3 neighbourhood in YCoCg.
    vec3 mn = vec3(1e9), mx = vec3(-1e9);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 c = toYCoCg(texture(uTex0, vUV + vec2(float(x), float(y)) * px).rgb);
            mn = min(mn, c);
            mx = max(mx, c);
        }

    // Reproject this pixel into the previous frame. The NDC convention matches the frame's
    // invViewProj (fragment position over size, mapped to -1..1).
    vec2 vel = texture(uVel, vUV).xy;
    vec2 prevUV;
    if (dot(vel, vel) > 1e-12) {
        prevUV = vUV - vel;            // moving surface: its own motion (includes camera motion)
    } else {
        float z = texture(uDepth, vUV).r;
        vec4 p = pc.model * vec4(vUV * 2.0 - 1.0, z, 1.0);
        prevUV = (p.xy / p.w) * 0.5 + 0.5;
    }
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        outColor = vec4(cur, 1.0);     // left the screen: no history to use
        return;
    }

    // Clamp the history into the neighbourhood box so stale colour cannot persist.
    vec3 hist = texture(uTex1, prevUV).rgb;
    vec3 histClamped = fromYCoCg(clamp(toYCoCg(hist), mn, mx));

    // Colour-based rejection: if the clamp had to move the history far, it is probably stale
    // (disocclusion or a moving edge). Trust the current frame more there.
    const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
    float lumaH = dot(histClamped, lw);
    float lumaC = dot(cur, lw);
    float diff = abs(lumaH - lumaC) / max(max(lumaH, lumaC), 0.05);
    float alpha = mix(pc.p0.x, 0.5, smoothstep(0.15, 0.5, diff));

    outColor = vec4(mix(histClamped, cur, alpha), 1.0);
}
