// HUD and text: UTF-8 decoding, glyph rasterisation from a real font, atlas packing, layout, clipping and batching.
#include "engine/hud.h"
#include "engine/text_atlas.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ukiyo;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

std::string fontPath() {
#ifdef UKIYO_ROOT
    return std::string(UKIYO_ROOT) + "/editor/res/fonts/IBMPlexSansJP-Regular.ttf";
#else
    return "editor/res/fonts/IBMPlexSansJP-Regular.ttf";
#endif
}
} // namespace

int main() {
    // UTF-8 decoding: ASCII, two-, three- and four-byte sequences, and the cases that must become U+FFFD.
    {
        const auto a = FontAtlas::decodeUtf8("abc");
        check(a.size() == 3 && a[0] == 'a', "ascii decodes");
        const auto e = FontAtlas::decodeUtf8("é");            // C3 A9
        check(e.size() == 1 && e[0] == 0xE9, "two-byte sequence decodes");
        const auto jp = FontAtlas::decodeUtf8("セーブ");        // three bytes each
        check(jp.size() == 3 && jp[0] == 0x30BB, "three-byte Japanese decodes");
        const auto emoji = FontAtlas::decodeUtf8("\xF0\x9F\x98\x80");   // U+1F600
        check(emoji.size() == 1 && emoji[0] == 0x1F600, "four-byte sequence decodes");
        const auto bad = FontAtlas::decodeUtf8("a\xFF b");
        check(bad.size() == 4 && bad[1] == 0xFFFD && bad[2] == ' ', "an invalid byte becomes U+FFFD");
        const auto trunc = FontAtlas::decodeUtf8("a\xE3\x81");
        check(trunc.size() == 2 && trunc[1] == 0xFFFD, "a truncated sequence becomes U+FFFD");
        const auto overlong = FontAtlas::decodeUtf8("\xC0\xAF");
        check(overlong.size() == 2 && overlong[0] == 0xFFFD, "an overlong encoding is rejected byte by byte");
        const auto surrogate = FontAtlas::decodeUtf8("\xED\xA0\x80");
        check(surrogate[0] == 0xFFFD, "a surrogate is rejected");
        check(FontAtlas::codePointCount("abc") == 3, "codePointCount matches the decoded length");
    }

    // A real font: the project's Japanese face.
    FontAtlas font;
    const bool loaded = font.loadFromFile(fontPath(), 20.0f, 1024);
    check(loaded, "the project font loads", fontPath());
    if (!loaded) {
        std::printf("hud_test: %d checks, %d failures (no font)\n", checks, failures);
        return 1;
    }
    check(font.width() == 1024 && font.height() == 1024, "the atlas has the requested page size");
    check(font.lineHeight() > 10.0f, "line height is sane");
    check(font.ascent() > 0.0f && font.descent() >= 0.0f, "ascent and descent are positive measures");

    const uint64_t rev0 = font.revision();
    const GlyphMetrics* A = font.glyph('A');
    check(A != nullptr, "glyph A rasterises");
    check(font.revision() > rev0, "rasterising a glyph bumps the revision");
    check(A && A->width > 4.0f && A->height > 4.0f, "glyph A has a bitmap");
    check(A && A->advance > 0.0f, "glyph A advances");
    check(A && A->u1 > A->u0 && A->v1 > A->v0, "glyph A has a UV rectangle");

    // The reserved white texel is opaque; the atlas background is transparent.
    const std::vector<uint8_t>& px = font.pixels();
    check(px[3] == 255 && px[0] == 255 && px[1] == 255 && px[2] == 255, "texel (0,0) is opaque white");
    bool foundInk = false;
    for (size_t i = 3; i < px.size(); i += 4)
        if (px[i] > 0 && px[i] < 255) {
            foundInk = true;
            break;
        }
    check(foundInk, "glyph pixels carry partial coverage in alpha");

    // Dirty rects are reported once.
    const FontAtlas::DirtyRect dirty = font.takeDirty();
    check(dirty.w > 0 && dirty.h > 0, "a dirty rectangle is reported");
    const FontAtlas::DirtyRect none = font.takeDirty();
    check(none.w == 0 && none.h == 0, "taking twice clears the dirty rectangle");

    check(font.glyph(0x30BB) != nullptr, "Japanese glyph セ rasterises");
    check(font.glyph(0x10FFFF) == nullptr || font.glyph(0x10FFFF)->width >= 0.0f, "an unlikely code point is handled");

    // Preloading ASCII adds many glyphs and advances the revision.
    const uint64_t revBefore = font.revision();
    const size_t ascii = font.preloadRange(32, 126);
    check(ascii >= 90, "preloading ASCII rasterises most printable characters", std::to_string(ascii));
    check(font.revision() > revBefore, "preloading bumps the revision");

    // Canvas layout.
    HudCanvas hud;
    hud.setFont(&font);
    hud.setScreen(1280.0f, 720.0f);
    hud.clear();
    check(hud.empty(), "a cleared canvas has no vertices");

    hud.rect({10.0f, 20.0f, 100.0f, 40.0f}, HudColor::hex(0x112233));
    check(hud.vertices().size() == 6, "a rectangle is two triangles");
    check(hud.vertices()[0].pos == glm::vec2(10.0f, 20.0f), "the first vertex is the top-left corner");
    check(hud.vertices()[0].uv.x < 0.01f && hud.vertices()[0].uv.y < 0.01f, "solid fills sample the white texel");

    hud.clear();
    hud.rect({0.0f, 0.0f, 50.0f, 50.0f}, HudColor());
    hud.rect({0.0f, 0.0f, 50.0f, 50.0f}, HudColor());
    check(hud.vertices().size() == 12, "each primitive appends six vertices");

    // Clipping: a rect half outside the scissor keeps its six vertices but only the visible half.
    hud.clear();
    hud.pushClip({0.0f, 0.0f, 100.0f, 100.0f});
    hud.rect({50.0f, 0.0f, 100.0f, 50.0f}, HudColor());
    check(hud.vertices().size() == 6, "a clipped rect still emits one quad");
    float maxX = 0.0f;
    for (const HudVertex& v : hud.vertices()) maxX = std::max(maxX, v.pos.x);
    check(maxX <= 100.001f, "the clipped quad stops at the scissor edge");
    hud.popClip();

    hud.clear();
    hud.pushClip({0.0f, 0.0f, 10.0f, 10.0f});
    hud.rect({50.0f, 50.0f, 10.0f, 10.0f}, HudColor());
    check(hud.vertices().empty(), "a rect entirely outside the scissor emits nothing");

    // Nested clips intersect.
    hud.clear();
    hud.pushClip({0.0f, 0.0f, 100.0f, 100.0f});
    hud.pushClip({50.0f, 50.0f, 100.0f, 100.0f});
    check(hud.clip().x == 50.0f && hud.clip().w == 50.0f, "nested clips intersect");
    hud.popClip();
    check(hud.clip().x == 0.0f && hud.clip().w == 100.0f, "popping restores the outer clip");
    hud.popClip();
    hud.popClip();   // extra pops are ignored
    check(hud.clip().x == 0.0f, "an extra pop is ignored");

    // Text: metrics, drawing and centring.
    hud.clear();
    const float wA = hud.textWidth("A", 20.0f);
    check(wA > 0.0f && wA < 40.0f, "text width is measured", std::to_string(wA));
    check(hud.textWidth("", 20.0f) == 0.0f, "an empty string is zero wide");
    hud.text(10.0f, 10.0f, "AB", HudColor());
    check(hud.vertices().size() == 12, "two glyphs are two quads");
    float maxY = 0.0f;
    for (const HudVertex& v : hud.vertices()) maxY = std::max(maxY, v.pos.y);
    check(maxY > 10.0f && maxY < 10.0f + 20.0f * 2.0f, "glyph quads sit inside the line box");

    hud.clear();
    hud.textCentered({0.0f, 0.0f, 400.0f, 40.0f}, 0.0f, "AB", HudColor(), 20.0f);
    const float startX = hud.vertices()[0].pos.x;
    check(startX > 180.0f && startX < 200.0f, "centred text starts near the middle", std::to_string(startX));

    hud.clear();
    hud.textCentered({0.0f, 0.0f, 400.0f, 40.0f}, 0.0f, "AB", HudColor(), 20.0f, HudAlign::Right);
    check(hud.vertices()[0].pos.x > 350.0f, "right-aligned text ends at the box edge");

    // Scaling: 40 px text is twice as wide as 20 px text.
    hud.clear();
    const float w20 = hud.textWidth("Hello", 20.0f);
    const float w40 = hud.textWidth("Hello", 40.0f);
    check(std::fabs(w40 - 2.0f * w20) < 1e-3f, "text scales linearly");

    // Line height scales.
    check(std::fabs(hud.lineHeight(40.0f) - 2.0f * hud.lineHeight(20.0f)) < 1e-3f, "line height scales linearly");

    // Wrapping: a long sentence over a narrow box becomes several lines.
    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    const auto lines = hud.wrap(sentence, 100.0f, 20.0f);
    check(lines.size() >= 3, "a long sentence wraps", std::to_string(lines.size()));
    for (const std::string& l : lines) check(hud.textWidth(l, 20.0f) <= 100.5f, "every wrapped line fits");
    // Reassembling the lines gives back the words in order.
    std::string joined;
    for (const std::string& l : lines) joined += l;
    check(joined.find("quick") != std::string::npos && joined.find("dog") != std::string::npos,
          "wrapping keeps every word");

    // Japanese text has no spaces: it must still break.
    const auto jpLines = hud.wrap("これはとても長い日本語の文章です", 80.0f, 20.0f);
    check(jpLines.size() >= 2, "Japanese wraps without spaces", std::to_string(jpLines.size()));

    // Explicit newlines are honoured.
    const auto hard = hud.wrap("one\ntwo", 1000.0f, 20.0f);
    check(hard.size() == 2 && hard[0] == "one" && hard[1] == "two", "a newline forces a break");

    hud.clear();
    const int drawn = hud.textBox({0.0f, 0.0f, 100.0f, 100.0f}, sentence, HudColor(), 20.0f);
    check(drawn >= 3, "textBox draws wrapped lines", std::to_string(drawn));

    // Bars clamp.
    hud.clear();
    hud.bar({0.0f, 0.0f, 100.0f, 10.0f}, 0.5f, HudColor(), HudColor());
    const float fillRight = hud.vertices()[7].pos.x;   // second quad, top-right vertex
    check(std::fabs(fillRight - 50.0f) < 0.01f, "a half bar fills half");
    hud.clear();
    hud.bar({0.0f, 0.0f, 100.0f, 10.0f}, 4.0f, HudColor(), HudColor());
    check(std::fabs(hud.vertices()[7].pos.x - 100.0f) < 0.01f, "a full bar clamps to the width");
    hud.clear();
    hud.bar({0.0f, 0.0f, 100.0f, 10.0f}, -1.0f, HudColor(), HudColor());
    check(hud.vertices().size() == 6, "an empty bar draws only the background");

    // Colour is passed through and alpha is preserved (straight alpha, not premultiplied).
    hud.clear();
    hud.rect({0, 0, 4, 4}, HudColor(0.25f, 0.5f, 0.75f, 0.5f));
    check(hud.vertices()[0].color == glm::vec4(0.25f, 0.5f, 0.75f, 0.5f), "colour reaches the vertices");

    // No font: text is a no-op and solid fills still work.
    HudCanvas bare;
    bare.setScreen(640.0f, 360.0f);
    bare.clear();
    check(bare.textWidth("x", 20.0f) == 0.0f, "no font measures zero");
    bare.text(0, 0, "x", HudColor());
    check(bare.vertices().empty(), "no font draws nothing");
    bare.rect({0, 0, 10, 10}, HudColor());
    check(bare.vertices().size() == 6, "solid fills work without a font");

    std::printf("hud_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
