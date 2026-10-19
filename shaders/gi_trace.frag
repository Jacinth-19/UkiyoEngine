// Real-time diffuse GI: the trace pass.
//
// One half-resolution pixel, N cosine-weighted rays into the heightfield, and the hit's radiance read from this
// frame's raster. This is the iGPU answer to the reference's traced GI: no RT cores, no BVH, no compute — a fragment
// shader, a height function, and eight short marches.
//
// The heightfield is the scene. Terrain is a heightmap, so a ray does not need an acceleration structure: it marches
// with a step that grows with the height above the ground and refines the hit by bisection, exactly as
// tools/pathtrace_preview.cpp does. Entries that a heightfield cannot express (props, characters) simply do not
// occlude the bounce; the engine's SSAO still darkens their contact.
//
// What the pass writes is the *difference* between the traced bounce and the ambient the raster already added:
//
//     out = albedo * (mean incoming radiance over the hemisphere - unoccluded hemisphere ambient)
//
// The raster frame contains `albedo * hemi(n)` for the sky, so adding the full traced integral would count the sky
// twice and the scene would brighten whenever GI was switched on. Subtracting the same hemisphere model the terrain
// shader uses (frame.skyColor / frame.groundColor) makes an open, flat surface land on zero and leaves only occlusion
// and the bounce. The albedo is the terrain's own, averaged to the heightfield's resolution and carried in the
// height texture's G, B and A channels (see uploadTerrain in src/engine/terrain_gpu.cpp).
//
// Inputs (Post set, set 1 in the pipeline layout because the Frame set is set 0):
//   uHeight  R = height normalised to heightScale, GBA = albedo in 0..1, one texel per heightfield sample
//   uRaster  this frame's HDR raster; the radiance a bounce ray lands on
//   uDepth   scene depth, uNormal view-space normal (alpha 1 = surface)
//
// Push constants:
//   model = inverse(viewProj)                      depth -> world
//   p0    = (heightCellSize, heightScale, marchDistance, hitBias)
//   p1    = (raysPerPixel, 0, 0, 0)
//   p2    = (tileOriginX, tileOriginZ, tileOriginY, frameIndex)
//
// The camera basis and the ambient colours come from the Frame set, which this pass also binds.
#include "common.glsl"

#include "frame.glsl"
TEX(1, 2) uniform sampler2D uHeight;    // R height, GBA albedo (linear)
TEX(1, 3) uniform sampler2D uRaster;    // this frame's HDR raster
TEX(1, 4) uniform sampler2D uDepth;     // scene depth
TEX(1, 5) uniform sampler2D uNormal;    // view-space normal
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float hash21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

// Metres from the tile's first sample. The engine's terrain instances translate a tile built at the origin; they do
// not rotate or scale it, which is the assumption the whole heightfield estimator is built on.
vec2 tileLocal(vec2 xz) { return xz - pc.p2.xy; }

// World xz -> the height texture's uv. Sample i sits at texel centre (i + 0.5) / size.
vec2 heightUV(vec2 xz) {
    float size = float(textureSize(uHeight, 0).x);
    return clamp((tileLocal(xz) / max(pc.p0.x, 1e-3) + 0.5) / size, vec2(0.0), vec2(1.0));
}

float heightMetres(vec2 xz) {
    return texture(uHeight, heightUV(xz)).r * pc.p0.y + pc.p2.z;
}

// The terrain's diffuse colour, averaged to the heightfield's resolution.
vec3 albedoAt(vec2 xz) {
    return max(texture(uHeight, heightUV(xz)).gba, vec3(0.015));
}

vec3 normalAt(vec2 xz) {
    float e = 0.35;
    float hl = heightMetres(xz - vec2(e, 0.0)), hr = heightMetres(xz + vec2(e, 0.0));
    float hd = heightMetres(xz - vec2(0.0, e)), hu = heightMetres(xz + vec2(0.0, e));
    return normalize(vec3(hl - hr, 2.0 * e, hd - hu));
}

// The raster's ambient model, integrated over the cosine hemisphere. frame.skyColor / groundColor are the same
// colours shaders/terrain.frag mixes, so a ray that escapes returns exactly what the raster added for a sky sample
// and the difference is zero on open ground. For a linear function of d.y, the cosine-weighted mean is
// 0.5 + n.y / 3 (the hemisphere average of d.y is 2/3).
vec3 skyRadiance(vec3 d) { return mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * d.y); }
vec3 hemiAmbient(vec3 n) { return mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + n.y / 3.0); }

// Adaptive march plus bisection: the step grows with the height above the ground, so empty air costs almost nothing
// and the hit lands within a few centimetres. A heightfield is one height per xz, so the march can never step *over*
// a ridge it is heading into; that bound is the whole reason this costs a handful of taps instead of a BVH
// traversal, and it is why an iGPU can run it.
bool march(vec3 ro, vec3 rd, float tmax, out float tout, out vec3 nout) {
    const int kSteps = 48;
    float t = 0.02;
    float size = float(textureSize(uHeight, 0).x);
    float extent = pc.p0.x * (size - 1.0);
    for (int i = 0; i < kSteps; ++i) {
        if (t >= tmax) return false;
        vec3 p = ro + rd * t;
        vec2 local = tileLocal(p.xz);
        if (local.x < -4.0 || local.y < -4.0 || local.x > extent + 4.0 || local.y > extent + 4.0) return false;
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
        // Bound the step by how far the ray can fall before it reaches the ground, and by a cap so a ray aimed at
        // the sky does not spend the whole budget stepping upward. 0.6 m reaches the far end of a 12 m ray in 20 taps.
        float air = p.y - h;
        t += clamp(min(air * 0.55, air / max(0.08, -rd.y + 0.08)), 0.08, 0.6);
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

    // World normal. Props and characters are in the normal pre-pass, encoded as view-space * 0.5 + 0.5; terrain is
    // not (it is drawn after the opaque list), so a pixel with no pre-pass normal takes the heightfield's own normal
    // — which is also the surface the rays are marching, so the two agree by construction.
    vec4 nS = texture(uNormal, vUV);
    vec3 Nd = nS.a > 0.5 ? normalize(mat3(frame.view) * normalize(nS.xyz * 2.0 - 1.0)) : normalAt(P.xz);
    vec3 albedo = albedoAt(P.xz);
    float n = max(pc.p1.x, 1.0);
    float radius = pc.p0.z;
    float bias = pc.p0.w;
    vec3 sum = vec3(0.0);
    // A per-pixel, per-frame offset: the rays rotate a little every frame so the accumulation converges on the
    // integral instead of on one fixed set of directions.
    float seed = hash21(gl_FragCoord.xy + pc.p2.w * 1.618034);
    for (int i = 0; i < 8; ++i) {
        if (float(i) >= n) break;
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
            // The radiance leaving the hit surface, from this frame's raster. Reading the raster rather than the
            // previous frame's composited image keeps the estimator from feeding its own output back into the rays,
            // which would amplify the bounce every frame.
            vec3 hp = P + Nd * bias + d * t;
            vec4 hc = frame.viewProj * vec4(hp, 1.0);
            vec2 uv = (hc.xy / max(hc.w, 1e-4)) * 0.5 + 0.5;
            sum += (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) ? texture(uRaster, uv).rgb
                                                                                   : skyRadiance(d);
        } else {
            sum += skyRadiance(d);
        }
    }
    // The delta, not the integral: what the raster's ambient did not already account for.
    outColor = vec4(albedo * (sum / n - hemiAmbient(Nd)), 1.0);
}
