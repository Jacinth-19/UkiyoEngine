// FXAA 3.11 (quality preset), after Timothy Lottes (NVIDIA), via the GLSL port in
// mattdesl/glsl-fxaa (MIT). Adapted to the Ukiyo post set: texture() instead of
// texture2D(), resolution in p1.xy. Luma is computed from RGB directly.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

#define FXAA_REDUCE_MIN (1.0 / 128.0)
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_SPAN_MAX   8.0

void main() {
    vec2 res = pc.p1.xy;
    vec2 fragCoord = gl_FragCoord.xy;
    vec2 inv = 1.0 / res;
    vec2 vNW = (fragCoord + vec2(-1.0, -1.0)) * inv;
    vec2 vNE = (fragCoord + vec2( 1.0, -1.0)) * inv;
    vec2 vSW = (fragCoord + vec2(-1.0,  1.0)) * inv;
    vec2 vSE = (fragCoord + vec2( 1.0,  1.0)) * inv;
    vec2 vM  = fragCoord * inv;

    vec3 rgbNW = texture(uTex0, vNW).xyz;
    vec3 rgbNE = texture(uTex0, vNE).xyz;
    vec3 rgbSW = texture(uTex0, vSW).xyz;
    vec3 rgbSE = texture(uTex0, vSE).xyz;
    vec3 rgbM  = texture(uTex0, vM).xyz;
    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * inv;

    vec3 rgbA = 0.5 * (texture(uTex0, vM + dir * (1.0 / 3.0 - 0.5)).xyz +
                       texture(uTex0, vM + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(uTex0, vM + dir * -0.5).xyz +
                                     texture(uTex0, vM + dir *  0.5).xyz);
    float lumaB = dot(rgbB, luma);
    outColor = vec4(((lumaB < lumaMin) || (lumaB > lumaMax)) ? rgbA : rgbB, 1.0);
}
