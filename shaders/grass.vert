// Grass (item 19), adapted from MagicGrass. Three vec4 rows per blade hold its world matrix, exactly like a foliage
// batch, so the shader rebuilds the blade from three dots. What the reference builds in a compute shader happens here:
//  * the distance bands: a stable per-blade hash thins the field as it recedes, so a near view is a superset of a far
//    one and blades appear as the camera approaches instead of flickering in and out (see grassDensityAt in grass.cpp),
//  * the wind: two travelling waves (one along the prevailing wind, one across it) sway the blade, the sway starts above
//    the root and grows to the tip, and the blade is kept inextensible, so a bent blade is a bend and not a stretch,
//  * the interaction push: a body in the grass (pc.p2) pushes the blades radially away from it.
// The scatter, the detail map and the terrain-layer masks are CPU work, once per tile: Ukiyo has no compute stage.
#include "common.glsl"
#include "frame.glsl"

UBO(1, 15) GrassRows {
    vec4 row[3 * 256];
    vec4 base;   // rgb root colour, a tip darkening
    vec4 tip;    // rgb tip colour, a translucency
} grassBlock;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;     // y runs 0 at the root to 1 at the tip

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vN;
layout(location = 2) out float vHeight;
layout(location = 3) out float vFade;
layout(location = 4) out float vRand;

// Must match grassBladeHash in src/engine/grass.cpp.
float bladeHash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Must match grassDensityAt in src/engine/grass.cpp: 1 inside bandNear, 0.5 at bandMid, 0 at cullDistance.
float bandDensity(float d, vec3 bands) {
    float nearBand = min(bands.x, bands.y);
    float midBand = max(bands.x, bands.y);
    float cull = max(bands.z, midBand + 0.001);
    if (d <= nearBand) return 1.0;
    if (d <= midBand) return mix(1.0, 0.5, smoothstep(nearBand, midBand, d));
    return 0.5 * (1.0 - smoothstep(midBand, cull, d));
}

void main() {
    uint i = uint(INSTANCE_ID);
    vec4 r0 = grassBlock.row[3 * i];
    vec4 r1 = grassBlock.row[3 * i + 1];
    vec4 r2 = grassBlock.row[3 * i + 2];
    vec3 origin = vec3(r0.w, r1.w, r2.w);
    vec3 root = (pc.model * vec4(origin, 1.0)).xyz;

    float d = distance(frame.camPos.xyz, root);
    float keep = bladeHash(origin.xz);
    if (keep > bandDensity(d, pc.p0.xyz)) {
        // Thinned out at this distance. One point outside the clip volume, so the whole blade is clipped away.
        vWorld = root;
        vN = vec3(0.0, 1.0, 0.0);
        vHeight = 0.0;
        vFade = 0.0;
        vRand = keep;
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    vec3 p = vec3(dot(r0, vec4(inPos, 1.0)), dot(r1, vec4(inPos, 1.0)), dot(r2, vec4(inPos, 1.0)));
    vec3 n = vec3(dot(r0.xyz, inNormal), dot(r1.xyz, inNormal), dot(r2.xyz, inNormal));

    float t = clamp(inUV.y, 0.0, 1.0);
    // The sway starts above the root and reaches full strength at the tip: the base stays planted.
    float curve = smoothstep(0.15, 1.0, t);

    // pc.p1 = wind strength (m), frequency (cycles/s), spatial scale (rad/m), flutter (m).
    // pc.p3 = the prevailing ground direction of the wind.
    vec2 dir = normalize(pc.p3.xy + vec2(1e-4, 0.0));
    vec2 across = vec2(-dir.y, dir.x);
    float time = frame.screenParams.z;
    float phase = time * pc.p1.y * 6.2831853;
    // Two waves: one rolling along the wind, one crossing it at a different rate. Together they read as gusts moving
    // over the field rather than every blade pumping in place. The per-blade phase keeps a tuft from moving as one.
    float w1 = sin(dot(root.xz, dir) * pc.p1.z + phase);
    float w2 = sin(dot(root.xz, across) * pc.p1.z * 1.7 - phase * 0.62 + keep * 6.2831853);
    float gust = mix(0.30, 1.0, (w1 * 0.5 + 0.5) * (w1 * 0.5 + 0.5));   // long lulls, short gusts
    vec2 sway = dir * (w1 * pc.p1.x * gust) + across * (w2 * (pc.p1.x * 0.45 + pc.p1.w));

    // pc.p2 = the body in the grass: xy its ground position, z the reach in metres, w the push in metres.
    if (pc.p2.w > 0.0) {
        vec2 away = root.xz - pc.p2.xy;
        float len = length(away);
        float infl = 1.0 - smoothstep(pc.p2.z * 0.35, pc.p2.z, len);
        if (len > 1e-4) sway += (away / len) * (pc.p2.w * infl);
    }

    vec3 off = p - origin;
    vec3 bent = off + vec3(sway.x, 0.0, sway.y) * curve;
    // An inextensible blade: a vertex keeps its distance from the root, so the tip arcs over instead of stretching.
    float len = length(off);
    if (len > 1e-4) bent = normalize(bent) * len;
    p = origin + bent;

    // Blades that are a few pixels wide shimmer as they recede. Widening them with distance trades a little accuracy
    // for a stable field, the same trick the reference uses (its local x stretch of viewWSLength * 0.003).
    float widen = 1.0 + max(0.0, d - 8.0) * 0.0035;
    p.xz = origin.xz + (p.xz - origin.xz) * widen;

    // The normal follows the bend, so a bent blade catches the light along its length.
    n = normalize(n - vec3(sway.x, 0.0, sway.y) * (curve * 1.6));

    vec4 world = pc.model * vec4(p, 1.0);
    vWorld = world.xyz;
    vN = mat3(pc.model) * n;
    vHeight = t;
    vRand = keep;
    // The outermost band stipples away instead of ending on a line; the fragment shader dithers with this value.
    vFade = 1.0 - smoothstep(pc.p0.z * 0.82, pc.p0.z, d);
    gl_Position = frame.viewProj * world;
}
