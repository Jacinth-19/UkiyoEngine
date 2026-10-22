#include "engine/text_atlas.h"

#include "engine/msdf.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace ukiyo {

struct FontAtlas::Impl {
    stbtt_fontinfo font{};
    std::vector<uint8_t> ttf;
    float pixelHeight = 16.0f;
    float scale = 1.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineHeight = 0.0f;

    int atlas = 1024;
    std::vector<uint8_t> pixels;
    int shelfX = 0;
    int shelfY = 0;
    int shelfHeight = 0;
    bool loaded = false;
    bool full = false;
    // MSDF mode: the field's half-width in texels, and the em size it was generated at. Zero range = coverage page.
    float msdfRange = 0.0f;
    float msdfEm = 0.0f;

    uint64_t revision = 0;
    DirtyRect dirtyAtlas{};
    bool dirtyAny = false;
    // Mapping from a rasterised glyph index to its atlas entry; two code points can share a bitmap.
    std::map<uint32_t, GlyphMetrics> glyphs;   // keyed by code point, so metrics stay cheap to look up
    std::map<int, GlyphMetrics> byIndex;       // keyed by glyph index, to reuse duplicates
};

FontAtlas::FontAtlas() : impl_(std::make_unique<Impl>()) {}

FontAtlas::~FontAtlas() = default;

bool FontAtlas::load(const uint8_t* ttf, size_t size, float pixelHeight, int atlasSize) {
    Impl& im = *impl_;
    im = Impl{};
    if (!ttf || size == 0 || pixelHeight <= 0.0f) return false;
    im.ttf.assign(ttf, ttf + size);
    if (!stbtt_InitFont(&im.font, im.ttf.data(), 0)) return false;
    im.pixelHeight = pixelHeight;
    im.atlas = std::max(128, std::min(atlasSize, 4096));
    im.pixels.assign(static_cast<size_t>(im.atlas) * im.atlas * 4, 0);
    // Texel (0,0) is reserved as opaque white so the HUD can draw solid fills from the same texture as text.
    im.pixels[0] = im.pixels[1] = im.pixels[2] = im.pixels[3] = 255;
    im.shelfX = 1;
    im.scale = stbtt_ScaleForPixelHeight(&im.font, pixelHeight);
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&im.font, &a, &d, &g);
    im.ascent = a * im.scale;
    im.descent = -d * im.scale;
    im.lineHeight = (a - d + g) * im.scale;
    if (im.lineHeight <= 0.0f) im.lineHeight = pixelHeight;   // a font with broken metrics still lays out
    im.loaded = true;
    return true;
}

bool FontAtlas::loadMsdf(const uint8_t* ttf, size_t size, float pixelHeight, float range, int atlasSize) {
    if (!load(ttf, size, pixelHeight, atlasSize)) return false;
    // A range of zero asks for a coverage page: the same font, the old field, which is what a project with a hand-picked
    // pixel size and no scaling wants.
    if (range > 0.0f) {
        impl_->msdfRange = range;
        impl_->msdfEm = pixelHeight;
    }
    return true;
}

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(n));
    const size_t read = std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    return read == out->size();
}

} // namespace

bool FontAtlas::loadFromFile(const std::string& path, float pixelHeight, int atlasSize) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, &bytes)) return false;
    return load(bytes.data(), bytes.size(), pixelHeight, atlasSize);
}

bool FontAtlas::loaded() const { return impl_->loaded; }
float FontAtlas::pixelHeight() const { return impl_->pixelHeight; }
float FontAtlas::ascent() const { return impl_->ascent; }
float FontAtlas::descent() const { return impl_->descent; }
float FontAtlas::lineHeight() const { return impl_->lineHeight; }
int FontAtlas::width() const { return impl_->atlas; }
int FontAtlas::height() const { return impl_->atlas; }
const std::vector<uint8_t>& FontAtlas::pixels() const { return impl_->pixels; }
uint64_t FontAtlas::revision() const { return impl_->revision; }

