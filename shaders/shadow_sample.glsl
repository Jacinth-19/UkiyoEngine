// Cascaded shadow map lookup. Cascade i lives in quadrant i of a 2x2 atlas.
// 3x3 tent percentage-closer filtering (four gathers). Include AFTER frame.glsl.
TEX(0, 1) uniform sampler2D uShadowMap;

float shadowFactor(vec3 worldPos, vec3 N) {
    if (frame.shadowParams.x < 0.5) return 1.0;
    vec3 L = normalize(frame.sunDir.xyz);
    float viewDist = -(frame.view * vec4(worldPos, 1.0)).z;
    int cascade = -1;
    if (viewDist < frame.cascadeSplits.x) cascade = 0;
    else if (viewDist < frame.cascadeSplits.y) cascade = 1;
    else if (viewDist < frame.cascadeSplits.z) cascade = 2;
    if (cascade < 0) return 1.0;

    float ndl = clamp(dot(N, L), 0.0, 1.0);
    vec3 p = worldPos + N * frame.shadowParams.z * (1.0 - ndl);
    vec4 lp = frame.shadowMatrix[cascade] * vec4(p, 1.0);
    vec3 sc = lp.xyz / lp.w;                      // xy in [-1,1], z in [0,1]
    vec2 uv = sc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;

    vec2 quad = vec2(float(cascade % 2), float(cascade / 2)) * 0.5;
    uv = clamp(quad + uv * 0.5, quad + vec2(0.001), quad + vec2(0.499));

    float texel = frame.shadowParams.y;
    float z = sc.z - frame.shadowParams.w;
    // 3x3 tent filter from four gathers. A gather at uv +- half a texel reads the 2x2 block of depths around
    // it, so the four blocks cover the same 3x3 texels a nine-tap filter reads, with weights 1-2-1 on each
    // axis. Four fetches instead of nine. The clamp above keeps every block inside this cascade's quadrant.
    vec4 g0 = textureGather(uShadowMap, uv + vec2(-0.5, -0.5) * texel);
    vec4 g1 = textureGather(uShadowMap, uv + vec2( 0.5, -0.5) * texel);
    vec4 g2 = textureGather(uShadowMap, uv + vec2(-0.5,  0.5) * texel);
    vec4 g3 = textureGather(uShadowMap, uv + vec2( 0.5,  0.5) * texel);
    float lit = dot(step(vec4(z), g0), vec4(1.0)) + dot(step(vec4(z), g1), vec4(1.0))
              + dot(step(vec4(z), g2), vec4(1.0)) + dot(step(vec4(z), g3), vec4(1.0));
    return lit / 16.0;
}
