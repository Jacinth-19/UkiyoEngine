// Endfield-style hybrid character shading (engine implementation).
// Blends a physically based base with a ramp-toned NPR base, then adds the stylised terms:
// spec ramp, anisotropic hair highlight, matcap, face-SDF shadow, rain wetness, rim, emission.
// The generator (build_endfield.py) puts the header in front: frame block, material block,
// texture slots and the P_<name> accessors for every property.

// ---- helpers ---------------------------------------------------------------------------

vec3 sampleLut(vec3 c) {
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 lut = atlasSample(REGION_LUT, vec2(clamp(luma, 0.0, 1.0), P_lutRow));
    return mix(c, c * lut, P_lutStrength);
}

float channelOf(vec4 v, float ch) {
    if (ch < 0.5) return v.r;
    if (ch < 1.5) return v.g;
    if (ch < 2.5) return v.b;
    return v.a;
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float sawFade(float t, float fadeIn, float fadeOut) {
    return smoothstep(0.0, fadeIn, t) * (1.0 - smoothstep(1.0 - fadeOut, 1.0, t));
}

// One layer of falling droplets on a grid. Each cell owns at most one drop; it falls over
// its phase, then fades. Returns 0..1 coverage.
float rainLayer(vec2 uv, float scale, float speed, float density, float seed, float t) {
    vec2 p = uv * scale;
    vec2 cell = floor(p);
    vec2 local = fract(p) - 0.5;
    float r = hash21(cell + seed);
    float phase = fract(t * speed * 0.22 + r);
    float isActive = step(1.0 - clamp(density, 0.0, 1.0), r);
    float fade = sawFade(phase, 0.16, 0.34) * isActive;

    vec2 jitter = (vec2(hash21(cell + seed + 2.17), hash21(cell + seed + 5.93)) - 0.5) * vec2(0.48, 0.18);
    jitter.y += mix(0.24, -0.20, smoothstep(0.10, 0.92, phase));
    vec2 q = local - jitter;

    float radius = mix(0.055, 0.135, smoothstep(0.0, 0.34, phase));
    float d = length(q / vec2(0.92, 1.08));
    float body = 1.0 - smoothstep(radius * 0.55, radius, d);
    float rim = (1.0 - smoothstep(radius, radius + 0.045, d)) * smoothstep(radius * 0.48, radius * 0.82, d);
    float drop = clamp(body * 0.75 + rim * 0.45, 0.0, 1.0) * fade;

    float trailWindow = smoothstep(0.03, 0.14, q.y) * (1.0 - smoothstep(0.14, 0.34, q.y));
    float trail = (1.0 - smoothstep(0.0, 0.022, abs(q.x))) * trailWindow * fade * step(0.82, r);
    return clamp(drop + trail * P_wetStreakStrength, 0.0, 1.0);
}

float rainCoverage(vec2 uv, float t) {
    float a = rainLayer(uv, P_wetDropletScale, P_wetDropletSpeed, P_wetDropletDensity, 0.0, t);
    float b = rainLayer(uv + vec2(0.17, 0.31), P_wetDropletScale * 1.65, P_wetDropletSpeed * 1.27,
                        P_wetDropletDensity * 0.72, 19.37, t) * 0.58;
    return clamp(max(a, b), 0.0, 1.0);
}

// Face shadow from a signed-distance atlas. The head's forward and right come from the frame
// (toonD, the character's head direction) when the face shadow is on, else from the material.
// Returns 1 where the face is in shadow for the light direction Lw.
float faceSdfShadow(vec2 uv, vec3 Lw) {
    bool fromFrame = frame.toonD.z > 0.5;
    vec3 fwdRaw = fromFrame ? vec3(frame.toonD.x, 0.0, frame.toonD.y) : P_faceForwardWS;
    vec3 fwd = normalize(fwdRaw.x * fwdRaw.x + fwdRaw.z * fwdRaw.z > 1e-10 ? fwdRaw : vec3(0.0, 0.0, 1.0));
    vec3 right = fromFrame ? normalize(cross(vec3(0.0, 1.0, 0.0), fwd)) : normalize(P_faceRightWS);
    vec3 up = cross(fwd, right);
    float upLen2 = dot(up, up);
    if (upLen2 < 1e-10) return 0.0;
    vec3 lightOnHead = Lw - dot(Lw, up) / upLen2 * up;
    float lightLen = length(lightOnHead);
    if (lightLen <= 1e-5) return 0.0;
    vec3 ld = lightOnHead / lightLen;

    float angle01 = acos(clamp(dot(ld, right), -1.0, 1.0)) / 3.14159265;
    float exposeRight = step(angle01, 0.5);
    float valueR = pow(clamp(1.0 - angle01 * 2.0, 0.0, 1.0), 3.0);
    float valueL = pow(clamp(angle01 * 2.0 - 1.0, 0.0, 1.0), 3.0);
    float angleValue = mix(valueL, valueR, exposeRight);

    vec4 sdf = texture(tex_faceSdf, uv);
    float sdfR = channelOf(sdf, P_sdfChannel);
    float sdfL = channelOf(texture(tex_faceSdf, vec2(1.0 - uv.x, uv.y)), P_sdfChannel);
    sdfR = mix(sdfR, sdf.r, P_sdfDirectionalRG);
    sdfL = mix(sdfL, sdf.g, P_sdfDirectionalRG);
    float sr = mix(sdfR, sdfL, P_sdfSwapRG);
    float sl = mix(sdfL, sdfR, P_sdfSwapRG);
    float sdfValue = mix(sr, sl, exposeRight);

    float bias = P_sdfThreshold - 0.5;
    float lit = smoothstep(angleValue - P_sdfSoftness, angleValue + P_sdfSoftness, sdfValue + bias);
    float front = smoothstep(-P_sdfBackFade, P_sdfBackFade, dot(ld, fwd));
    lit = mix(1.0, lit, front);
    return 1.0 - lit;
}

// 1 on the side of the split line that uses the NPR look, 0 on the other side. The split is off
// unless the direction is set.
float splitSide() {
    vec2 dir = vec2(P_splitLineDirX, P_splitLineDirY);
    if (dot(dir, dir) < 1e-8) return 0.0;
    vec2 res = frame.screenParams.xy;
    vec2 px = gl_FragCoord.xy;   // bottom-up here, so already y-up (matches effects.frag)
    vec2 perp = vec2(-dir.y, dir.x);
    float s = dot(px - res * 0.5, perp) - P_splitLineOffsetPx;
    return s >= 0.0 ? 1.0 : 0.0;
}

#ifdef NAHIDA_ILM
// BodyAndHair's ramp-row select. The ILM alpha picks one of five rows (rampRow0..4, rows 1..5), with hard steps
// at the midpoints of the five enum values 0, 0.3, 0.5, 0.7 and 1: 0.15, 0.4, 0.6 and 0.85. The V coordinate is
// row / 10 - 0.05. The night ramp is the day ramp shifted by 0.5.
float ilmRampV(float a) {
    float v = P_rampRow4 / 10.0 - 0.05;
    v = mix(v, P_rampRow3 / 10.0 - 0.05, step(a, 0.85));
    v = mix(v, P_rampRow2 / 10.0 - 0.05, step(a, 0.6));
    v = mix(v, P_rampRow1 / 10.0 - 0.05, step(a, 0.4));
    v = mix(v, P_rampRow0 / 10.0 - 0.05, step(a, 0.15));
    return v;
}
#endif

// ---- main ------------------------------------------------------------------------------

void main() {
    vec2 uv = (P_useBackUV > 0.5 && !gl_FrontFacing) ? v_uv1 : v_uv;   // back faces read TEXCOORD_1 when on
    // Packed inputs. Each channel is the one the unpacked shader read:
    //   normalStyle: rgb normal, a style.r        maskFlow: r mask.r, g mask.g, b flow.r, a flow.g
    //   highlightHair: r highlight.r, g highlight.g, b hair.r, a hair.g
    vec4 base = texture(tex_base, uv) * vec4(P_baseColor, 1.0);
    vec4 normalStyle = texture(tex_normalStyle, uv);
    vec4 param = texture(tex_param, uv);
    vec4 maskFlow = texture(tex_maskFlow, uv);
    vec4 emission = texture(tex_emission, uv);
    vec4 highlightHair = texture(tex_highlightHair, uv);
    vec2 mask = maskFlow.rg;
    vec2 flow = maskFlow.ba;
    vec2 highlight = highlightHair.rg;
    vec2 hairSpecRG = highlightHair.ba;
    float styleR = normalStyle.a;

    float t = P_wetPreviewTime > 0.0 ? P_wetPreviewTime : frame.screenParams.z;
    float wet = clamp(P_wetness, 0.0, 1.0);
    float wetFlow = clamp(0.45 + flow.x * 0.55 + mask.x * 0.12, 0.0, 1.0);
    float inferredGloss = smoothstep(0.52, 0.92, 1.0 - param.g);
    float inferredMetal = clamp(param.r * P_metallicScale, 0.0, 1.0);
    float dropletSurface = clamp(P_wetGlossMask * 0.48 + inferredGloss * 0.46 + inferredMetal * 0.36
                                 + P_wetClothMask * 0.08, 0.0, 1.0);

    // Surfaces that face up or down take more water. The graph vertex stage has no tangents,
    // so the tangent frame comes from UV derivatives.
    vec3 Ng = normalize(v_normal);
    vec3 dp1 = dFdx(v_world);
    vec3 dp2 = dFdy(v_world);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, Ng);
    vec3 dp1perp = cross(Ng, dp1);
    vec3 Tm = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 Bm = dp2perp * duv1.y + dp1perp * duv2.y;
    float invm = inversesqrt(max(max(dot(Tm, Tm), dot(Bm, Bm)), 1e-12));
    vec3 T = normalize(Tm * invm);
    mat3 TBN = mat3(Tm * invm, Bm * invm, Ng);

    float verticalSurface = smoothstep(0.12, 0.55, 1.0 - abs(Ng.y));
    vec2 rainUv = uv + (flow - 0.5) * 0.018;
    float rain = rainCoverage(rainUv, t);
    rain = clamp(rain * wet * wetFlow * dropletSurface * (0.08 + verticalSurface * 1.22), 0.0, 1.0);
    float clothWet = wet * P_wetClothMask * wetFlow;
    float glossWet = wet * clamp(P_wetGlossMask + rain * 0.75, 0.0, 1.0);
    base.rgb = mix(base.rgb, base.rgb * P_wetDarkenColor, clothWet * P_wetDarken);

    float alpha = base.a * P_alpha;
    if (alpha - P_alphaClip < 0.0) discard;

    vec3 nt = normalStyle.xyz * 2.0 - 1.0;
    nt = normalize(mix(vec3(0.0, 0.0, 1.0), nt, P_normalStrength));
    nt.xy += vec2(dFdx(rain), dFdy(rain)) * P_wetNormalStrength * 1.35;
    nt = normalize(nt);
    vec3 N = normalize(TBN * nt);
    vec3 V = normalize(frame.camPos.xyz - v_world);

    vec3 L = normalize(frame.sunDir.xyz * P_lightDirMul);
    vec3 H = normalize(L + V);
    float NoL = clamp(dot(N, L), 0.0, 1.0);
    float NoV = clamp(dot(N, V), 0.0, 1.0);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float halfLambert = NoL * 0.5 + 0.5;
    float lightVisible = shadowFactor(v_world, N);   // 1 = lit by the sun

    // Physically based part: metal, smoothness and occlusion from the parameter map, lit by the
    // sun and the sky hemisphere.
    float metallic = inferredMetal;
    float baseSmooth = clamp(mix(P_smoothnessMin, P_smoothnessMax, 1.0 - param.g) * P_smoothnessScale, 0.0, 1.0);
    float smoothness = clamp(baseSmooth + glossWet * P_wetSmoothnessBoost, 0.0, 1.0);
    float occlusion = mix(1.0, clamp(param.b, 0.0, 1.0), P_occlusionStrength);
    occlusion *= mix(1.0, v_color.r, P_vertexAOStrength);   // COLOR_0.r as ambient occlusion; white = no change
    vec3 albedo = base.rgb;
    vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * N.y);
    float shin = exp2(1.0 + 10.0 * smoothness);
    float blinn = pow(NoH, shin) * (shin + 8.0) / 25.13274;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    // Nahida: ILM blue is the highlight mask (the NPR repo's ILM.b), so the specular only shows where it is set.
    float specMask = 1.0;
