// A 2D canvas for the HUD: panels, text, bars and buttons, emitted as one triangle list.
//
// The canvas is immediate mode on purpose: a HUD is rebuilt every frame from current state, and retaining a widget tree
// would add lifetime rules a game does not need. Coordinates are pixels with the origin at the top-left and y growing
// down, which is how a designer measures a screen. The renderer flips y once, in the vertex shader.
//
// Everything is axis-aligned and clipped on the CPU against a scissor stack, so no GPU state changes are needed between
// panels and text: one vertex buffer, one atlas, one draw. That keeps the HUD off the RHI's critical path and makes the
// layout testable without a GPU.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/text_atlas.h"

namespace ukiyo {

struct HudVertex {
    glm::vec2 pos;     // pixels, origin top-left
    glm::vec2 uv;      // atlas UV; the white texel for solid fills
    glm::vec4 color;   // straight alpha
};
static_assert(sizeof(HudVertex) == 32, "HudVertex is 32 bytes; the renderer converts it to the GPU vertex layout");

struct HudRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
};

struct HudColor {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    HudColor() = default;
    HudColor(float rr, float gg, float bb, float aa = 1.0f) : r(rr), g(gg), b(bb), a(aa) {}
    glm::vec4 v() const { return glm::vec4(r, g, b, a); }
    static HudColor hex(uint32_t rgb, float a = 1.0f);
    HudColor withAlpha(float alpha) const { return HudColor(r, g, b, alpha); }
};

enum class HudAlign { Left, Center, Right };

// A small, complete HUD. One instance per frame is the normal use: clear(), draw, submit.
class HudCanvas {
public:
    // The atlas is borrowed and must outlive the canvas. Without one, text draws nothing and measure returns 0.
    void setFont(FontAtlas* font);
    FontAtlas* font() const { return font_; }

    // Screen size in pixels; the renderer flips y against it and clipping uses it.
    void setScreen(float widthPx, float heightPx);
    float screenWidth() const { return screenW_; }
    float screenHeight() const { return screenH_; }

    void clear();
    bool empty() const { return vertices_.empty(); }
    const std::vector<HudVertex>& vertices() const { return vertices_; }

    // Scissor stack. Every primitive is clipped against the current rect; unbalanced pops are ignored.
    void pushClip(const HudRect& r);
    void popClip();
    const HudRect& clip() const { return clip_; }

    // Primitives.
    void rect(const HudRect& r, const HudColor& c);
    // A rectangle with a rounded look is not needed yet; borders are drawn as four rects.
    void border(const HudRect& r, float thickness, const HudColor& c);
    void bar(const HudRect& r, float fraction, const HudColor& back, const HudColor& fill);
    // Text baseline is the top of the line box; `size` scales the font (0 uses the font's own pixel height).
    void text(float x, float y, const std::string& utf8, const HudColor& c, float size = 0.0f);
    // Wrapped text inside `box`; returns the number of lines drawn.
    int textBox(const HudRect& box, const std::string& utf8, const HudColor& c, float size = 0.0f, float lineGap = 2.0f);
    // Text with a 1 px shadow for legibility over the scene.
    void textShadowed(float x, float y, const std::string& utf8, const HudColor& c, float size = 0.0f);
    // Centred horizontally in `box`.
    void textCentered(const HudRect& box, float y, const std::string& utf8, const HudColor& c, float size = 0.0f,
                      HudAlign align = HudAlign::Center);

    // Measurements, in pixels. Both scale with `size` and use the font's advances.
    float textWidth(const std::string& utf8, float size = 0.0f) const;
    float lineHeight(float size = 0.0f) const;
    // Splits a string into lines that fit `maxWidth`, breaking at spaces and falling back to character breaks for
    // Japanese (which has no spaces). Exposed for tests and for layout code that needs the height before drawing.
    std::vector<std::string> wrap(const std::string& utf8, float maxWidth, float size = 0.0f) const;

    // Adds a full-screen translucent wash; used by menus and by the pause overlay.
    void dim(const HudColor& c);

private:
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const glm::vec4& c);
    void addClippedQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                        const glm::vec4& c);
    const GlyphMetrics* glyphFor(uint32_t cp) const;
    float scaleFor(float size) const;

    FontAtlas* font_ = nullptr;
    float whiteU_ = 0.5f / 1024.0f;
    float whiteV_ = 0.5f / 1024.0f;
    float screenW_ = 1280.0f;
    float screenH_ = 720.0f;
    std::vector<HudVertex> vertices_;
    std::vector<HudRect> clips_;
    HudRect clip_{0.0f, 0.0f, 1e9f, 1e9f};
};

} // namespace ukiyo
