// Five-type outline for NahidaHybrid (BodyAndHair's DrawOutline). The ILM alpha (normalStyle.a) picks one of
// five type alphas, using the source's cascade: hard steps at 0.15, 0.4, 0.6 and 0.85. A type with alpha < 0.01
// draws no outline. The colour is the base texture times 0.25, not a per-type colour.
// KEEP THE THRESHOLDS IN STEP with ilmRampV() in tools/endfield/EndfieldHybrid.body.glsl.
layout(set = 1, binding = 15, std140) uniform GraphOutlineMaterial {
    vec4 o0;
    vec4 o1;
    vec4 o2;
    vec4 o3;   // outlineMapAlpha0 (ILM alpha 0 .. 0.15)
    vec4 o4;   // outlineMapAlpha1 (0.15 .. 0.4)
    vec4 o5;   // outlineMapAlpha2 (0.4 .. 0.6)
    vec4 o6;   // outlineMapAlpha3 (0.6 .. 0.85)
    vec4 o7;   // outlineMapAlpha4 (0.85 .. 1)
};
layout(set = 1, binding = 0) uniform sampler2D tex_base;
layout(set = 1, binding = 1) uniform sampler2D tex_normalStyle;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;
void main() {
    float a = texture(tex_normalStyle, v_uv).a;
    float m = mix(o7.x, o6.x, step(a, 0.85));
    m = mix(m, o5.x, step(a, 0.6));
    m = mix(m, o4.x, step(a, 0.4));
    m = mix(m, o3.x, step(a, 0.15));
    if (m < 0.01) discard;
    outColor = vec4(texture(tex_base, v_uv).rgb * 0.25, 1.0);
}
