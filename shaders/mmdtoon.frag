// MMD-style toon shading, ported from the uploaded effect (research/reference/MMD_Toon.fx,
// PS of the Render00-07 techniques). The shadow colour is the hue-dependent darkening of
// GetToonColor, chosen by a hard light test (usetoon branch: N.L < 0.5 is in shade). The HLSL
// self-shadow map is replaced by the engine's cascaded shadow map, which also receives the
// character's own shadow. The sphere map and self-shadow depth pass are not ported (Seed-san
// has neither). Output is linear HDR, like mtoon.frag.
#include "common.glsl"
#include "fade_discard.glsl"
#include "frame.glsl"
#include "shadow_sample.glsl"
#include "material.glsl"
#include "toon_extras.glsl"
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// GetToonColor(): the shadow colour. Each branch darkens the colour according to its hue family.
vec3 toonDarkColor(vec3 c) {
    const float e = 1e-5;
    if ((c.g + c.b) / max(c.r, e) < 0.2)                     // red family
        c = c * 0.5;
    if (c.r + c.g + c.b > 2.5)
        c.gb = c.gb * 0.8;
    else if (c.b / max(c.r + c.g, e) < 0.1)                 // yellow family
        c.gb = c.gb * 0.5;
    else if ((c.r + c.g) / max(c.b, e) < 0.2)               // blue family
        c.gb = c.gb * 0.5;
    else if ((c.r + c.b) / max(c.g, e) < 0.2)               // green family
        c = c * 0.5;
    else
        c.rg = c.rg * 0.5;
    return c;
}

void main() {
    toonDitherFade(vWorldPos);
    distanceFadeDiscard();
    float fxGlow = toonCharacterDiscard(vWorldPos);
    vec3 N0 = gl_FrontFacing ? normalize(vNormal) : -normalize(vNormal);
    vec4 tex = texture(uBaseTex, toonBaseUV(vUV, vWorldPos, N0, normalize(frame.camPos.xyz - vWorldPos)));
    vec4 base = mtl.baseColor * tex;
    int mode = int(mtl.flags.x + 0.5);
    if (mode == 1 && base.a < mtl.flags.y) discard;
    float outA = (mode == 2) ? base.a : 1.0;

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    // RealToon normal map (off unless the project turns it on and the material has a normal texture).
    if (frame.toonC.y > 0.0 && mtl.toon2.y > 0.5)
        N = applyNormalMap(N, vWorldPos, vUV, texture(uNormalTex, vUV).rgb, frame.toonC.y);
    float glossMask = mtl.toon2.x > 0.5 ? 1.0 - texture(uGlossTex, vUV).g : 1.0;
    vec3 L = normalize(frame.sunDir.xyz);                  // towards the light, as -LightDirection in the effect

    // Light test (usetoon branch): dot(N, -LightDirection) < 0.5 puts the surface in shade.
    // Softened for the editor look: the shade colour is lifted toward white by kShadeLift, and the
    // light threshold is kLightThreshold (the effect's original values were 0.0 lift and 0.5).
    const float kShadeLift = 0.35;
    const float kLightThreshold = 0.0;
    vec3 darkcolor = mix(toonDarkColor(base.rgb), vec3(1.0), kShadeLift);
    bool inShade = dot(N, L) < kLightThreshold;

    // Self-shadow: a shadowed surface takes the shade colour too (min of the two colours).
    float selfLit = shadowFactor(vWorldPos, N);
    if (selfLit < 0.5) inShade = true;

    // RealToon self shadow: a soft threshold on N.L replaces the hard light test (off by default).
    float mask = inShade ? 0.0 : 1.0;
    vec3 shadowcolor = inShade ? darkcolor : vec3(1.0);
    if (frame.toonA.w > 0.5) {
        float nl01 = 0.5 * dot(N, L) + 0.5;
        mask = smoothstep(frame.toonB.x - frame.toonB.y, frame.toonB.x + frame.toonB.y, nl01) * step(0.5, selfLit);
        shadowcolor = mix(darkcolor, vec3(1.0), mask);
    }
    if (frame.toonC.x > 0.5) {
        // Cel band (same curve as mtoon.frag): three flat steps between the shade and lit colours.
        float nl = max(dot(N, L), 0.0);
        float band = smoothstep(0.02, 0.06, nl) * (0.28 + smoothstep(0.31, 0.35, nl) * 0.32 + smoothstep(0.67, 0.71, nl) * 0.40);
        band *= step(0.5, selfLit);
        mask = band;
        shadowcolor = mix(darkcolor, vec3(1.0), band);
    }
    vec3 color = base.rgb * shadowcolor * frame.sunColor.rgb;
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    if (frame.toonD.z > 0.5 && mtl.toon2.z > 0.5) {                 // face SDF light test
        float fl = faceShadowLit(vUV, L);
        mask = fl;
        shadowcolor = mix(darkcolor, vec3(1.0), fl);
        color = base.rgb * shadowcolor * frame.sunColor.rgb;
    }
    color += toonExtras(N, V, L, vWorldPos, vUV, mask, glossMask);
    color += toonSoftSweep(N, L, base.rgb, vWorldPos);
    color += toonPbr(N, V, L, base.rgb, mask, mtl.toon2.x > 0.5 ? texture(uGlossTex, vUV).gb : vec2(0.5, 0.0));
    color = toonEyeFlatten(color, base.rgb);
    color += toonEyeAdd(vUV, N);
    color = toonCharacterFx(color, fxGlow);
    outColor = vec4(color, outA);
}
