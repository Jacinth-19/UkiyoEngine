// RealToon extras shared by the toon shaders (mtoon.frag and mmdtoon.frag). Every term is zero
// unless the project turns it on (frame.toonA / frame.toonB, set from the Graphics page), so the
// defaults add nothing. Requires frame.glsl.

// Strand direction from screen-space UV derivatives (no tangent attribute). Falls back to a fixed
// axis where the UVs have no gradient.
vec3 strandTangent(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 T = cross(dp2, N) * duv1.x + cross(N, dp1) * duv2.x;
    float len2 = dot(T, T);
    if (len2 < 1e-12) return normalize(cross(N, vec3(0.0, 1.0, 0.0)) + vec3(1e-4));
    return T * inversesqrt(len2);
}

// Tangent-space normal map without a tangent attribute: the tangent frame comes from UV derivatives
// (cotangent frame). tn is the sampled texel (0..1). strength scales the tangent-plane tilt.
vec3 applyNormalMap(vec3 N, vec3 p, vec2 uv, vec3 tn, float strength) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    vec3 ts = tn * 2.0 - 1.0;
    ts.xy *= strength;
    return normalize(TBN * normalize(ts));
}

// RealToon reflection (procedural sky, no cubemap): the sky/ground gradient in the reflection direction, weighted
// by a Fresnel term and the gloss mask. Strength is the project default in toonC.w (0 = off).
vec3 toonReflection(vec3 N, vec3 V, float glossMask) {
    float k = frame.toonC.w;
    if (k <= 0.0) return vec3(0.0);
    vec3 R = reflect(-V, N);
    vec3 env = mix(frame.groundColor.rgb, frame.skyColor.rgb, smoothstep(-0.2, 0.6, R.y));
    float fres = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);
    return env * k * glossMask * (0.04 + 0.96 * fres);
}

// Face shadow from the SDF atlas (nine light angles 0..180 deg in a 3x3 grid, see tools/make_face_sdf.py).
// The light's angle to the head's forward axis picks two neighbouring tiles; their SDF values are blended and
// compared with 0.5 (the boundary). Light from the far side mirrors the UV across the face. Returns 1 when lit.
float faceTile(float i, vec2 q) {
    return textureLod(uFaceSdfTex, (vec2(mod(i, 3.0), floor(i / 3.0)) + q) / 3.0, 0.0).r;
}
float faceShadowLit(vec2 uv, vec3 L) {
    vec2 Lh = L.xz;
    float lh = length(Lh);
    if (lh < 1e-4) return 1.0;
    Lh /= lh;
    vec2 f = normalize(frame.toonD.xy + vec2(1e-5, 0.0));
    vec2 r = vec2(f.y, -f.x);                       // face right (cross of up and forward)
    float a = acos(clamp(dot(f, Lh), -1.0, 1.0)) / 3.14159265;   // 0 = light in front, 1 = behind
    vec2 q = vec2(dot(r, Lh) < 0.0 ? 1.0 - uv.x : uv.x, uv.y);
    float t = clamp(a, 0.0, 1.0) * 8.0;
    float i0 = floor(min(t, 7.999));
    float s = mix(faceTile(i0, q), faceTile(i0 + 1.0, q), t - i0);
    return smoothstep(0.48, 0.52, s);
}

// Camera dither fade: a surface closer than toonF.x to the camera is dithered out, and one beyond toonF.y is solid.
// Off unless toonF.y > toonF.x. Keeps a close character from blocking the view.
void toonDitherFade(vec3 p) {
    float near = frame.toonF.x;
    float far = frame.toonF.y;
    if (far <= near) return;
    float d = length(frame.camPos.xyz - p);
    float keep = clamp((d - near) / (far - near), 0.0, 1.0);
    if (keep < ditherBayer4(gl_FragCoord.xy)) discard;
}

