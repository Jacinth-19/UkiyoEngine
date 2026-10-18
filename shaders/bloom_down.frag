// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of Duty", 2014), then the threshold.
// The threshold runs before the blur, as in Kaze. The glow tint is applied in bloom_combine.frag.
// p0.x = soft-threshold (0 = off, used for the first prefilter level only); p1.xy = 1/src size.
#include "common.glsl"
TEX(0, 1) uniform sampler2D uTex0;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 t = pc.p1.xy;
    vec2 uv = vUV;
    vec3 a = texture(uTex0, uv + t * vec2(-2.0,  2.0)).rgb;
    vec3 b = texture(uTex0, uv + t * vec2( 0.0,  2.0)).rgb;
    vec3 c = texture(uTex0, uv + t * vec2( 2.0,  2.0)).rgb;
    vec3 d = texture(uTex0, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 e = texture(uTex0, uv + t * vec2( 1.0,  1.0)).rgb;
    vec3 f = texture(uTex0, uv + t * vec2(-2.0,  0.0)).rgb;
    vec3 g = texture(uTex0, uv).rgb;
    vec3 h = texture(uTex0, uv + t * vec2( 2.0,  0.0)).rgb;
    vec3 i = texture(uTex0, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 j = texture(uTex0, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 k = texture(uTex0, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 l = texture(uTex0, uv + t * vec2( 0.0, -2.0)).rgb;
    vec3 m = texture(uTex0, uv + t * vec2( 2.0, -2.0)).rgb;
    vec3 col = (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625
             + (d + e + i + j) * 0.125 + g * 0.125;
    float thr = pc.p0.x;
    int tmode = int(pc.p0.y + 0.5);   // 0 luminance knee, 1 Color (per channel), 2 Brightness (max channel)
    if (thr > 0.0 && tmode == 1) {
        // Color (Kaze's UnityGenshinPostProcessing): per-channel subtract, so a bright blue stays blue.
        col = max(col - thr, vec3(0.0));
    } else if (thr > 0.0 && tmode == 2) {
        // Brightness (Kaze): the brightest channel gates the whole colour, so the hue is kept.
        float br = max(col.r, max(col.g, col.b));
        col *= clamp(br - thr, 0.0, 1.0);
    } else if (thr > 0.0) {
        float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
        float knee = thr * 0.5;
        float soft = clamp(lum - thr + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        float contrib = max(soft, lum - thr) / max(lum, 1e-4);
        col *= contrib;
    }
    outColor = vec4(col, 1.0);
}
