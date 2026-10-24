// Cascaded shadow map lookup. Cascade i lives in quadrant i of a 2x2 atlas.
//
// Two filters share the map. The default is a 3x3 tent percentage-closer filter built from four gathers, which gives
// every shadow the same one-texel edge. When frame.shadowSoft.y is set, percentage-closer soft shadows replace it: the
// blocker search and the penumbra it sizes are the ones in src/engine/pcss.h, and docs/PCSS.md measures both.
// Include AFTER frame.glsl.
TEX(0, 1) uniform sampler2D uShadowMap;

// The tap pattern the two PCSS passes use: a Vogel spiral, deterministic and evenly spread for any count, so the CPU
// reference (vogelTap in src/engine/pcss.h) and this shader draw the same points.
vec2 vogelTap(int i, int n) {
    const float kGoldenAngle = 2.39996323;
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * kGoldenAngle;
    return vec2(r * cos(a), r * sin(a));
}

// PCSS, in the atlas's uv. `zReceiver` is the receiver's depth in the shadow matrix's z; `quad` is this cascade's
// quadrant corner; `L` is the direction towards the sun.
//
// The two distances the technique needs are both in the map's own units, so nothing here depends on the cascade's world
// size being what the CPU reference assumed:
//   uv per metre        = row 0 of the shadow matrix, half a cascade across the atlas
//   z per metre         = row 2, an orthographic projection's depth scale
// A world distance across the receiver is not a distance on the light's plane: along the sun's azimuth a metre of floor
// foreshortens to |N.L| metres, so the filter is scaled by it. That is `facing` below, and src/engine/pcss.h has the
// long version of why it matters.
float pcssShadow(vec2 uv, float zReceiver, vec2 quad, int cascade, vec3 N, vec3 L) {
    float uvPerMetre = 0.25 * max(length(frame.shadowMatrix[cascade][0].xyz), 1e-9);
    float zPerMetre = max(abs(frame.shadowMatrix[cascade][2].z), 1e-9);
    float facing = sqrt(max(1.0 - dot(N, L) * dot(N, L), 0.0));
    float scale = uvPerMetre * max(facing, 1e-3);
    vec2 uvLo = quad + vec2(0.001), uvHi = quad + vec2(0.499);

    // 1. Blockers: the average depth of the texels in front of this surface. A texel nearer than the receiver by less
    // than the receiver's own slope over that offset is the receiver's plane, not a blocker -- without that every floor
    // shadows itself with its neighbours and the penumbra comes out several times too wide.
    float search = frame.shadowSoft.z * scale;
    float blocker = 0.0;
    int found = 0;
    for (int i = 0; i < 16; ++i) {
        vec2 off = vogelTap(i, 16) * search;
        float d = texture(uShadowMap, clamp(uv + off, uvLo, uvHi)).r;
        float slack = frame.shadowParams.w + facing * (length(off) / scale) * zPerMetre;
        if (d < zReceiver - slack) {
            blocker += d;
            ++found;
        }
    }
    if (found == 0) return 1.0;   // nothing between this point and the sun

    // 2. The penumbra that blocker casts here: a disc of angular radius tanRadius at the sun, the gap along the light,
    // and the cap that keeps a distant caster from covering the screen with grey.
    float gap = max(zReceiver - blocker / float(found), 0.0) / zPerMetre;
    float width = min(2.0 * frame.shadowSoft.x * gap, frame.shadowSoft.w) * scale;

    int lit = 0;
    for (int i = 0; i < 32; ++i) {
        vec2 off = vogelTap(i, 32) * width;
        float d = texture(uShadowMap, clamp(uv + off, uvLo, uvHi)).r;
        float slack = frame.shadowParams.w + facing * (length(off) / scale) * zPerMetre;
        lit += (d >= zReceiver - slack) ? 1 : 0;
    }
    return float(lit) / 32.0;
}

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
    // Receiver offset. A normal offset alone is not enough for a height field lit at a grazing angle: the depth error
    // across one shadow texel grows as 1 / N.L, so a hillside shadows itself in stripes. Push the sample along the
    // light by a few texels' worth of world space, which is the standard slope-scaled receiver bias. The cascade's
    // world size per texel is recovered from the shadow matrix: clip x spans [-1, 1] over 2 * radius metres.
    uint cascadeSize = uint(0.5 / max(frame.shadowParams.y, 1e-9));   // kShadowAtlas / 2, from 1 / atlas size
    float mRow0 = max(length(frame.shadowMatrix[cascade][0].xyz), 1e-9);
    float texelWorld = 2.0 / (mRow0 * float(cascadeSize));
    vec3 p = worldPos + N * frame.shadowParams.z * (1.0 - ndl) +
             L * (texelWorld * 2.0 / max(ndl, 0.25) + texelWorld * 0.5);
    vec4 lp = frame.shadowMatrix[cascade] * vec4(p, 1.0);
    vec3 sc = lp.xyz / lp.w;                      // xy in [-1,1], z in [0,1]
    vec2 uv = sc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;

    vec2 quad = vec2(float(cascade % 2), float(cascade / 2)) * 0.5;
    uv = clamp(quad + uv * 0.5, quad + vec2(0.001), quad + vec2(0.499));

    float texel = frame.shadowParams.y;
    float z = sc.z - frame.shadowParams.w;
    // Soft shadows first: they read the map at a radius the blocker's distance decides, so the tent filter's own
    // budget of four gathers does not apply.
    // Unbiassed: the blocker search applies the bias itself, as a distance, against the receiver's own slope.
    if (frame.shadowSoft.y > 0.5) return pcssShadow(uv, sc.z, quad, cascade, N, L);
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
