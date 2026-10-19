// Real-time diffuse GI: the trace pass.
//
// One half-resolution pixel, N cosine-weighted rays into the heightfield, and the hit's radiance read from the last
// frame's raster result. This is the iGPU answer to the reference's traced GI: no RT cores, no BVH, no compute — a
// fragment shader, a height function, and eight short marches.
//
// The heightfield is the scene. Terrain is a heightmap, so a ray does not need an acceleration structure: it marches
// with a step that grows with the height above the ground and refines the hit by bisection, exactly as
// tools/pathtrace_preview.cpp does. Entries that a heightfield cannot express (props, characters) simply do not
// occlude the bounce; the engine's SSAO still darkens their contact.
//
// Inputs:  uTex0 previous frame's HDR raster (full resolution), uDepth scene depth, uNormal view-space normals.
// Output:  RGBA16F half-resolution GI in linear radiance, alpha = mean albedo for the composite.
//
// Push constants (five vectors, so the whole pass fits the engine's 128-byte push block):
//   model = inverse(viewProj)                       depth -> world
//   (frame.viewProj, frame.view and frame.camPos are in the Frame set, which this pass also binds: the camera basis
//   turns the view-space normals into world space and the projection turns a hit back into screen UV.)
//   p0    = (heightCellSize, heightScale, marchDistance, hitBias)
//   p1    = (sunDir.xyz, raysPerPixel)
//   p2    = (frame, noiseSeed, 0, 0)
//   p2    = (frame, noiseSeed, reprojectScale, 0)
//
// Reprojection: the pass does not own a previous viewProj, and the Post set has no uniform buffer to put one in. The
// camera's motion between two frames is small, so the previous frame's UV is approximated from the current one by
// the per-frame screen-space offset the renderer computes on the CPU (pc.p2.z applies it as a scale on the offset
// from the screen centre). The error is bounded by the camera's own rotation over one frame and the accumulation
// pass discards history where depth disagrees.
//
// `model` carries inverse(viewProj) so a depth value can be turned into a world position here, the same way
// effects.frag does it. The heightmap texture is the uHeight sampler in slot 2, R32F, with the tile at texel centres.
#include "common.glsl"

#include "frame.glsl"
TEX(0, 3) uniform sampler2D uPrev;      // previous frame's raster HDR (binding 3: the GL backend shares one unit
                                        // space across sets, and binding 1 is the Frame set's shadow atlas)
TEX(0, 2) uniform sampler2D uHeight;    // heightmap, R channel, metres
TEX(0, 3) uniform sampler2D uDepth;     // scene depth
TEX(0, 4) uniform sampler2D uNormal;    // view-space normal, alpha 1 = surface
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float hash21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

// The inverse ACES curve the sky is authored through (see tools/pathtrace_preview.cpp). Authoring the sky in display
// space and converting back is what keeps the horizon the colour it was designed to be.
float inverseAces(float y) {
    y = clamp(y, 0.0, 0.995);
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    float A = c * y - a, B = d * y - b, C = e * y;
    float disc = B * B - 4.0 * A * C;
    if (disc < 0.0) return 0.0;
    float sq = sqrt(disc);
    return max(0.0, max((-B + sq) / (2.0 * A), (-B - sq) / (2.0 * A)));
}
vec3 fromDisplay(vec3 c) {
    c = pow(clamp(c, 0.0, 1.0), vec3(2.2));
    return vec3(inverseAces(c.r), inverseAces(c.g), inverseAces(c.b));
}
// The sky of the estimator. Cheaper than the tracer's: the cirrus deck is two noise octaves instead of five and the
// sun glow is a single lobe. It is sampled by rays that leave the heightfield, so it has to be the same colour as
// the sky the raster pass draws or the bounce will not match it.
vec3 sky(vec3 d, vec3 sunDir) {
    float up = clamp(d.y, 0.0, 1.0);
    vec3 disp = mix(vec3(0.90, 0.78, 0.62), vec3(0.18, 0.38, 0.68), pow(up, 0.42));
    disp = mix(disp, vec3(0.94, 0.89, 0.82), exp(-up * 11.0) * 0.45);
    float cs = max(dot(d, sunDir), 0.0);
    disp += vec3(1.0, 0.86, 0.66) * (pow(cs, 900.0) * 0.55 + pow(cs, 24.0) * 0.10 + pow(cs, 4.0) * 0.035);
    return fromDisplay(disp);
}

float heightMetres(vec2 xz) {
    // The heightmap is stored normalised; the tile is heightCellSize metres per texel and heightScale metres tall.
    float texels = float(textureSize(uHeight, 0).x - 1);
    vec2 uv = clamp(xz / max(pc.p0.x, 1e-3) / max(texels, 1.0), vec2(0.0), vec2(1.0));
    return texture(uHeight, uv).r * pc.p0.y;
}