// Soft light and light sweep, added after the toon lighting.
// Soft light: a wrap term (N.L remapped to 0..1, squared) adds albedo light to the shade side, for smooth skin.
// Light sweep: a bright band climbs from 0 m to 2 m of world height at frame.toonE.z cycles per second, strength
// toonE.y. It assumes the character stands on y = 0 at about human height.
vec3 toonSoftSweep(vec3 N, vec3 L, vec3 albedo, vec3 p) {
    vec3 add = vec3(0.0);
    float soft = frame.toonE.x;
    if (soft > 0.0) {
        float wrap = clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
        add += albedo * frame.sunColor.rgb * soft * wrap * wrap * 0.25;
    }
    float sweep = frame.toonE.y;
    if (sweep > 0.0) {
        float s = fract(frame.screenParams.z * frame.toonE.z);
        float h = mix(0.0, 2.0, s);                  // band height, metres
        float band = exp(-pow((p.y - h) / 0.15, 2.0));
        add += vec3(1.0) * band * sweep * 0.6;
    }
    return add;
}

// ---- Eye shader and stylised PBR (ASP-style features, our own approximations) ----
// Parallax pupil: the base-texture UV is shifted along the view direction in the tangent plane, by
// frame.toonG.z times a radial weight that is 1 at the centre of the eye texture and 0 at its edge. There is no
// height map, so this approximates depth. Eye materials only (mtl.toon2.w).
vec2 toonBaseUV(vec2 uv, vec3 p, vec3 N, vec3 V) {
    if (mtl.toon2.w < 0.5 || frame.toonG.z <= 0.0) return uv;
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    vec2 vt = vec2(dot(V, T), dot(V, B)) * invmax;
    float h = 1.0 - clamp(length(uv - vec2(0.5)) * 2.0, 0.0, 1.0);
    return uv - vt * frame.toonG.z * h;
}

// Eye flatten: blends the lit colour toward the albedo at full sun, so the eye reads evenly in any light.
vec3 toonEyeFlatten(vec3 color, vec3 albedo) {
    float k = frame.toonG.y;
    if (mtl.toon2.w < 0.5 || k <= 0.0) return color;
    return mix(color, albedo * frame.sunColor.rgb, clamp(k, 0.0, 1.0));
}

// Eye matcap and highlight map. Each term is off unless its texture is present (mtl.flags2.z / flags2.w), so the
// default white placeholder adds nothing. No material gets these textures from the file yet.
vec3 toonEyeAdd(vec2 uv, vec3 N) {
    if (mtl.toon2.w < 0.5) return vec3(0.0);
    vec3 add = vec3(0.0);
    if (mtl.flags2.z > 0.5 && frame.toonG.w > 0.0) {
        vec3 nv = (frame.view * vec4(N, 0.0)).xyz;
        add += texture(uMatcapTex, nv.xy * 0.5 + 0.5).rgb * frame.toonG.w;
    }
    if (mtl.flags2.w > 0.5 && frame.toonH.x > 0.0) {
        add += texture(uHighlightTex, uv).rgb * frame.toonH.x;
    }
    return add;
}

// Stylised PBR: a GGX highlight, banded into a hard-edged disc, tinted by F0 (dielectric 0.04, or the albedo for
// metal). rm = (roughness, metallic). The highlight is multiplied by the toon light mask lit. Strength frame.toonG.x.
vec3 toonPbr(vec3 N, vec3 V, vec3 L, vec3 albedo, float lit, vec2 rm) {
    float k = frame.toonG.x;
    if (k <= 0.0) return vec3(0.0);
    float a = max(rm.x * rm.x, 0.02);
    float a2 = a * a;
    vec3 H = normalize(L + V);
    float nh = max(dot(N, H), 0.0);
    float dn = nh * nh * (a2 - 1.0) + 1.0;
    float nd = a2 * a2 / (dn * dn);                 // GGX distribution, normalised to 1 at the peak
    float band = smoothstep(0.1, 0.2, nd);          // wide enough to show at roughness 0.5
    vec3 spec = mix(vec3(0.25), albedo, clamp(rm.y, 0.0, 1.0));   // stylised dielectric specular (0.25, not physical 0.04)
    return spec * band * lit * frame.sunColor.rgb * 0.8 * k;
}

