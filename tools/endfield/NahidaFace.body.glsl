// Nahida face (Face.shader's logic, our own GLSL). Used by NahidaFace.ukmat only.
// Packed inputs: base = face diffuse; faceSdf = FaceLightmap (R: SDF threshold, G: shadow strength,
// A: forced-light mask; the source's _SDF and _ShadowTex are the same texture); lookup = face shadow ramp
// (region RAMP) and the toon matcap (region MATCAP). Face has no normal map.
// Head direction: frame.toonD (as the Endfield face path) when set, else the material's forward and right.

void main() {
    vec2 uv = (P_useBackUV > 0.5 && !gl_FrontFacing) ? v_uv1 : v_uv;   // back faces read TEXCOORD_1 when on
    vec4 baseTex = texture(tex_base, uv);
    // Blush (Kaze): lerp the base colour towards the blush colour by the strength and the base texture's alpha.
    float blushMask = clamp(P_faceBlushStrength * baseTex.a, 0.0, 1.0);
    vec3 baseRgb = mix(baseTex.rgb, P_faceBlushColor, blushMask);
    vec3 N = normalize(v_normal);
    vec3 V = normalize(frame.camPos.xyz - v_world);
    vec3 L = normalize(frame.sunDir.xyz * P_lightDirMul);
    float NoV = clamp(dot(N, V), 0.0, 1.0);

    // Base colour: ambient lerped towards ambient + diffuse, then the base and toon (matcap) textures.
    vec3 baseColor = clamp(mix(P_ambientColor, P_ambientColor + P_diffuseColor, 0.6), 0.0, 1.0);
    baseColor = mix(baseColor, baseColor * baseRgb, P_baseTexFac);
    vec3 nVS = (frame.view * vec4(N, 0.0)).xyz;
    vec3 toonTex = atlasSample(REGION_MATCAP, nVS.xy * 0.5 + 0.5);
    baseColor = mix(baseColor, baseColor * toonTex, P_toonTexFac);

    // Shadow ramp: row faceRampRow at U = 0.003. The night row is the day row + 0.5. Day/night by the light's Y.
    float rampV = P_faceRampRow / 10.0 - 0.05;
    // dayOverride < 0 follows the sun; 0..1 forces night..day.
    float isDay = P_dayOverride < 0.0 ? (L.y + 1.0) * 0.5 : clamp(P_dayOverride, 0.0, 1.0);
    vec3 rampColor = mix(atlasSample(REGION_RAMP, vec2(0.003, rampV + 0.5)),
                         atlasSample(REGION_RAMP, vec2(0.003, rampV)), isDay);

    // SDF face shadow. Project the light onto the head's horizontal plane, then compare the light's angle
    // from the head's right vector with the SDF threshold, sampled from the side the light is on.
    bool fromFrame = frame.toonD.z > 0.5;
    vec3 fwd = fromFrame ? vec3(frame.toonD.x, 0.0, frame.toonD.y) : P_forwardVector;
    vec3 right = fromFrame ? cross(vec3(0.0, 1.0, 0.0), fwd) : P_rightVector;
    float sdf = 1.0;
    vec3 up = cross(fwd, right);
    float upLen2 = dot(up, up);
    if (upLen2 > 1e-12) {
        vec3 lightOnHead = L - dot(L, up) / upLen2 * up;
        float lightLen = length(lightOnHead);
        if (lightLen > 1e-5) {
            vec3 ld = lightOnHead / lightLen;
            float angle01 = acos(clamp(dot(ld, normalize(right)), -1.0, 1.0)) / 3.14159265;
            float exposeRight = step(angle01, 0.5);
            float valueR = pow(clamp(1.0 - angle01 * 2.0, 0.0, 1.0), 3.0);
            float valueL = pow(clamp(angle01 * 2.0 - 1.0, 0.0, 1.0), 3.0);
            float mixValue = mix(valueL, valueR, exposeRight);
            float sdfLeft = texture(tex_faceSdf, vec2(1.0 - uv.x, uv.y)).r;
            float sdfRight = texture(tex_faceSdf, uv).r;
            float mixSdf = mix(sdfRight, sdfLeft, exposeRight);
            // Softness 0 is a hard edge; above 0 the edge is smoothed over 0.05 * softness either side of the offset threshold.
            float edgeW = P_faceShadowSoftness * 0.05;
            float thr = mixValue + P_faceShadowOffset;
            float sdfRaw = edgeW > 0.0 ? smoothstep(thr - edgeW, thr + edgeW, mixSdf) : step(thr, mixSdf);
            // Light behind the head: fully lit.
            sdf = mix(1.0, sdfRaw, step(0.0, dot(ld, normalize(fwd))));
            vec4 shadowTex = texture(tex_faceSdf, uv);
            sdf *= shadowTex.g;
            sdf = mix(sdf, 1.0, shadowTex.a);
        }
    }

    vec3 shadowColor = baseColor * rampColor * P_shadowColor;
    vec3 diffuse = mix(shadowColor, baseColor, sdf);
    float fresnel = pow(1.0 - NoV, P_rimPower);
    diffuse += fresnel * P_rimIntensity * P_rimColor;

    float alpha = P_alpha * baseTex.a;
    if (alpha < P_alphaClip) discard;
    o_color = vec4(diffuse, alpha);
}
