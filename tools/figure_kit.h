// Shared drawing for the offline figure tools: an RGB image, a 5x7 bitmap font, and a PNG writer.
//
// Every figure in docs/ is produced by a small C++ program that runs in CI, so the numbers a caption quotes are the ones
// the code printed. This header is the part those programs have in common. It is deliberately plain: no antialiasing, no
// gradients, no font loading -- a figure is a measurement sheet, and the measurements are drawn with text so a reader can
// see them without the caption.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace figure {

struct Image {
    int w = 0, h = 0;
    std::vector<glm::vec3> color;

    void reset(int width, int height, glm::vec3 fill = glm::vec3(0.0f)) {
        w = width;
        h = height;
        color.assign(size_t(w) * size_t(h), fill);
    }
    void set(int x, int y, glm::vec3 c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        color[size_t(y) * size_t(w) + size_t(x)] = c;
    }
    glm::vec3 get(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return color[size_t(y) * size_t(w) + size_t(x)];
    }
};

// 5x7 rows, bits 4..0 left to right. A-Z, 0-9, then . / : - + % , ( ) = ' and a degree sign.
const uint8_t kGlyphRows[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},      {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},   {6, 8, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 12, 12},      {1, 1, 2, 4, 8, 16, 16},      {0, 12, 12, 0, 12, 12, 0},
    {0, 0, 0, 31, 0, 0, 0},       {0, 4, 4, 31, 4, 4, 0},       {25, 26, 4, 8, 8, 22, 19},
    {8, 20, 20, 20, 20, 20, 27},  {14, 17, 1, 6, 8, 16, 31},    {31, 1, 2, 4, 2, 1, 30},
    {9, 9, 9, 9, 31, 4, 14},      {3, 4, 4, 4, 4, 4, 3},        {7, 8, 8, 8, 8, 8, 7},
};

inline int glyphIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';   // the table is upper case only; lower case reads as upper
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
        case '.': return 36;
        case '/': return 37;
        case ':': return 38;
        case '-': return 39;
        case '+': return 40;
        case '%': return 41;
        case ',': return 42;
        case '(': return 43;
        case ')': return 44;
        case '=': return 45;
        case '\'': return 46;
        default: return -1;
    }
}

inline void fillRect(Image& img, int x0, int y0, int rw, int rh, glm::vec3 c, float alpha = 1.0f) {
    for (int y = std::max(0, y0); y < std::min(img.h, y0 + rh); ++y)
        for (int x = std::max(0, x0); x < std::min(img.w, x0 + rw); ++x)
            img.color[size_t(y) * size_t(img.w) + size_t(x)] =
                glm::mix(img.color[size_t(y) * size_t(img.w) + size_t(x)], c, alpha);
}

inline int textWidth(const std::string& text, int scale) {
    int w = 0;
    for (char ch : text) w += (ch == ' ') ? 4 * scale : 6 * scale;
    return w > 0 ? w - scale : 0;
}

inline void drawText(Image& img, int x, int y, const std::string& text, int scale, glm::vec3 c) {
    for (char ch : text) {
        if (ch == ' ') {
            x += 4 * scale;
            continue;
        }
        const int g = glyphIndex(ch);
        if (g < 0) {
            x += 6 * scale;
            continue;
        }
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (kGlyphRows[g][row] & (1u << (4 - col)))
                    fillRect(img, x + col * scale, y + row * scale, scale, scale, c, 1.0f);
        x += 6 * scale;
    }
}

inline glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

inline void toSrgbInPlace(Image& img) {
    for (glm::vec3& c : img.color) c = toSrgb(c);
}

// Writes the image as 8-bit RGB. Returns false when the path cannot be opened.
inline bool savePng(const Image& img, const std::string& path) {
    std::vector<uint8_t> bytes(size_t(img.w) * size_t(img.h) * 3);
    for (int i = 0; i < img.w * img.h; ++i) {
        const glm::vec3 c = glm::clamp(img.color[size_t(i)], glm::vec3(0.0f), glm::vec3(1.0f));
        bytes[size_t(i) * 3 + 0] = uint8_t(c.x * 255.0f + 0.5f);
        bytes[size_t(i) * 3 + 1] = uint8_t(c.y * 255.0f + 0.5f);
        bytes[size_t(i) * 3 + 2] = uint8_t(c.z * 255.0f + 0.5f);
    }
    if (!stbi_write_png(path.c_str(), img.w, img.h, 3, bytes.data(), img.w * 3)) return false;
    std::printf("wrote %s (%dx%d)\n", path.c_str(), img.w, img.h);
    return true;
}

// A panel header: a title, then a dimmer note, over a dark strip so it reads on any image under it.
inline void panelHeader(Image& img, int x, int y, const std::string& title, const std::string& note) {
    const int w = std::max(textWidth(title, 2), textWidth(note, 2)) + 20;
    fillRect(img, x, y, w, note.empty() ? 30 : 50, glm::vec3(0.02f, 0.02f, 0.03f), 0.72f);
    drawText(img, x + 10, y + 9, title, 2, glm::vec3(0.95f, 0.96f, 0.99f));
    if (!note.empty()) drawText(img, x + 10, y + 29, note, 2, glm::vec3(0.60f, 0.66f, 0.74f));
}

} // namespace figure
