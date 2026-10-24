// HUD quads in pixels, origin top-left. No descriptor set is needed for geometry: the screen size travels in the
// push block, so the pass binds only the glyph atlas and the shader has no Frame dependency.
#include "common.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inUV;
layout(location = 5) in vec4 inColor;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
void main() {
    // p0.xy is the presentation size in pixels, set by Renderer::drawHud.
    vec2 screen = max(pc.p0.xy, vec2(1.0));
    vUV = inUV;
    vColor = inColor;
    gl_Position = vec4(inPos.x / screen.x * 2.0 - 1.0, 1.0 - inPos.y / screen.y * 2.0, 0.0, 1.0);
}
