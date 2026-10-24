// HUD fragment: one glyph atlas, tinted per vertex.
//
// Two kinds of page, one shader. A coverage page (TextAtlas::load) stores antialiased coverage in alpha, so a glyph is
// sharp at the size it was rasterised at and soft everywhere else. An MSDF page (TextAtlas::loadMsdf) stores a
// multi-channel distance field in rgb: the outline is where the *median* channel crosses 0.5, and its screen-space
// gradient gives an edge exactly one pixel wide at any magnification, because interpolating a linear field stays linear.
// p0.z carries the field's half-width in texels (0 for a coverage page), so the branch is a uniform and free.
//
// Solid fills sample the reserved white texel at (0,0) of the page: its rgb is 1 and its median is therefore 0.5 above
// the outline, so a fill comes out at full alpha through the same path.
#include "common.glsl"
TEX(0, 0) uniform sampler2D uAtlas;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(uAtlas, vUV);
    float alpha = texel.a;
    if (pc.p0.z > 0.0) {
        // Range in texels -> range in uv units, then into screen pixels through the uv derivative. fwidth of a quad's
        // linear uv is constant across it, so this is exact rather than a per-pixel estimate.
        // The median of the three channels: the same reconstruction as msdf::median in src/engine/msdf.h, which is what
        // the CPU preview draws with, so the figure and the frame agree.
        float median = max(min(texel.r, texel.g), min(max(texel.r, texel.g), texel.b));
        float sig = median - 0.5;
        vec2 unitRange = vec2(pc.p0.z) / vec2(textureSize(uAtlas, 0));
        vec2 texelPerPixel = fwidth(vUV);
        // One screen pixel of ramp, not the textbook two: half a pixel of extra ink per side is negligible on a heading
        // and a third of the glyph on 13 px text (docs/MSDF.md has the measurements).
        float screenPxRange = max(dot(unitRange, 1.0 / max(texelPerPixel, vec2(1e-9))), 1.0);
        alpha = clamp(sig * screenPxRange + 0.5, 0.0, 1.0);
    }
    outColor = vec4(vColor.rgb, vColor.a * alpha);
}
