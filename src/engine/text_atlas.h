// Text for the HUD: a TrueType font rasterised into one RGBA8 atlas on demand.
//
// The engine had no text at all — not even a font atlas — so a game could not draw a score, a name or a menu. This
// builds the glyph cache the HUD needs, using stb_truetype (public domain, vendored beside stb_image). Glyphs are
// rasterised the first time a code point is drawn, packed into shelves, and the changed rectangle is handed back so the
// renderer can upload only what is new.
//
// Two ways to fill the page, and they differ in what the page can be asked to do later:
//
//   * Coverage (load / loadFromFile): glyphs rasterised at one pixel size, antialiased coverage in alpha. Sharp at that
//     size and only that size; drawn at twice the size every edge is a two-pixel ramp, at half the size the thin strokes
//     go.
//   * Multi-channel signed distance (loadMsdf): glyphs generated at one *generation* size with a distance field in RGB
//     (docs/MSDF.md). The outline is recovered at any size from where the median channel crosses 0.5, so one page serves
//     a 12 px caption and a 96 px heading — the point of Flax 1.12's MSDF fonts. Generation is ~0.2 ms a glyph at 40 px,
//     done on first use.
//
// Coverage lives in the alpha channel, with white RGB, so the HUD shader tints one texture for every colour; an MSDF page
// puts the field in RGB and the median in alpha, which is what a shader that has not been taught about MSDF will draw,
// so an MSDF page shown by old code still produces legible text. The atlas is 1024x1024 by default: at 20 px, ASCII
// plus Latin-1 plus a screenful of Japanese fits comfortably, and a full 4096 page can be requested when a game shows
// longer Japanese text. When the atlas is full, glyphs are refused (null) rather than evicting ones already on screen,
// which would make the previous frame's text wrong.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ukiyo {

struct GlyphMetrics {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;   // atlas UVs
    float width = 0.0f, height = 0.0f;                  // bitmap size in pixels
    float xoff = 0.0f, yoff = 0.0f;                     // from the pen to the bitmap's top-left
    float advance = 0.0f;                               // pen movement
};

class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // `pixelHeight` is the em size in pixels (the size the designer would pick in Qt). The atlas is a square page.
    bool load(const uint8_t* ttf, size_t size, float pixelHeight, int atlasSize = 1024);
    bool loadFromFile(const std::string& path, float pixelHeight, int atlasSize = 1024);

    // The MSDF variant: glyphs are generated at `pixelHeight` with a field `range` pixels wide either side of the
    // outline, and the page can then be drawn at any size. `range` is the number the shader needs to reconstruct an
    // edge (FontAtlas::msdfRange below); 6 px at a 40 px em is the pair the engine uses.
    bool loadMsdf(const uint8_t* ttf, size_t size, float pixelHeight, float range = 6.0f, int atlasSize = 1024);
    bool loadMsdfFromFile(const std::string& path, float pixelHeight, float range = 6.0f, int atlasSize = 1024);

    bool loaded() const;
    // True when the page holds multi-channel distance fields instead of coverage.
    bool msdf() const;
    // The field's half-width, in atlas texels, that the shader divides by. 0 for a coverage page.
    float msdfRange() const;

    float pixelHeight() const;
    float ascent() const;
    float descent() const;     // positive distance below the baseline
    float lineHeight() const;

    // Returns the glyph metrics, rasterising it on first use. Null when the font has no glyph for the code point or
    // the atlas is full. The pointer stays valid until the atlas is destroyed.
    const GlyphMetrics* glyph(uint32_t codepoint);

    // ASCII 32..126 then Latin-1 160..255. A game that shows Japanese passes its own string to preload().
    void preloadAscii();
    void preload(const std::string& utf8);            // every code point in the string
    size_t preloadRange(uint32_t first, uint32_t last);   // returns how many were added

    const std::vector<uint8_t>& pixels() const;   // RGBA8, width*height*4
    int width() const;
    int height() const;

    // Increments whenever new glyphs were written. The renderer recreates or uploads the texture when it changes.
    uint64_t revision() const;

    struct DirtyRect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    // Rectangles written since the last call, merged into one covering rect (text is packed densely, so one rect is
    // almost always cheaper than many).
    DirtyRect takeDirty();

    // UTF-8 decode. Invalid bytes become U+FFFD rather than being dropped, so a broken string is visible.
    static std::vector<uint32_t> decodeUtf8(const std::string& utf8);
    static uint32_t codePointCount(const std::string& utf8);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const GlyphMetrics* rasteriseMsdf(uint32_t codepoint, int glyphIndex);
};

} // namespace ukiyo
