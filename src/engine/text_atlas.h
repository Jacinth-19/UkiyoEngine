// Text for the HUD: a TrueType font rasterised into one RGBA8 atlas on demand.
//
// The engine had no text at all — not even a font atlas — so a game could not draw a score, a name or a menu. This
// builds the glyph cache the HUD needs, using stb_truetype (public domain, vendored beside stb_image). Glyphs are
// rasterised the first time a code point is drawn, packed into shelves, and the changed rectangle is handed back so the
// renderer can upload only what is new.
//
// Coverage lives in the alpha channel, with white RGB, so the HUD shader tints one texture for every colour. The atlas
// is 1024x1024 by default: at 20 px, ASCII plus Latin-1 plus a screenful of Japanese fits comfortably, and a full
// 4096 page can be requested when a game shows longer Japanese text. When the atlas is full, glyphs are refused (null)
// rather than evicting ones already on screen, which would make the previous frame's text wrong.
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
    bool loaded() const;

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
};

} // namespace ukiyo
