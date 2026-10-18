// 3x3 tent upsample; written with additive blending onto the next-finer level.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 t = pc.p1.xy * pc.p0.x;
    vec2 uv = vUV;
    vec3 s = texture(uTex0, uv + t * vec2(-1.0,  1.0)).rgb
           + texture(uTex0, uv + t * vec2( 1.0,  1.0)).rgb
           + texture(uTex0, uv + t * vec2(-1.0, -1.0)).rgb
           + texture(uTex0, uv + t * vec2( 1.0, -1.0)).rgb
           + (texture(uTex0, uv + t * vec2( 0.0,  1.0)).rgb
           +  texture(uTex0, uv + t * vec2( 0.0, -1.0)).rgb
           +  texture(uTex0, uv + t * vec2(-1.0,  0.0)).rgb
           +  texture(uTex0, uv + t * vec2( 1.0,  0.0)).rgb) * 2.0
           + texture(uTex0, uv).rgb * 4.0;
    outColor = vec4(s / 16.0, 1.0);
}
