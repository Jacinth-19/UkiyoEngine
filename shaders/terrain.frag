// Terrain surface (item 18, steps 1, 2 and 2b). Each layer is a cell of two atlases: the diffuse atlas (rgb albedo,
// alpha roughness) and the normal atlas (tangent-space normal). Layers are tiled over the world in XZ. The blend
// weights come from the vertex. The albedo, the roughness, and the normal are blended, then lit with a toon ramp whose
// edge follows the roughness. Lit by the sun and the sky ambient. No shadows yet.
#include "common.glsl"
#include "frame.glsl"

TEX(1, 0) uniform sampler2D uTerrainDiffuse;   // 4 by 2 cells: rgb albedo, a roughness
TEX(1, 1) uniform sampler2D uTerrainNormal;    // 4 by 2 cells: tangent-space normal

UBO(1, 15) TerrainLayers {
    vec4 layer[8];   // rgb tint, multiplied into the diffuse
    vec4 tile[8];    // x: metres of ground per texture repeat
} terrainLayers;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vN;
layout(location = 2) in vec4 vWeightsLo;
layout(location = 3) in vec4 vWeightsHi;
layout(location = 0) out vec4 outColor;

const vec2 kCell = vec2(0.25, 0.5);            // one cell in atlas uv (4 by 2 cells)
const vec2 kTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);

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
        vec3 nTs = textureGrad(uTerrainNormal, auv, gx, gy).xyz * 2.0 - 1.0;
        albedo += w * terrainLayers.layer[k].rgb * d.rgb;
        rough += w * d.a;

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

    vec3 n = length(nSum) > 1e-5 ? normalize(nSum) : n0;
    vec3 L = normalize(frame.sunDir.xyz);
    float ndl = max(dot(n, L), 0.0);
    float edge = mix(0.03, 0.25, clamp(rough, 0.0, 1.0));
    float ramp = smoothstep(0.15 - edge, 0.15 + edge, ndl);
    vec3 hemi = mix(frame.groundColor.rgb, frame.skyColor.rgb, 0.5 + 0.5 * n.y);
    outColor = vec4(albedo * (hemi + frame.sunColor.rgb * ramp), 1.0);
}
