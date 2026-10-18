// Outline colour for generated materials (see graph_outline.vert for the slots).
layout(set = 1, binding = 15, std140) uniform GraphOutlineMaterial {
    vec4 o0;
    vec4 o1;
    vec4 o2;
};
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(o0.rgb, 1.0);
}
