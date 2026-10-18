// Underwater effect (item 16). A blended fog over the frame, drawn when the camera is inside the volume and below
// its level. The fog depends on the view distance stored in the refraction copy. Blended objects drawn before it
// are kept; their own depth is not in the copy, so they take the fog from the scene behind them.
#include "common.glsl"
#include "frame.glsl"

TEX(0, 2) uniform sampler2D uRefr;   // Post set binding 2, shared with water.frag

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float d = clamp(texture(uRefr, vUV).a, 0.0, 1000.0);
    float density = frame.waterShallow.w * 0.6 + 0.05;
    float fog = 1.0 - exp(-d * density);
    vec3 tint = mix(frame.waterShallow.rgb, frame.waterDeep.rgb, 1.0 - exp(-d * 0.15));
    vec3 light = frame.skyColor.rgb * 0.6 + frame.sunColor.rgb * 0.2;
    outColor = vec4(tint * light, fog);
}