// Added light: an anisotropic hair band and a toon gloss. mask is 1 where the surface is lit, 0 in shade.
// glossMask scales the gloss by a material's gloss texture (1 where there is none).
vec3 toonExtras(vec3 N, vec3 V, vec3 L, vec3 p, vec2 uv, float mask, float glossMask) {
    vec3 add = vec3(0.0);
    vec3 H = normalize(L + V);
    if (frame.toonA.x > 0.0) {
        vec3 T = strandTangent(N, p, uv);
        vec3 Ts = normalize(T + N * frame.toonA.y);
        float th = dot(Ts, H);
        float band = pow(sqrt(max(1.0 - th * th, 0.0)), max(frame.toonA.z, 1.0));
        add += frame.sunColor.rgb * band * frame.toonA.x * mask * 0.6;
    }
    if (frame.toonB.z > 0.0) {
        float g = smoothstep(0.7, 0.8, pow(max(dot(N, H), 0.0), max(frame.toonB.w, 1.0)));
        add += frame.sunColor.rgb * g * frame.toonB.z * mask * glossMask * 0.8;
    }
    return add;
}

// ---- Per-character effects (material fx0..fx4, set by setCharacterFx / setHairFace / setOccludable) ----
float fxHash(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453); }

// Smooth 3D value noise in 0..1, for the dissolve.
float fxNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float x00 = mix(fxHash(i), fxHash(i + vec3(1.0, 0.0, 0.0)), f.x);
    float x10 = mix(fxHash(i + vec3(0.0, 1.0, 0.0)), fxHash(i + vec3(1.0, 1.0, 0.0)), f.x);
    float x01 = mix(fxHash(i + vec3(0.0, 0.0, 1.0)), fxHash(i + vec3(1.0, 0.0, 1.0)), f.x);
    float x11 = mix(fxHash(i + vec3(0.0, 1.0, 1.0)), fxHash(i + vec3(1.0, 1.0, 1.0)), f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}

// Cut-outs run before the shading. Returns the dissolve edge glow (0..1) for toonCharacterFx.
//  - Dissolve: noise below fx0.x is discarded; a band of width fx0.y above the cut glows.
//  - Hair: within fx2.w of the face anchor (fx2.xyz), hair on the camera side is dithered out.
//  - Occlusion: when frame.toonI.w > 0, surfaces between the camera and the target point are
//    dithered out. Materials with fx3.w = 1 (the target itself) are skipped.
float toonCharacterDiscard(vec3 p) {
    float glow = 0.0;
    float cut = mtl.fx0.x;
    if (cut > 0.0) {
        float n = fxNoise(p * 6.0);
        if (n < cut) discard;
        float edge = mtl.fx0.y;
        if (edge > 0.0) glow = 1.0 - smoothstep(cut, cut + edge, n);
    }
    float hr = mtl.fx2.w;
    if (hr > 0.0) {
        vec3 an = mtl.fx2.xyz;
        if (length(frame.camPos.xyz - p) < length(frame.camPos.xyz - an)) {
            float keep = smoothstep(hr * 0.5, hr, length(p - an));
            if (keep < ditherBayer4(gl_FragCoord.xy)) discard;
        }
    }
    float orad = frame.toonI.w;
    if (orad > 0.0 && mtl.fx3.w < 0.5) {
        vec3 c = frame.camPos.xyz;
        vec3 seg = frame.toonI.xyz - c;
        float t = dot(p - c, seg) / max(dot(seg, seg), 1e-6);
        if (t > 0.0 && t < 0.97) {
            float keep = smoothstep(orad * 0.5, orad, length(p - (c + seg * t)));
            if (keep < ditherBayer4(gl_FragCoord.xy)) discard;
        }
    }
    return glow;
}

// Colour effects, applied after the lighting.
vec3 toonCharacterFx(vec3 color, float glow) {
    color += mtl.fx1.rgb * glow * 2.0;                                  // dissolve edge glow
    float pulse = mtl.fx1.w * (0.5 + 0.5 * sin(frame.screenParams.z * 6.0));
    color += mtl.fx4.rgb * pulse;                                       // upgrade pulse
    color = mix(color, mtl.fx3.rgb, clamp(mtl.fx0.w, 0.0, 1.0));        // get-hit flash
    color *= 1.0 - clamp(mtl.fx0.z, 0.0, 1.0) * 0.6;                    // target darken
    return color;
}
