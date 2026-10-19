// Screen-space effects on the HDR frame, run before TAA, bloom and tonemap.
// Inputs: uHdr scene HDR; uNormal view-space normals (alpha 1 = surface; written only when a normal
// effect is on); uVel motion vectors in UV units (written only when motion blur is on); uDepth scene depth.
// Push constants packed into the model matrix, p0 and p1:
//   model[0] = (P00, P11, P22, P32)   eye distance = P32 / (d + P22)
//   model[1] = (fog colour rgb, fog density per metre)
//   model[2] = (sun direction in view space, sun in-scatter strength)
//   model[3] = (sun screen uv, lens flare amount, SSAO strength)
//   p0 = (SSR strength, depth-normal outline strength, motion blur amount, aspect ratio)
//   p1 = (SSAO radius in metres, screen-space rim strength, outline noise amount, depth-edge rim strength)
//   p2 = (split direction, unit, split offset px, split line width px); zero direction = no split line
//   p3 = (NPR side tint, PBR side tint, line colour) packed, and p3.w = traced GI strength (0 = none this frame)
//   p3.w = traced GI strength
// With everything at its default the pass copies the frame.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uHdr;
TEX(0, 2) uniform sampler2D uNormal;
TEX(0, 3) uniform sampler2D uVel;
TEX(0, 4) uniform sampler2D uDepth;
TEX(0, 5) uniform sampler2D uGi;       // half-resolution traced GI, read only when p2.x > 0
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float hash21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

float eyeDist(float d) { return pc.model[0].w / (d + pc.model[0].z); }

vec3 viewPos(vec2 uv, float d) {
    float z = eyeDist(d);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / pc.model[0].x, ndc.y * z / pc.model[0].y, -z);
}

vec2 projectUV(vec3 p) {
    float z = max(-p.z, 1e-4);
    return vec2(pc.model[0].x * p.x, pc.model[0].y * p.y) / z * 0.5 + 0.5;
}

// Occlusion 0..1: the share of hemisphere samples that land inside nearby geometry.
float ssao(vec2 uv, vec3 P, vec3 N, float radius) {
    float rnd = hash21(gl_FragCoord.xy) * 6.2831853;
    float occ = 0.0;
    const int taps = 12;
    for (int i = 0; i < taps; ++i) {
        float fi = float(i) + 0.5;
        float a = fi * 2.39996 + rnd;
        float r = sqrt(fi / float(taps));
        vec3 s = normalize(vec3(cos(a) * r, sin(a) * r, 1.0));
        if (dot(s, N) < 0.0) s = -s;
        vec3 q = P + s * radius * (0.25 + 0.75 * fi / float(taps));
        vec2 uv2 = projectUV(q);
        if (any(lessThan(uv2, vec2(0.0))) || any(greaterThan(uv2, vec2(1.0)))) continue;
        float sdRaw = texture(uDepth, uv2).r;
        if (sdRaw > 0.99999) continue;
        float sd = eyeDist(sdRaw);
        float qd = -q.z;
        float range = smoothstep(0.0, 1.0, radius / max(abs(-P.z - sd), 1e-3));
        occ += (sd < qd - 0.03 ? 1.0 : 0.0) * range;
    }
    return occ / float(taps);
}

// Edge 0..1 from depth and normal differences to the four neighbours.
float depthNormalEdge(vec2 uv, float d, vec3 N, vec2 px) {
    float d0 = eyeDist(d);
    const vec2 offs[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    float edge = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 q = uv + offs[i] * px;
        float dq = texture(uDepth, q).r;
        float dd = abs(eyeDist(dq) - d0) / max(d0, 1e-3);
        vec4 nq = texture(uNormal, q);
        vec3 n2 = nq.a > 0.5 ? normalize(nq.xyz * 2.0 - 1.0) : N;
        edge = max(edge, max(smoothstep(0.03, 0.12, dd), smoothstep(0.25, 0.55, length(n2 - N))));
    }
    return edge;
}

