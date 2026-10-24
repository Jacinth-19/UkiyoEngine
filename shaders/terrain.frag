// Terrain surface (item 18, steps 1, 2 and 2b, production pass for item 19). Each layer is a cell of two atlases: the
// diffuse atlas (rgb albedo, alpha roughness) and the normal atlas (rgb tangent-space normal, alpha ambient occlusion).
// Layers are tiled over the world in XZ. The blend weights come from the vertex. The albedo, the roughness, the normal
// and the occlusion are blended, then lit with a toon ramp whose edge follows the roughness.
//
// Three things stop a tiled ground from reading as a tiled ground, which is what separates this from a test scene:
//   * the occlusion map (alpha of the normal atlas) darkens only the sky light, so crevices keep their shape,
//   * a detail overlay tiled at ~1.7 m breaks up the repeat under the camera and fades out with distance,
//   * a low-frequency macro variation tints the albedo, so a field has patches of darker and lighter growth.
// The tile also casts into the cascaded shadow atlas (shaders/terrain_shadow.vert), so its own hills shade it and the
// grass standing on it.
#include "common.glsl"
#include "frame.glsl"
#include "shadow_sample.glsl"

// Binding 1 is the shadow atlas in set 0, and the GL backend has one flat texture-unit space for every set, so the
// ShaderMaterial textures start at 2 (see the note in water.frag).
TEX(1, 0) uniform sampler2D uTerrainDiffuse;   // 4 by 2 cells: rgb albedo, a roughness
TEX(1, 2) uniform sampler2D uTerrainNormal;    // 4 by 2 cells: tangent-space normal, a occlusion
TEX(1, 3) uniform sampler2D uTerrainDetail;    // one repeat per `detail.x` metres; white when there is no overlay

UBO(1, 15) TerrainLayers {
    vec4 layer[8];   // rgb tint, multiplied into the diffuse
    vec4 tile[8];    // x: metres of ground per texture repeat
    vec4 detail;     // x: metres per detail repeat, y: detail strength, z: macro variation
} terrainLayers;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vN;
layout(location = 2) in vec4 vWeightsLo;
layout(location = 3) in vec4 vWeightsHi;
layout(location = 0) out vec4 outColor;

const vec2 kCell = vec2(0.25, 0.5);            // one cell in atlas uv (4 by 2 cells)
const vec2 kTexel = vec2(1.0 / 2048.0, 1.0 / 1024.0);

void main() {
    vec3 n0 = normalize(vN);
    // Derivatives are taken here, in uniform control flow. Inside the layer loop the flow is not uniform.
    vec3 dpx = dFdx(vWorld);
    vec3 dpy = dFdy(vWorld);
    vec2 dwx = dFdx(vWorld.xz);
    vec2 dwy = dFdy(vWorld.xz);

    const vec2 margin = kTexel * 1.5;            // keeps bilinear taps inside the cell
    const vec2 inner = kCell - 2.0 * margin;

    vec3 albedo = vec3(0.0);
    float rough = 0.0;
    float ao = 0.0;
    vec3 nSum = vec3(0.0);
    for (int k = 0; k < 8; ++k) {
        float w = k < 4 ? vWeightsLo[k] : vWeightsHi[k - 4];
        if (w < 0.001) continue;
        float tile = terrainLayers.tile[k].x;
        vec2 cell = vec2(float(k % 4), float(k / 4)) * kCell;
        vec2 tuv = vWorld.xz / tile;
        vec2 auv = cell + margin + fract(tuv) * inner;
        vec2 gx = (dwx / tile) * inner;          // explicit gradients: no seams where the repeat wraps
        vec2 gy = (dwy / tile) * inner;
        vec4 d = textureGrad(uTerrainDiffuse, auv, gx, gy);
        vec4 nTex = textureGrad(uTerrainNormal, auv, gx, gy);
        vec3 nTs = nTex.xyz * 2.0 - 1.0;
        albedo += w * terrainLayers.layer[k].rgb * d.rgb;
        rough += w * d.a;
        ao += w * nTex.a;

        // Tangent frame from the world derivatives and this layer's uv derivatives (cotangent frame).
        vec2 duvx = dwx / tile;
        vec2 duvy = dwy / tile;
        vec3 dp2perp = cross(dpy, n0);
        vec3 dp1perp = cross(n0, dpx);
        vec3 T = dp2perp * duvx.x + dp1perp * duvy.x;
        vec3 B = dp2perp * duvx.y + dp1perp * duvy.y;
        float m = max(dot(T, T), dot(B, B));
        vec3 nWs = n0;
        if (m > 1e-20) nWs = normalize(mat3(T * inversesqrt(m), B * inversesqrt(m), n0) * nTs);
        nSum += w * nWs;
    }

    // Detail overlay: the same texture at two scales. Near the camera it is ground clutter on top of the layer, and
    // its low-frequency octave tints the whole tile so a single layer is not one flat colour across a valley.
    float metres = max(terrainLayers.detail.x, 0.05);
    vec3 det = textureGrad(uTerrainDetail, vWorld.xz / metres, dwx / metres, dwy / metres).rgb;
    float detLum = dot(det, vec3(0.299, 0.587, 0.114));
    float viewDist = distance(frame.camPos.xyz, vWorld);
    float close = 1.0 - smoothstep(18.0, 45.0, viewDist);
    albedo *= mix(vec3(1.0), vec3(0.55 + 0.9 * detLum), close * terrainLayers.detail.y);
    float macroA = textureGrad(uTerrainDetail, vWorld.xz * 0.013 + vec2(0.31, 0.77), dwx * 0.013, dwy * 0.013).g;
    float macroB = textureGrad(uTerrainDetail, vWorld.xz * 0.047 - vec2(0.11, 0.43), dwx * 0.047, dwy * 0.047).r;
    albedo *= 1.0 + (macroA * 0.6 + macroB * 0.4 - 0.5) * terrainLayers.detail.z;

    vec3 n = length(nSum) > 1e-5 ? normalize(nSum) : n0;
    vec3 L = normalize(frame.sunDir.xyz);
    float ndl = max(dot(n, L), 0.0);
    float edge = mix(0.03, 0.25, clamp(rough, 0.0, 1.0));
    float ramp = smoothstep(0.15 - edge, 0.15 + edge, ndl);
    // Occlusion darkens the sky light only: the sun is already blocked by the shadow map, and multiplying both would
    // turn every crevice black.
    vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * n.y) * (0.45 + 0.55 * clamp(ao, 0.0, 1.0));
    // The tile casts into the same cascade atlas the meshes use (see the shadow pass), so its own hills shade it and
    // anything standing on it.
    float sun = ramp * shadowFactor(vWorld, n);
    outColor = vec4(albedo * (hemi + frame.sunColor.rgb * sun), 1.0);
}