bool FontAtlas::msdf() const { return impl_->msdfRange > 0.0f; }
float FontAtlas::msdfRange() const { return impl_->msdfRange; }

bool FontAtlas::loadMsdfFromFile(const std::string& path, float pixelHeight, float range, int atlasSize) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, &bytes)) return false;
    return loadMsdf(bytes.data(), bytes.size(), pixelHeight, range, atlasSize);
}

const GlyphMetrics* FontAtlas::glyph(uint32_t cp) {
    Impl& im = *impl_;
    if (!im.loaded) return nullptr;
    auto known = im.glyphs.find(cp);
    if (known != im.glyphs.end()) return &known->second;
    const int gi = stbtt_FindGlyphIndex(&im.font, static_cast<int>(cp));
    if (gi == 0 && cp != 0 && cp != 0xFFFD) return nullptr;   // the font has no glyph for this code point
    auto dup = im.byIndex.find(gi);
    if (dup != im.byIndex.end()) {
        im.glyphs[cp] = dup->second;
        return &im.glyphs[cp];
    }
    if (im.msdfRange > 0.0f) return rasteriseMsdf(cp, gi);

    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&im.font, gi, &adv, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&im.font, gi, im.scale, im.scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;

    GlyphMetrics m;
    m.width = static_cast<float>(w);
    m.height = static_cast<float>(h);
    m.xoff = static_cast<float>(x0);
    m.yoff = static_cast<float>(y0);
    m.advance = adv * im.scale;
    if (w > 0 && h > 0) {
        const int pad = 1;
        if (im.shelfX + w + pad > im.atlas) {
            im.shelfX = 0;
            im.shelfY += im.shelfHeight + pad;
            im.shelfHeight = 0;
        }
        if (im.shelfY + h + pad > im.atlas) {
            im.full = true;
            return nullptr;   // atlas exhausted: refuse rather than corrupt text already on screen
        }
        const int ax = im.shelfX;
        const int ay = im.shelfY;
        std::vector<uint8_t> bitmap(static_cast<size_t>(w) * h);
        stbtt_MakeGlyphBitmap(&im.font, bitmap.data(), w, h, w, im.scale, im.scale, gi);
        for (int row = 0; row < h; ++row) {
            uint8_t* dst = im.pixels.data() + (static_cast<size_t>(ay + row) * im.atlas + ax) * 4;
            const uint8_t* src = bitmap.data() + static_cast<size_t>(row) * w;
            for (int col = 0; col < w; ++col) {
                const uint8_t a = src[col];
                dst[static_cast<size_t>(col) * 4 + 0] = 255;
                dst[static_cast<size_t>(col) * 4 + 1] = 255;
                dst[static_cast<size_t>(col) * 4 + 2] = 255;
                dst[static_cast<size_t>(col) * 4 + 3] = a;
            }
        }
        m.u0 = static_cast<float>(ax) / im.atlas;
        m.v0 = static_cast<float>(ay) / im.atlas;
        m.u1 = static_cast<float>(ax + w) / im.atlas;
        m.v1 = static_cast<float>(ay + h) / im.atlas;
        im.shelfX += w + pad;
        im.shelfHeight = std::max(im.shelfHeight, h);
        // Merge into the pending dirty rect.
        if (!im.dirtyAny) {
            im.dirtyAtlas = {ax, ay, w, h};
            im.dirtyAny = true;
        } else {
            DirtyRect& d = im.dirtyAtlas;
            const int nx = std::min(d.x, ax), ny = std::min(d.y, ay);
            const int nr = std::max(d.x + d.w, ax + w), nb = std::max(d.y + d.h, ay + h);
            d = {nx, ny, nr - nx, nb - ny};
        }
        ++im.revision;
    }
    im.byIndex[gi] = m;
    im.glyphs[cp] = m;
    return &im.glyphs[cp];
}

