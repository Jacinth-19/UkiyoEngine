// Writes the view-space normal, encoded to 0..1. Alpha 1 marks an opaque surface.
#include "common.glsl"
layout(location = 0) in vec3 vNormal;
layout(location = 0) out vec4 outNormal;
void main() {
    vec3 n = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    outNormal = vec4(n * 0.5 + 0.5, 1.0);
}
