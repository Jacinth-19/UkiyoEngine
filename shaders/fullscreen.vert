// Fullscreen triangle with no vertex buffer. vUV spans [0,1] over the target.
#include "common.glsl"
layout(location = 0) out vec2 vUV;
void main() {
    int id = VERTEX_ID;
    vec2 pos = vec2(float((id << 1) & 2), float(id & 2));
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
