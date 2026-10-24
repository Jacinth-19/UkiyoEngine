// Trail ribbons (item 27). The CPU has already built the strip: every vertex is a world-space point offset sideways
// from the path, with its texture coordinate and gradient colour baked in. Nothing here depends on the camera, so a
// ribbon keeps its shape when the camera moves, and the same vertex buffer draws it from any angle.
#include "common.glsl"
#include "frame.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inUV;
layout(location = 5) in vec4 inColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV = inUV;
    vColor = inColor;
    gl_Position = frame.viewProj * (pc.model * vec4(inPos, 1.0));
}
