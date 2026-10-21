#include "engine/hud.h"

#include <algorithm>
#include <cmath>

namespace ukiyo {

namespace {
constexpr int kMaxClipDepth = 32;
}

HudColor HudColor::hex(uint32_t rgb, float a) {
    return HudColor(float((rgb >> 16) & 0xff) / 255.0f, float((rgb >> 8) & 0xff) / 255.0f, float(rgb & 0xff) / 255.0f, a);
}

void HudCanvas::setFont(FontAtlas* font) {
    font_ = font;
    if (font_) {
        whiteU_ = 0.5f / static_cast<float>(font_->width());
        whiteV_ = 0.5f / static_cast<float>(font_->height());
    }
}

void HudCanvas::setScreen(float widthPx, float heightPx) {
    screenW_ = std::max(widthPx, 1.0f);
    screenH_ = std::max(heightPx, 1.0f);
}

void HudCanvas::clear() {
    vertices_.clear();
    clips_.clear();
    clip_ = HudRect{0.0f, 0.0f, screenW_, screenH_};
}

void HudCanvas::pushClip(const HudRect& r) {
    if (clips_.size() >= static_cast<size_t>(kMaxClipDepth)) return;
    // Intersect with the current clip so nested clips never grow the visible area.
    const float x0 = std::max(clip_.x, r.x);
    const float y0 = std::max(clip_.y, r.y);
    const float x1 = std::min(clip_.x + clip_.w, r.x + r.w);
    const float y1 = std::min(clip_.y + clip_.h, r.y + r.h);
    clips_.push_back(clip_);
    clip_ = HudRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

void HudCanvas::popClip() {
    if (clips_.empty()) return;
    clip_ = clips_.back();
    clips_.pop_back();
}

void HudCanvas::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                     const glm::vec4& c) {
    // Two triangles, counter-clockwise in a y-down pixel space. The renderer flips y, which keeps the winding valid.
    vertices_.push_back({glm::vec2(x0, y0), glm::vec2(u0, v0), c});
    vertices_.push_back({glm::vec2(x1, y0), glm::vec2(u1, v0), c});
    vertices_.push_back({glm::vec2(x1, y1), glm::vec2(u1, v1), c});
    vertices_.push_back({glm::vec2(x0, y0), glm::vec2(u0, v0), c});
    vertices_.push_back({glm::vec2(x1, y1), glm::vec2(u1, v1), c});
    vertices_.push_back({glm::vec2(x0, y1), glm::vec2(u0, v1), c});
}

void HudCanvas::addClippedQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                               const glm::vec4& c) {
    if (x1 <= x0 || y1 <= y0) return;
    // Clip against the active scissor by adjusting the quad and interpolating the UVs, so a partly visible label stays
    // aligned with the part that was cut off.
    const float cx0 = clip_.x, cy0 = clip_.y, cx1 = clip_.x + clip_.w, cy1 = clip_.y + clip_.h;
    if (x1 <= cx0 || x0 >= cx1 || y1 <= cy0 || y0 >= cy1) return;
    const float w = x1 - x0, h = y1 - y0;
    const float nx0 = std::max(x0, cx0), ny0 = std::max(y0, cy0);
    const float nx1 = std::min(x1, cx1), ny1 = std::min(y1, cy1);
    const float nu0 = u0 + (nx0 - x0) / w * (u1 - u0);
    const float nu1 = u0 + (nx1 - x0) / w * (u1 - u0);
    const float nv0 = v0 + (ny0 - y0) / h * (v1 - v0);
    const float nv1 = v0 + (ny1 - y0) / h * (v1 - v0);
    quad(nx0, ny0, nx1, ny1, nu0, nv0, nu1, nv1, c);
}

void HudCanvas::rect(const HudRect& r, const HudColor& c) {
    addClippedQuad(r.x, r.y, r.x + r.w, r.y + r.h, whiteU_, whiteV_, whiteU_, whiteV_, c.v());
}

void HudCanvas::border(const HudRect& r, float t, const HudColor& c) {
    if (t <= 0.0f) return;
    rect({r.x, r.y, r.w, t}, c);                 // top
    rect({r.x, r.y + r.h - t, r.w, t}, c);        // bottom
    rect({r.x, r.y + t, t, r.h - 2.0f * t}, c);   // left
    rect({r.x + r.w - t, r.y + t, t, r.h - 2.0f * t}, c);
}

void HudCanvas::bar(const HudRect& r, float fraction, const HudColor& back, const HudColor& fill) {
    rect(r, back);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0.0f) rect({r.x, r.y, r.w * f, r.h}, fill);
}

void HudCanvas::dim(const HudColor& c) { rect({0.0f, 0.0f, screenW_, screenH_}, c); }