vec3 normalAt(vec2 xz) {
    float e = 0.35;
    float hl = heightMetres(xz - vec2(e, 0.0)), hr = heightMetres(xz + vec2(e, 0.0));
    float hd = heightMetres(xz - vec2(0.0, e)), hu = heightMetres(xz + vec2(0.0, e));
    return normalize(vec3(hl - hr, 2.0 * e, hd - hu));
}

// Adaptive march plus bisection: the step grows with the height above the ground, so empty air costs almost nothing
// and the hit lands within a few centimetres.
bool march(vec3 ro, vec3 rd, float tmax, out float tout, out vec3 nout) {
    float t = 0.02;
    float limit = pc.p0.x * (float(textureSize(uHeight, 0).x) - 1.0) + 8.0;
    for (int i = 0; i < 64; ++i) {
        if (t >= tmax) return false;
        vec3 p = ro + rd * t;
        if (p.x < -8.0 || p.z < -8.0 || p.x > limit || p.z > limit) return false;
        float h = heightMetres(p.xz);
        if (p.y <= h) {
            float lo = max(0.0, t - 2.0), hi = t;
            for (int k = 0; k < 8; ++k) {
                float mid = 0.5 * (lo + hi);
                vec3 q = ro + rd * mid;
                if (q.y <= heightMetres(q.xz)) hi = mid; else lo = mid;
            }
            tout = hi;
            nout = normalAt((ro + rd * hi).xz);
            return true;
        }
        // A heightfield is one height per xz, so a march can never step *over* a ridge it is heading into: the step
        // below is bounded by how far the ray can fall before it reaches the ground. That is the whole reason this
        // costs a handful of taps instead of a BVH traversal, and it is why an iGPU can run it.
        t += max(0.08, min((p.y - h) * 0.55, max(0.1, (p.y - h) / max(0.08, -rd.y + 0.08))));
    }
    return false;
}

void main() {
    float rawDepth = texture(uDepth, vUV).r;
    if (rawDepth > 0.99999) { outColor = vec4(0.0); return; }   // sky: no surface to bounce from

    // Rebuild the surface point from the depth buffer and the inverse view-projection, at this half-resolution pixel.
    vec4 clip = vec4(vUV * 2.0 - 1.0, rawDepth, 1.0);
    vec4 world = pc.model * clip;
    vec3 P = world.xyz / world.w;

    // View-space normal -> world. The camera basis comes from the Post set's uniform, filled with the view matrix's
    // 3x3 on the CPU; a mat3 of inverse(viewProj) is not a rotation and cannot do this job.
    vec3 Nv = normalize(texture(uNormal, vUV).xyz);
    vec3 Nd = normalize(mat3(frame.view) * Nv);
    vec3 sunDir = pc.p1.xyz;
    float n = pc.p1.w;
    float radius = pc.p0.z;
    float bias = pc.p0.w;
    vec3 sum = vec3(0.0);
    float seed = hash21(gl_FragCoord.xy + pc.p2.z);
    for (int i = 0; i < 8; ++i) {
        if (float(i) >= n) break;
        // Cosine-weighted direction around the normal, with a stratified azimuth.
        float u1 = fract(seed + float(i) * 0.6180339);
        float u2 = fract(seed * 1.37 + float(i) * 0.7548776);
        float r = sqrt(u1);
        float phi = 6.2831853 * u2;
        vec3 tangent = normalize(abs(Nd.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), Nd) : cross(vec3(1.0, 0.0, 0.0), Nd));
        vec3 bitangent = cross(Nd, tangent);
        vec3 d = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + Nd * sqrt(max(0.0, 1.0 - u1)));
        float t = 0.0;
        vec3 hn;
        if (march(P + Nd * bias, d, radius, t, hn)) {
            // Project the hit into the previous frame's raster and take its radiance. The reprojection error this
            // introduces is bounded by the march's radius; when the hit is off screen the sky is the better guess.
            // Previous-frame UV from this frame's projection, shifted by the camera's screen-space motion.
            vec3 hp = P + Nd * bias + d * t;
            vec4 hc = frame.viewProj * vec4(hp, 1.0);
            vec2 uv = (hc.xy / max(hc.w, 1e-4)) * 0.5 + 0.5 - (vUV - 0.5) * (pc.p2.z - 1.0);
            sum += (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) ? texture(uPrev, uv).rgb : sky(d, sunDir);
        } else {
            sum += sky(d, sunDir);
        }
    }
    outColor = vec4(sum / max(n, 1.0), 1.0);
}
