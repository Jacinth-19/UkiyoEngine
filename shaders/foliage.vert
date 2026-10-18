// Foliage (item 18, step 3). One instanced draw per batch. Each instance's transform is three vec4 rows at binding 15,
// read here (row r is (m[0][r], m[1][r], m[2][r], m[3][r]), so the translation is the w of each row). The push
// constant model moves the tile into the world. Past p0.x metres from the camera the instance shrinks to its base,
// and it is gone by p0.y, so the tile's far edge fades out without popping.
#include "common.glsl"
#include "frame.glsl"

UBO(1, 15) FoliageRows {
    vec4 row[3 * 256];
} rows;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vN;

void main() {
    uint i = uint(INSTANCE_ID);
    vec4 r0 = rows.row[3 * i];
    vec4 r1 = rows.row[3 * i + 1];
    vec4 r2 = rows.row[3 * i + 2];
    vec3 origin = vec3(r0.w, r1.w, r2.w);

    vec3 p = vec3(dot(r0, vec4(inPos, 1.0)), dot(r1, vec4(inPos, 1.0)), dot(r2, vec4(inPos, 1.0)));
    vec3 n = vec3(dot(r0.xyz, inNormal), dot(r1.xyz, inNormal), dot(r2.xyz, inNormal));

    float d = distance(frame.camPos.xyz, (pc.model * vec4(origin, 1.0)).xyz);
    float fade = 1.0 - smoothstep(pc.p0.x, pc.p0.y, d);
    p = origin + (p - origin) * fade;

    vec4 w = pc.model * vec4(p, 1.0);
    vWorld = w.xyz;
    vN = mat3(pc.model) * n;
    gl_Position = frame.viewProj * w;
}