#ifdef NAHIDA_ILM
    specMask = clamp(param.b, 0.0, 1.0);
#endif
    vec3 pbr = albedo * (1.0 - metallic) * (frame.sunColor.rgb * NoL * lightVisible + hemi) * occlusion
             + F0 * frame.sunColor.rgb * blinn * step(0.0, NoL) * lightVisible * specMask;

    // NPR part: a diffuse ramp, the LUT colour grade and the light wrap.
    float rampU = smoothstep(P_rampThreshold - P_rampFeather, P_rampThreshold + P_rampFeather, halfLambert);
    rampU = clamp(mix(rampU, styleR, P_styleRampStrength), 0.0, 1.0);
    vec3 shadowTint = P_shadowColor;
#ifdef NAHIDA_ILM
    // customMatType >= 0 replaces the ILM alpha as the ramp-row type. dayOverride < 0 follows the sun.
    float ilmDay = ilmRampV(P_customMatType >= 0.0 ? P_customMatType : normalStyle.a);
    float isDay = P_dayOverride < 0.0 ? (L.y + 1.0) * 0.5 : clamp(P_dayOverride, 0.0, 1.0);
    // The NoL edge is 0.2..0.4 at the defaults; shadowOffset moves it and shadowSoftness widens it.
    float ilmEdge = max(0.1 * P_shadowSoftness, 1e-4);
    float ilmC = 0.3 + P_shadowOffset;
    float legacyU = clamp(smoothstep(ilmC - ilmEdge, ilmC + ilmEdge, pow(NoL * 0.5 + 0.5, 2.0)), 0.003, 0.997);
    // kazeShadow = 1: Kaze's shadow (GetShadow). Half-Lambert x AO (ILM green x vertex colour red), forced lit at AO 0.9,
    // then smoothstep over [0.5 + offset - width, 0.5 + offset]. Past the top edge the ramp is 1, and the shadow tint fades in.
    float kazeAo = clamp(param.g * v_color.r, 0.0, 1.0);   // Nahida: ILM green = AO (Kaze's lightMap.g)
    float kazeS = clamp(2.0 * (dot(N, L) * 0.5 + 0.5) * kazeAo, 0.0, 1.0);
    kazeS = mix(kazeS, 1.0, step(0.9, kazeAo));
    float kzMax = 0.5 + P_shadowOffset;
    float kazeU = clamp(smoothstep(kzMax - ilmEdge, kzMax, kazeS), 0.003, 0.997);
    float ilmU = P_kazeShadow > 0.5 ? kazeU : legacyU;
    vec3 rampColor = mix(atlasSample(REGION_RAMP, vec2(ilmU, ilmDay + 0.5)),
                         atlasSample(REGION_RAMP, vec2(ilmU, ilmDay)), isDay);
    if (P_kazeShadow > 0.5) {
        rampColor = mix(rampColor, vec3(1.0), step(kzMax, kazeS));
        shadowTint = mix(P_shadowColor, vec3(1.0), smoothstep(0.9, 1.0, ilmU));
    }