float HudCanvas::scaleFor(float size) const {
    if (!font_ || font_->pixelHeight() <= 0.0f) return 1.0f;
    if (size <= 0.0f) return 1.0f;
    return size / font_->pixelHeight();
}

float HudCanvas::lineHeight(float size) const {
    if (!font_) return 0.0f;
    return font_->lineHeight() * scaleFor(size);
}

float HudCanvas::textWidth(const std::string& utf8, float size) const {
    if (!font_) return 0.0f;
    const float s = scaleFor(size);
    float w = 0.0f;
    for (uint32_t cp : FontAtlas::decodeUtf8(utf8)) {
        const GlyphMetrics* g = font_->glyph(cp);
        if (!g) continue;
        w += g->advance * s;
    }
    return w;
}

const GlyphMetrics* HudCanvas::glyphFor(uint32_t cp) const {
    return font_ ? font_->glyph(cp) : nullptr;
}

void HudCanvas::text(float x, float y, const std::string& utf8, const HudColor& c, float size) {
    if (!font_ || !font_->loaded()) return;
    const float s = scaleFor(size);
    const float baseline = y + font_->ascent() * s;
    float pen = x;
    const glm::vec4 color = c.v();
    // Glyph metrics are cached; decoding into a vector first keeps the loop free of iterator invalidation concerns when
    // the atlas grows (it never invalidates metrics, but this also avoids re-decoding per glyph).
    for (uint32_t cp : FontAtlas::decodeUtf8(utf8)) {
        const GlyphMetrics* g = glyphFor(cp);
        if (!g) continue;
        if (g->width > 0.0f && g->height > 0.0f) {
            const float gx = pen + g->xoff * s;
            const float gy = baseline + g->yoff * s;
            addClippedQuad(gx, gy, gx + g->width * s, gy + g->height * s, g->u0, g->v0, g->u1, g->v1, color);
        }
        pen += g->advance * s;
    }
}

void HudCanvas::textShadowed(float x, float y, const std::string& utf8, const HudColor& c, float size) {
    text(x + 1.0f, y + 1.0f, utf8, HudColor(0.0f, 0.0f, 0.0f, c.a * 0.85f), size);
    text(x, y, utf8, c, size);
}

void HudCanvas::textCentered(const HudRect& box, float y, const std::string& utf8, const HudColor& c, float size,
                             HudAlign align) {
    const float w = textWidth(utf8, size);
    float x = box.x;
    if (align == HudAlign::Center) x = box.x + (box.w - w) * 0.5f;
    else if (align == HudAlign::Right) x = box.x + box.w - w;
    text(x, y, utf8, c, size);
}

std::vector<std::string> HudCanvas::wrap(const std::string& utf8, float maxWidth, float size) const {
    std::vector<std::string> lines;
    if (!font_ || maxWidth <= 0.0f) {
        lines.push_back(utf8);
        return lines;
    }
    // Break at spaces where they exist; Japanese has none, so a line that cannot grow by a character is broken there.
    std::string line;
    float lineW = 0.0f;
    auto flush = [&](bool includeTrailingSpace) {
        (void)includeTrailingSpace;
        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
            lineW = 0.0f;
        }
    };
    const std::vector<uint32_t> cps = FontAtlas::decodeUtf8(utf8);
    std::string pendingSpace;
    auto encode = [](uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (uint32_t cp : cps) {
        if (cp == '\n') {
            flush(false);
            pendingSpace.clear();
            continue;
        }
        const GlyphMetrics* g = font_->glyph(cp);
        const float adv = g ? g->advance * scaleFor(size) : 0.0f;
        if (cp == ' ') {
            pendingSpace.push_back(' ');
            if (lineW + adv <= maxWidth) {
                encode(cp, &line);
                lineW += adv;
                pendingSpace.clear();
            } else {
                flush(false);
                pendingSpace.clear();
            }
            continue;
        }
        if (lineW + adv > maxWidth && !line.empty()) {
            flush(false);
            pendingSpace.clear();
        }
        encode(cp, &line);
        lineW += adv;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back(std::string());
    return lines;
}

int HudCanvas::textBox(const HudRect& box, const std::string& utf8, const HudColor& c, float size, float lineGap) {
    pushClip(box);
    const std::vector<std::string> lines = wrap(utf8, box.w, size);
    const float lh = lineHeight(size) + lineGap;
    float y = box.y;
    int drawn = 0;
    for (const std::string& l : lines) {
        if (y + lh > box.y + box.h && drawn > 0) break;   // do not draw past the box
        text(box.x, y, l, c, size);
        y += lh;
        ++drawn;
    }
    popClip();
    return drawn;
}

} // namespace ukiyo