const GlyphMetrics* FontAtlas::rasteriseMsdf(uint32_t cp, int gi) {
    Impl& im = *impl_;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&im.font, gi, &adv, &lsb);

    msdf::Params params;
    params.range = im.msdfRange;
    msdf::Field field;
    if (!msdf::build(im.ttf.data(), im.ttf.size(), gi, im.scale, params, &field)) {
        // A glyph with no outline (a space, a tab): it advances and draws nothing, and it is worth remembering so the
        // next lookup does not pay for the generator again.
        GlyphMetrics m;
        m.advance = adv * im.scale;
        im.byIndex[gi] = m;
        im.glyphs[cp] = m;
        return &im.glyphs[cp];
    }

    const int w = field.width, h = field.height;
    const int pad = 1;
    if (im.shelfX + w + pad > im.atlas) {
        im.shelfX = 0;
        im.shelfY += im.shelfHeight + pad;
        im.shelfHeight = 0;
    }
    if (im.shelfY + h + pad > im.atlas) {
        im.full = true;
        return nullptr;   // atlas exhausted: refuse rather than corrupt text already on screen
    }
    const int ax = im.shelfX;
    const int ay = im.shelfY;
    for (int row = 0; row < h; ++row) {
        std::memcpy(im.pixels.data() + (static_cast<size_t>(ay + row) * im.atlas + ax) * 4,
                    field.pixels.data() + static_cast<size_t>(row) * w * 4, static_cast<size_t>(w) * 4);
    }

    GlyphMetrics m;
    m.width = static_cast<float>(w);
    m.height = static_cast<float>(h);
    m.xoff = static_cast<float>(field.xoff);
    m.yoff = static_cast<float>(field.yoff);
    m.advance = adv * im.scale;
    m.u0 = static_cast<float>(ax) / im.atlas;
    m.v0 = static_cast<float>(ay) / im.atlas;
    m.u1 = static_cast<float>(ax + w) / im.atlas;
    m.v1 = static_cast<float>(ay + h) / im.atlas;

    im.shelfX += w + pad;
    im.shelfHeight = std::max(im.shelfHeight, h);
    if (!im.dirtyAny) {
        im.dirtyAtlas = {ax, ay, w, h};
        im.dirtyAny = true;
    } else {
        DirtyRect& d = im.dirtyAtlas;
        const int nx = std::min(d.x, ax), ny = std::min(d.y, ay);
        const int nr = std::max(d.x + d.w, ax + w), nb = std::max(d.y + d.h, ay + h);
        d = {nx, ny, nr - nx, nb - ny};
    }
    ++im.revision;
    im.byIndex[gi] = m;
    im.glyphs[cp] = m;
    return &im.glyphs[cp];
}

void FontAtlas::preloadAscii() {
    preloadRange(32, 126);
    preloadRange(160, 255);
}

size_t FontAtlas::preloadRange(uint32_t first, uint32_t last) {
    size_t added = 0;
    for (uint32_t cp = first; cp <= last && cp <= 0x10FFFF; ++cp) {
        if (glyph(cp)) ++added;
        if (impl_->full) break;
    }
    return added;
}

void FontAtlas::preload(const std::string& utf8) {
    for (uint32_t cp : decodeUtf8(utf8)) glyph(cp);
}

FontAtlas::DirtyRect FontAtlas::takeDirty() {
    const DirtyRect r = impl_->dirtyAtlas;
    impl_->dirtyAtlas = {};
    impl_->dirtyAny = false;
    return r;
}

std::vector<uint32_t> FontAtlas::decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(extra) >= s.size()) {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings, surrogates and out-of-range values: those can only be corruption or an attack.
        if (!ok || (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

uint32_t FontAtlas::codePointCount(const std::string& utf8) {
    return static_cast<uint32_t>(decodeUtf8(utf8).size());
}

} // namespace ukiyo
