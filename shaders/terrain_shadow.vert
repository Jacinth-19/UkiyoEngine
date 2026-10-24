// Terrain in the cascaded shadow pass (item 19, production pass). A depth-only draw of the tile, so its hills shade
// themselves, the grass on them and anything standing on them. p0.x is the cascade.
#include "common.glsl"
#include "frame.glsl"

layout(location = 0) in vec3 inPos;

void main() {
    int cascade = int(pc.p0.x + 0.5);
    gl_Position = frame.shadowMatrix[cascade] * (pc.model * vec4(inPos, 1.0));
}
