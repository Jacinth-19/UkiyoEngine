// Water surface (item 16). The grid vertices are rest points in [0,1]^2 over the volume rectangle. Each one is
// displaced by the same four-wave Gerstner sum as src/engine/water.cpp (waterOffset / waterSurface). The rest point
// is found by a few fixed-point steps so the drawn position lands on the rest grid.
#include "common.glsl"
#include "frame.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vN;
layout(location = 2) out float vCrest;

// Offset of the displaced point for rest point r at time t. Writes the normal.
vec3 gerstnerOffset(vec2 r, float t, out vec3 n) {
    vec3 off = vec3(0.0);
    vec3 dPdx = vec3(1.0, 0.0, 0.0);
    vec3 dPdz = vec3(0.0, 0.0, 1.0);
    for (int i = 0; i < 4; ++i) {
        vec2 d = frame.waterWaveD[i].xy;
        float a = max(frame.waterWaveD[i].w, 0.0);
        float k = 6.28318530718 / max(frame.waterWaveD[i].z, 0.01);
        float omega = sqrt(9.81 * k) * frame.waterWaveS[i].x;
        float steep = clamp(frame.waterWaveS[i].y, 0.0, 1.0);
        float ka = k * a;
        float q = ka > 1e-6 ? steep / (ka * 4.0) : 0.0;
        float phase = k * dot(d, r) - omega * t;
        float c = cos(phase);
        float s = sin(phase);
        off.x += q * a * d.x * c;
        off.z += q * a * d.y * c;
        off.y += a * s;
        dPdx.x -= q * ka * d.x * d.x * s;
        dPdx.y += ka * d.x * c;
        dPdx.z -= q * ka * d.x * d.y * s;
        dPdz.x -= q * ka * d.x * d.y * s;
        dPdz.y += ka * d.y * c;
        dPdz.z -= q * ka * d.y * d.y * s;
    }
    n = cross(dPdz, dPdx);
    n = length(n) > 1e-6 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    return off;
}

void main() {
    vec2 rest0 = frame.waterArea.xz - frame.waterHalf.xz + 2.0 * frame.waterHalf.xz * inUV;
    float t = frame.screenParams.z;
    vec2 r = rest0;
    vec3 n;
    vec3 off;
    for (int it = 0; it < 4; ++it) {
        off = gerstnerOffset(r, t, n);
        r = rest0 - off.xz;
    }
    off = gerstnerOffset(r, t, n);
    vec3 wp = vec3(rest0.x, frame.waterArea.y + off.y, rest0.y);
    float sumA = max(frame.waterWaveD[0].w, 0.0) + max(frame.waterWaveD[1].w, 0.0)
               + max(frame.waterWaveD[2].w, 0.0) + max(frame.waterWaveD[3].w, 0.0);
    float h = sumA > 1e-4 ? 0.5 + 0.5 * off.y / sumA : 0.5;
    vCrest = smoothstep(0.82, 0.98, h);
    vWorld = wp;
    vN = n;
    gl_Position = frame.viewProj * vec4(wp, 1.0);
}