#else
    vec3 rampColor = atlasSample(REGION_RAMP, vec2(rampU, 0.5));   // Endfield ramps: one row, replicated
#endif
    vec3 lutBase = mix(sampleLut(base.rgb), base.rgb, P_faceColorStabilize);
    float wrapped = mix(NoL, halfLambert, P_nprLightWrap);
    float normalShade = mix(1.0, mix(0.92, 1.08, clamp(wrapped, 0.0, 1.0)), P_nprNormalShadeStrength);
    vec3 nprLit = lutBase * normalShade * P_nprLitBoost;
    vec3 rampTone = lutBase * rampColor * shadowTint;

    float sdfShadow = faceSdfShadow(uv, L);
    vec3 sdfTone = lutBase * P_sdfShadowColor;
    rampTone = mix(rampTone, sdfTone, sdfShadow * P_sdfShadowStrength);

    float realtimeShadow = (1.0 - lightVisible) * P_realtimeShadowStrength;
    float rampShadow = 1.0 - rampU;

    // Debug views: 1 base, 2 param, 3 style, 4 mask, 5 face SDF, 6 SDF channel, 7 SDF shadow,
    // 8 realtime shadow, 9 ramp shadow. Mode 0 is the normal shading.
    int mode = int(P_debugMode + 0.5);
    if (mode > 0) {
        vec3 dbg = base.rgb;
        if (mode == 2) dbg = param.rgb;
        else if (mode == 3) dbg = vec3(styleR);
        else if (mode == 4) dbg = vec3(mask, 0.0);
        else if (mode == 5) dbg = texture(tex_faceSdf, uv).rgb;
        else if (mode == 6) dbg = vec3(channelOf(texture(tex_faceSdf, uv), P_sdfChannel));
        else if (mode == 7) dbg = vec3(sdfShadow);
        else if (mode == 8) dbg = vec3(realtimeShadow);
        else if (mode == 9) dbg = vec3(rampShadow);
        o_color = vec4(clamp(dbg * P_debugExposure, 0.0, 1.0), alpha);
        return;
    }

    float stableRampShadow = mix(rampShadow, 0.0, P_faceColorStabilize);
    float stylizedShadow = clamp(max(max(realtimeShadow, stableRampShadow), sdfShadow * P_sdfShadowStrength), 0.0, 1.0);
    float pbrBlend = mix(P_pbrBaseBlend, 0.0, P_faceColorStabilize);
    vec3 litBase = mix(nprLit, pbr, pbrBlend);
    vec3 hybrid = mix(litBase, rampTone, stylizedShadow * P_rampBlend);
    hybrid = max(hybrid, lutBase * P_shadowFloor * stylizedShadow);

    float nprShadow = clamp(max(stableRampShadow, sdfShadow * P_sdfShadowStrength), 0.0, 1.0);
    vec3 pureNpr = mix(nprLit, rampTone, nprShadow * P_rampBlend);
    pureNpr = max(pureNpr, lutBase * P_shadowFloor * nprShadow);

    // Specular: a ramp indexed by N.H and the paint's specular value, the wet glints, and the
    // anisotropic hair highlight.
    vec3 specRamp = atlasSample(REGION_SPEC, vec2(NoH, param.a));
    hybrid += specRamp * mask.x * P_specRampStrength * pow(NoH, 32.0) * clamp(NoL + 0.2, 0.0, 1.0);
    hybrid += specRamp * (rain + glossWet * 0.35) * P_wetSpecBoost * pow(NoH, 48.0) * clamp(NoL + 0.25, 0.0, 1.0);
    hybrid += rain * P_wetDropletVisibility * mix(0.25, 0.85, NoV) * clamp(NoL + 0.35, 0.0, 1.0);
    float rainCore = smoothstep(0.28, 0.88, rain);
    float rainEdge = clamp(length(vec2(dFdx(rain), dFdy(rain))) * 10.0, 0.0, 1.0);
    float reflectGlint = pow(clamp(dot(reflect(-L, N), V), 0.0, 1.0), 64.0);
    float glint = rainCore * (0.35 + rainEdge) * (pow(NoH, 96.0) * 1.4 + reflectGlint * 1.15) * clamp(NoL + 0.35, 0.0, 1.0);
    hybrid += glint * P_wetDropletVisibility * P_wetSpecBoost * vec3(1.0, 0.96, 0.88);

    float TdotH = dot(T, H);
    float hairSinTH = sqrt(clamp(1.0 - TdotH * TdotH, 0.0, 1.0));
    float hairSpec = pow(clamp(hairSinTH + P_hairSpecShift, 0.0, 1.0), P_hairAnisoPower);
    float hairMask = clamp(hairSpecRG.x + hairSpecRG.y * 0.5 + mask.x * 0.25, 0.0, 1.0);
    hybrid += hairSpec * hairMask * specRamp * base.rgb * P_hairAnisoStrength * clamp(NoL + 0.35, 0.0, 1.0);

    // Matcap: looked up with the view-space normal.
    vec3 nVS = normalize((frame.view * vec4(N, 0.0)).xyz);
    vec3 matcap = atlasSample(REGION_MATCAP, nVS.xy * 0.5 + 0.5);
    hybrid += matcap * P_matCapStrength * (1.0 - P_faceColorStabilize) * mask.y;

    // Stylised extras, added to both looks.
    float faceHighlight = clamp(highlight.x + highlight.y * 0.5, 0.0, 1.0);
    vec3 stylizedAdd = faceHighlight * P_highlightStrength * mix(0.4, 1.0, NoL) * base.rgb;
    float emissionMask = clamp(emission.a + emission.r + flow.x * 0.35, 0.0, 1.0);
    stylizedAdd += emission.rgb * P_emissionColor * emissionMask * P_emissionStrength;
    float rim = pow(1.0 - NoV, P_rimPower) * P_rimIntensity;
    stylizedAdd += rim * P_rimColor;
    hybrid += stylizedAdd;
    pureNpr += stylizedAdd;

#ifdef NAHIDA_ILM
    // BodyAndHair's split: the NPR side is pureNpr and the PBR side is the lit PBR colour. With the split off,
    // the colour is the Hybrid blend, as before.
    vec2 splitDir = vec2(P_splitLineDirX, P_splitLineDirY);
    bool splitOn = dot(splitDir, splitDir) > 1e-8;
    vec3 color = splitOn ? mix(pbr + stylizedAdd, pureNpr, splitSide()) : hybrid;
#else
    vec3 color = mix(hybrid, pureNpr, splitSide());
#endif
    o_color = vec4(color, alpha);
}