// Screen-space reflection: march the reflected ray through the depth buffer.
vec3 ssrColor(vec3 P, vec3 N) {
    vec3 R = reflect(normalize(P), N);
    if (R.z > 0.0) return vec3(0.0);            // heads back towards the camera
    for (int i = 1; i <= 16; ++i) {
        vec3 q = P + R * (0.1 * float(i));
        vec2 uv2 = projectUV(q);
        if (any(lessThan(uv2, vec2(0.01))) || any(greaterThan(uv2, vec2(0.99)))) break;
        float sdRaw = texture(uDepth, uv2).r;
        if (sdRaw > 0.99999) continue;
        float surf = eyeDist(sdRaw);
        float qd = -q.z;
        if (qd > surf + 0.02 && qd < surf + 0.5) {
            float edge = smoothstep(0.0, 0.12, min(min(uv2.x, uv2.y), min(1.0 - uv2.x, 1.0 - uv2.y)));
            return texture(uHdr, uv2).rgb * edge;
        }
    }
    return vec3(0.0);
}

void main() {
    vec2 size = vec2(textureSize(uHdr, 0));
    vec2 px = 1.0 / size;
    vec4 src = texture(uHdr, vUV);
    vec3 col = src.rgb;

    // Motion blur along the velocity, six taps either side of the pixel.
    float mb = pc.p0.z;
    if (mb > 0.0) {
        vec2 v = texture(uVel, vUV).rg * mb;
        vec3 acc = col * 0.25;
        for (int i = 1; i <= 6; ++i) acc += texture(uHdr, vUV - v * (float(i) / 6.0 - 0.5)).rgb * 0.125;
        col = acc;
    }

    float d = texture(uDepth, vUV).r;
    bool surface = d < 0.99999;
    vec4 nS = texture(uNormal, vUV);
    bool hasN = nS.a > 0.5;
    vec3 N = hasN ? normalize(nS.xyz * 2.0 - 1.0) : vec3(0.0, 0.0, 1.0);
    vec3 P = viewPos(vUV, d);

    if (surface && hasN && pc.model[3].w > 0.0) {
        float occ = ssao(vUV, P, N, pc.p1.x);
        col *= 1.0 - clamp(occ * pc.model[3].w * 1.5, 0.0, 1.0);
    }
    if (surface && hasN && pc.p0.y > 0.0) {
        float edge = clamp(depthNormalEdge(vUV, d, N, px), 0.0, 1.0);
        // Outline noise (RealToon sketch noise, screen-space version): breaks the line into dashes.
        float dash = hash21(floor(gl_FragCoord.xy * 0.5));
        edge *= mix(1.0, step(0.35, dash), clamp(pc.p1.z, 0.0, 1.0));
        col *= 1.0 - edge * clamp(pc.p0.y, 0.0, 1.0);
    }
    if (surface && hasN && pc.p0.x > 0.0) {
        col += ssrColor(P, N) * pc.p0.x * 0.3;
    }
    if (surface && hasN && pc.p1.y > 0.0) {
        // Screen-space rim light: surfaces turned away from the camera (view-space normal close to
        // perpendicular to the view ray) take a cool edge. Scene-wide; not per material.
        float facing = clamp(N.z, 0.0, 1.0);
        float rim = smoothstep(0.35, 0.9, 1.0 - facing);
        col += vec3(0.55, 0.8, 1.0) * rim * clamp(pc.p1.y, 0.0, 1.0) * 0.35;
    }
    if (surface && hasN && pc.p1.w > 0.0) {
        // Depth-edge rim, Kaze's GetRim: the depth one pixel away along the view normal's x (RimOffset 1), the
        // eye-distance difference smoothed over 0..1 m (RimThreshold 1), times fresnel^5 from the normal and the view ray.
        // Scene-wide: applied to every surface here, not per material as in Kaze's forward pass.
        float dEye = eyeDist(d);
        float oEye = eyeDist(texture(uDepth, vUV + vec2(N.x * px.x, 0.0)).r);
        float rim = smoothstep(0.0, 1.0, oEye - dEye);
        vec3 V = normalize(-P);
        float fres = pow(clamp(1.0 - dot(N, V), 0.0, 1.0), 5.0);
        col += vec3(0.55, 0.8, 1.0) * rim * fres * clamp(pc.p1.w, 0.0, 1.0);
    }
    // Traced diffuse GI, composited at full resolution from the half-resolution accumulation. A 3x3 tent weighted by
    // depth, so the valley does not receive the bounce of the hill in front of it. This is the pass that makes the
    // renderer's ambient *traced* rather than a hemisphere guess: the same one bounce the CPU reference takes.
    if (surface && hasN && pc.p3.w > 0.0) {
        vec3 acc = vec3(0.0);
        float wsum = 0.0;
        float myEye = eyeDist(d);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec2 uv = vUV + vec2(float(x), float(y)) * px * 2.0;
                float sd = eyeDist(texture(uDepth, uv).r);
                float w = (x == 0 && y == 0 ? 2.0 : 1.0) / (1e-3 + abs(sd - myEye) * 2.0);
                acc += texture(uGi, uv).rgb * w;
                wsum += w;
            }
        }
        col += (acc / max(wsum, 1e-4)) * pc.p3.w;
    }
    if (surface && pc.model[1].w > 0.0) {
        float fogAmt = 1.0 - exp(-eyeDist(d) * pc.model[1].w);
        vec3 rd = normalize(P);
        float phase = pow(max(dot(rd, pc.model[2].xyz), 0.0), 8.0);
        vec3 fogCol = pc.model[1].rgb + vec3(1.0, 0.95, 0.85) * phase * pc.model[2].w;
        col = mix(col, fogCol, fogAmt);
    }
    float fl = pc.model[3].z;
    if (fl > 0.0) {
        vec2 s = pc.model[3].xy;
        float occ = (all(greaterThan(s, vec2(0.0))) && all(lessThan(s, vec2(1.0))) && texture(uDepth, s).r > 0.99999) ? 1.0 : 0.0;
        vec2 asp = vec2(pc.p0.w, 1.0);
        vec2 delta = (vUV - s) * asp;
        float r = length(delta);
        float streak = exp(-abs(delta.y) * 180.0) * exp(-abs(delta.x) * 5.0) * 0.12;
        float halo = exp(-r * 20.0) * 0.17;
        vec3 ghosts = vec3(0.0);
        for (int i = 1; i <= 4; ++i) {
            vec2 pos = mix(s, vec2(0.5), float(i) * 0.65);
            float g = exp(-length((vUV - pos) * asp) * float(35 + i * 15));
            ghosts += vec3(0.35 + float(i) * 0.12, 0.38, 0.70 - float(i) * 0.1) * g * 0.11;
        }
        col += (vec3(halo + streak) + ghosts) * occ * fl;
    }
    // SplitScreen line (shaders/SplitScreen.shader's look, on the materials' split convention). The side with
    // screen side > 0 takes the NPR tint, the other side the PBR tint, and a line is drawn on the split.
    vec2 sdir = pc.p2.xy;
    if (dot(sdir, sdir) > 1e-8) {
        vec2 res = vec2(textureSize(uHdr, 0));
        vec2 ctr = res * 0.5;
        // gl_FragCoord.y is bottom-up here (positive viewport, GL-style projection), so it is already y-up.
        vec2 px = gl_FragCoord.xy;
        vec2 perp = vec2(-sdir.y, sdir.x);
        float side = dot(px - ctr, perp) - pc.p2.z;
        vec3 nprTint = unpackUnorm4x8(floatBitsToUint(pc.p3.x)).rgb;
        vec3 pbrTint = unpackUnorm4x8(floatBitsToUint(pc.p3.y)).rgb;
        vec3 lineCol = unpackUnorm4x8(floatBitsToUint(pc.p3.z)).rgb;
        col *= side > 0.0 ? nprTint : pbrTint;
        float width = max(pc.p2.w, 1e-3);
        float lineMask = 1.0 - smoothstep(0.0, width, abs(side));
        col = mix(col, lineCol, lineMask);
    }
    outColor = vec4(max(col, vec3(0.0)), src.a);
}
