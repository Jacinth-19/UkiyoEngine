// MToon-style toon shading: shade ramp with shadow receive, hemisphere ambient,
// point lights, parametric rim and emission. Output is linear HDR.
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

float linearstep(float a, float b, float x) {
    return clamp((x - a) / (b - a), 0.0, 1.0);
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
    if (mtl.flags.w > 0.5) {                       // KHR_materials_unlit
        outColor = vec4(base.rgb, outA);
        return;
    }

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    // RealToon normal map (off unless the project turns it on and the material has a normal texture).
    if (frame.toonC.y > 0.0 && mtl.toon2.y > 0.5)
        N = applyNormalMap(N, vWorldPos, vUV, texture(uNormalTex, vUV).rgb, frame.toonC.y);
    float glossMask = mtl.toon2.x > 0.5 ? 1.0 - texture(uGlossTex, vUV).g : 1.0;
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    vec3 L = normalize(frame.sunDir.xyz);

    float toony = clamp(mtl.toon.y, 0.0, 1.0);
    float shift = mtl.toon.x;
    // RealToon smear: the cel edge is pulled along the object's motion (pc.p1.xyz, per frame). Surfaces facing
    // the motion take more light, so the shade edge stretches away from the direction the object is moving.
    float smearSpeed = length(pc.p1.xyz);
    vec3 smearDir = smearSpeed > 1e-6 ? pc.p1.xyz / smearSpeed : vec3(0.0);
    float smearShift = frame.toonD.w * clamp(smearSpeed / 0.02, 0.0, 1.0) * dot(N, smearDir);
    float ndl = dot(N, L) + smearShift;
    float ramp = linearstep(-1.0 + toony, 1.0 - toony, ndl + shift);
    if (frame.toonC.x > 0.5) {
        // Cel band: three flat steps (0, 0.28, 0.60, 1.00). The curve follows the band function in the
        // RavenEngine PP set, re-derived here.
        float nl = max(ndl + shift, 0.0);
        ramp = smoothstep(0.02, 0.06, nl) * (0.28 + smoothstep(0.31, 0.35, nl) * 0.32 + smoothstep(0.67, 0.71, nl) * 0.40);
    }
    if (frame.toonD.z > 0.5 && mtl.toon2.z > 0.5) ramp = faceShadowLit(vUV, L);   // face SDF light test
    float shadow = shadowFactor(vWorldPos, N);
    float lit = ramp * shadow;
    // RealToon self shadow: a second threshold on N.L, set by the project settings (off by default).
    if (frame.toonA.w > 0.5) {
        float nl01 = 0.5 * (ndl + shift) + 0.5;     // N.L remapped to 0..1
        lit *= smoothstep(frame.toonB.x - frame.toonB.y, frame.toonB.x + frame.toonB.y, nl01);
    }

    vec3 albedo = base.rgb;
    vec3 shadeTint = mtl.shadeColor.rgb * texture(uShadeTex, vUV).rgb;
    vec3 toonColor = mix(albedo * shadeTint, albedo, lit);
    vec3 color = toonColor * frame.sunColor.rgb;

    // Ambient. A scene with an environment (docs/IBL.md) shades with the probe at the camera's focus, equalised the
    // way MToon defines it: gi(n) = lerp(rawGi(n), (rawGi(up) + rawGi(down)) / 2, giEqualization). Without one, the
    // two-colour hemisphere stays, so every scene drawn before this existed renders identically.
    float hemi = N.y * 0.5 + 0.5;
    vec3 amb;
    if (frame.envParams.x > 0.0) {
        vec3 rawGi = shIrradiance(N);
        vec3 uniformedGi = (shIrradiance(vec3(0.0, 1.0, 0.0)) + shIrradiance(vec3(0.0, -1.0, 0.0))) * 0.5;
        amb = mix(rawGi, uniformedGi, mtl.toon.z) * frame.envParams.z;
    } else {
        amb = mix(frame.groundColor.rgb, frame.skyColor.rgb, hemi);
        amb = mix(amb, (frame.skyColor.rgb + frame.groundColor.rgb) * 0.5, mtl.toon.z);
    }
    color += albedo * amb * 0.55;

    // Point lights (toon-banded as well). A scene that authors lights gets its own (up to eight, ranked by
    // lights.h); a scene that authors none keeps the two stylistic fills the engine has always had, so every scene
    // drawn before this existed renders identically.
    if (frame.lightParams.x < 0.5) {
        for (int i = 0; i < 4; ++i) {
            float r = frame.pointPos[i].w;
            if (r <= 0.0) continue;
            vec3 dv = frame.pointPos[i].xyz - vWorldPos;
            float dist = length(dv);
            float att = clamp(1.0 - dist / r, 0.0, 1.0);
            att *= att;
            float pn = dot(N, dv / max(dist, 1e-4));
            float pr = linearstep(-1.0 + toony, 1.0 - toony, pn + shift);
            color += mix(albedo * shadeTint, albedo, pr) * frame.pointColor[i].rgb * att;
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            if (i >= int(frame.lightParams.x)) break;
            vec4 posRadius = frame.lights[i].posRadius;
            vec3 toLight = posRadius.xyz - vWorldPos;
            float dist = length(toLight);
            vec3 l = toLight / max(dist, 1e-4);
            // Inverse-square, windowed to nothing at the authored reach: a light that is on at its edge and off one
            // metre past it draws a visible ring, and an unbounded one floods the scene.
            float reach = posRadius.w;
            float window = clamp(1.0 - dist / max(reach, 1e-3), 0.0, 1.0);
            float att = window * window / (1.0 + dist * dist);
            if (frame.lights[i].params.x > 1.5) {
                // A spot: the cone is soft between the inner and outer angles, and hard-clipped outside the outer one.
                float cd = dot(-l, normalize(frame.lights[i].dirCosOuter.xyz));
                float cosOuter = frame.lights[i].dirCosOuter.w;
                float cosInner = frame.lights[i].params.y;
                float cone = smoothstep(cosOuter, max(cosInner, cosOuter + 1e-3), cd);
                att *= cone;
            }
            if (att <= 0.0) continue;
            float pn = dot(N, l);
            float pr = linearstep(-1.0 + toony, 1.0 - toony, pn + shift);
            vec3 litColor = mix(albedo * shadeTint, albedo, pr);
            color += litColor * frame.lights[i].colorIntensity.rgb * frame.lights[i].colorIntensity.w * att;
        }
    }

    // Leaf transmission (own implementation): light from behind a thin surface shows through, most when the view
    // looks towards the light. It takes the surface colour and the sun colour. The sun shadow is not applied to it.
    if (mtl.leaf.x > 0.0) {
        float facing = clamp(dot(V, -L), 0.0, 1.0);
        float behind = clamp(-dot(N, L), 0.0, 1.0);
        color += albedo * frame.sunColor.rgb * (pow(facing, mtl.leaf.y) * behind * mtl.leaf.x);
    }

    // Parametric rim light, faded in shadowed areas by rimLightingMix.
    float ndv = clamp(dot(N, V), 0.0, 1.0);
    float rim = pow(1.0 - ndv, max(mtl.rim.w, 0.01));
    rim *= mix(1.0, ramp, clamp(mtl.toon.w, 0.0, 1.0));
    color += mtl.rim.rgb * rim * 0.8;

    // RealToon hair highlight and gloss (zero by default).
    color += toonExtras(N, V, L, vWorldPos, vUV, ramp, glossMask);
    color += toonReflection(N, V, glossMask);                      // RealToon reflection (project strength)

    color += toonSoftSweep(N, L, albedo, vWorldPos);
    color += toonPbr(N, V, L, albedo, lit, mtl.toon2.x > 0.5 ? texture(uGlossTex, vUV).gb : vec2(0.5, 0.0));
    color = toonEyeFlatten(color, albedo);
    color += toonEyeAdd(vUV, N);
    color += mtl.emissive.rgb * texture(uEmissiveTex, vUV).rgb;
    color = toonCharacterFx(color, fxGlow);
    outColor = vec4(color, outA);
}
