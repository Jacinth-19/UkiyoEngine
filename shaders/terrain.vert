// Terrain chunk (item 18). Positions are in tile space; the push constant model moves them into the world.
// The layer weights come in two attributes: layers 0 to 3 as floats (location 4, the weights slot), and layers 4 to 7
// as u16 scaled by 65535 (location 3, the joints slot). The terrain pipeline does not skin.
#include "common.glsl"
#include "frame.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inWeightsHi;
layout(location = 4) in vec4 inWeightsLo;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vN;
layout(location = 2) out vec4 vWeightsLo;
layout(location = 3) out vec4 vWeightsHi;

void main() {
    vec4 w = pc.model * vec4(inPos, 1.0);
    vWorld = w.xyz;
    vN = mat3(pc.model) * inNormal;
    vWeightsLo = inWeightsLo;
    vWeightsHi = vec4(inWeightsHi) / 65535.0;
    gl_Position = frame.viewProj * w;
}
