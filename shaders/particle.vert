// Particle billboards: one instanced quad per live particle. No vertex buffer. Six vertices per
// instance (two triangles), with the corner chosen from gl_VertexID and the particle from gl_InstanceID.
#include "common.glsl"
#include "frame.glsl"

struct ParticleInst {
    vec4 posSize;   // xyz world position, w size (world units)
    vec4 color;     // rgba, linear
};

// Up to 1024 instances per draw. 32 KB, within the 64 KB minimum uniform block size.
UBO(1, 11) ParticleBlock {
    ParticleInst p[1024];
} parts;

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;

void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
        vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));
    vec2 c = corners[VERTEX_ID % 6];
    ParticleInst inst = parts.p[INSTANCE_ID];

    // Camera right and up come from the view matrix rows, so quads face the camera.
    vec3 right = vec3(frame.view[0][0], frame.view[1][0], frame.view[2][0]);
    vec3 up    = vec3(frame.view[0][1], frame.view[1][1], frame.view[2][1]);
    vec3 world = inst.posSize.xyz + (right * c.x + up * c.y) * inst.posSize.w;

    vCorner = c;
    vColor = inst.color;
    gl_Position = frame.viewProj * vec4(world, 1.0);
}
