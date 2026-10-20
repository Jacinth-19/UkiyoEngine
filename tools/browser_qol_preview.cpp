// Offline figure for docs/FILESYSTEM.md: the two things Godot 4.8 added to its FileSystem dock (GH-119103), drawn by the
// code Ukiyo's Project browser runs.
//
//   Top:    Ctrl + the wheel zooms the thumbnails. Six of the ladder's thirteen stops are drawn at their real pixel size,
//           inside the grid cell the browser lays out, so the figure and the browser cannot disagree about a number.
//   Bottom: glob search. Each query is run through ukiyo::matchesNameTokens — the function the browser's filter model
//           calls — against the asset list above it, and the names it keeps are printed. The counts are measured, not
//           written by hand: change the grammar and the figure changes with it.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src tools/browser_qol_preview.cpp src/engine/browser_zoom.cpp src/engine/search.cpp
//         -o build/browser_qol_preview
// Run:
//     ./build/browser_qol_preview docs/browser-qol.png 1600 1000
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <glm/glm.hpp>

#include "engine/browser_zoom.h"
#include "engine/search.h"

using namespace ukiyo;

namespace {

// ---- image and text ----------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<glm::vec3> c;
    void reset(int width, int height) {
        w = width;
        h = height;
        c.assign(size_t(w) * h, glm::vec3(0.0f));
    }
};

// 5x7 glyphs: A-Z, 0-9 and the punctuation a query can contain. A '*' and a brace in a caption have to be legible,
// because they are the subject.
const uint8_t kGlyphRows[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},   {6, 8, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 12, 12},      {1, 1, 2, 4, 8, 16, 16},      {0, 12, 12, 0, 12, 12, 0},
    {0, 0, 0, 31, 0, 0, 0},       {0, 4, 4, 31, 4, 4, 0},       {25, 26, 4, 8, 8, 22, 19},
    {4, 21, 14, 31, 14, 21, 4},   {14, 17, 1, 2, 4, 0, 4},      {30, 16, 16, 16, 16, 16, 30},
    {15, 1, 1, 1, 1, 1, 15},      {6, 8, 8, 16, 8, 8, 6},       {12, 2, 2, 1, 2, 2, 12},
    {0, 0, 0, 0, 12, 4, 8},       {2, 4, 8, 16, 8, 4, 2},       {8, 4, 2, 1, 2, 4, 8},
    {0, 0, 31, 0, 31, 0, 0},      {0, 0, 0, 0, 0, 0, 31},       {4, 4, 4, 4, 4, 0, 4},
};

int glyphIndex(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= '0' && ch <= '9') return 26 + (ch - '0');
    switch (ch) {
        case '.': return 36;   // .
        case '/': return 37;   // /
        case ':': return 38;   // :
        case '-': return 39;   // -
        case '+': return 40;   // +
        case '%': return 41;   // %
        case '*': return 42;   // *
        case '?': return 43;   // ?
        case '[': return 44;   // [
        case ']': return 45;   // ]
        case '{': return 46;   // {
        case '}': return 47;   // }
        case ',': return 48;   // ,
        case '<': return 49;   // <
        case '>': return 50;   // >
        case '=': return 51;   // =
        case '_': return 52;   // _
        case '!': return 53;   // !
        default: return -1;
    }
}

void fillRect(Image& img, int x0, int y0, int rw, int rh, glm::vec3 c, float alpha = 1.0f) {
    for (int y = std::max(0, y0); y < std::min(img.h, y0 + rh); ++y)
        for (int x = std::max(0, x0); x < std::min(img.w, x0 + rw); ++x)
            img.c[size_t(y) * img.w + x] = glm::mix(img.c[size_t(y) * img.w + x], c, alpha);
}

void strokeRect(Image& img, int x, int y, int rw, int rh, glm::vec3 c, int t = 1) {
    fillRect(img, x, y, rw, t, c);
    fillRect(img, x, y + rh - t, rw, t, c);
    fillRect(img, x, y, t, rh, c);
    fillRect(img, x + rw - t, y, t, rh, c);
}

int textWidth(const std::string& text, int scale) {
    int w = 0;
    for (char ch : text) w += (ch == ' ') ? 4 * scale : 6 * scale;
    return w > 0 ? w - scale : 0;
}

void drawText(Image& img, int x, int y, const std::string& text, int scale, glm::vec3 c) {
    for (char raw : text) {
        const char ch = (raw >= 'a' && raw <= 'z') ? char(raw - 32) : raw;   // the font is upper case only
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
                    fillRect(img, x + col * scale, y + row * scale, scale, scale, c);
        x += 6 * scale;
    }
}

// Text that stops at a column instead of running past it, with the same elide the browser uses on a long name.
void drawTextClipped(Image& img, int x, int y, const std::string& text, int scale, int maxWidth, glm::vec3 c) {
    std::string shown;
    int w = 0;
    bool cut = false;
    for (char ch : text) {
        const int adv = (ch == ' ') ? 4 * scale : 6 * scale;
        if (w + adv > maxWidth) {
            cut = true;
            break;
        }
        shown += ch;
        w += adv;
    }
    if (cut) {
        // Two dots, the way the browser elides: the reader has to see that a name continues rather than read a wrong one.
        while (!shown.empty() && w + 12 * scale > maxWidth) {
            w -= (shown.back() == ' ') ? 4 * scale : 6 * scale;
            shown.pop_back();
        }
        shown += "..";
    }
    drawText(img, x, y, shown, scale, c);
}

glm::vec3 toSrgb(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f));
}

// ---- palette -----------------------------------------------------------------------------------

const glm::vec3 kBg{0.055f, 0.065f, 0.080f};
const glm::vec3 kPanel{0.085f, 0.100f, 0.120f};
const glm::vec3 kRule{0.20f, 0.235f, 0.275f};
const glm::vec3 kText{0.86f, 0.89f, 0.93f};
const glm::vec3 kDim{0.47f, 0.52f, 0.58f};
const glm::vec3 kGreen{0.48f, 0.85f, 0.56f};
const glm::vec3 kCyan{0.34f, 0.76f, 0.84f};
const glm::vec3 kAmber{0.90f, 0.71f, 0.40f};

// ---- a thumbnail ------------------------------------------------------------------------------

// The picture inside a tile: a horizon, a hill, a sun and a couple of pines, drawn at whatever size the tile is. It only
// has to be recognisable at 32 px, which is the bottom of the ladder.
void drawThumbArt(Image& img, int x, int y, int size, float hue) {
    for (int row = 0; row < size; ++row) {
        const float t = float(row) / float(std::max(1, size - 1));
        const glm::vec3 sky = glm::mix(glm::vec3(0.10f, 0.20f, 0.34f), glm::vec3(0.42f, 0.30f, 0.38f), t);
        fillRect(img, x, y + row, size, 1, sky);
    }
    // Sun.
    const int sunR = std::max(1, size / 10);
    const int sunX = x + int(size * 0.70f), sunY = y + int(size * 0.28f);
    for (int dy = -sunR; dy <= sunR; ++dy)
        for (int dx = -sunR; dx <= sunR; ++dx)
            if (dx * dx + dy * dy <= sunR * sunR) fillRect(img, sunX + dx, sunY + dy, 1, 1, glm::vec3(0.96f, 0.78f, 0.44f));

    // Two hills, back one darker.
    for (int col = 0; col < size; ++col) {
        const float u = float(col) / float(std::max(1, size - 1));
        const float back = 0.52f + 0.10f * std::sin(u * 3.1f + 0.7f);
        const float front = 0.66f + 0.09f * std::sin(u * 5.7f + 2.1f);
        const int backY = y + int(size * back);
        const int frontY = y + int(size * front);
        fillRect(img, x + col, backY, 1, size - int(size * back), glm::mix(glm::vec3(0.16f, 0.24f, 0.22f), glm::vec3(0.22f, 0.33f, 0.26f), hue));
        fillRect(img, x + col, frontY, 1, size - int(size * front), glm::mix(glm::vec3(0.09f, 0.15f, 0.14f), glm::vec3(0.13f, 0.21f, 0.16f), hue));
    }
    // Pines, taller on the bigger tiles so the picture keeps its detail as it grows.
    const int pines = size >= 96 ? 3 : (size >= 48 ? 2 : 1);
    for (int p = 0; p < pines; ++p) {
        const float px = 0.16f + 0.22f * float(p);
        const int baseY = y + int(size * (0.80f + 0.03f * float(p % 2)));
        const int height = std::max(3, int(size * 0.22f));
        const int halfW = std::max(1, height / 3);
        for (int row = 0; row < height; ++row) {
            const int wide = std::max(1, halfW * row / std::max(1, height - 1));
            fillRect(img, x + int(size * px) - wide, baseY - height + row, wide * 2 + 1, 1, glm::vec3(0.06f, 0.12f, 0.10f));
        }
    }
    strokeRect(img, x, y, size, size, glm::vec3(0.24f, 0.27f, 0.31f));
}

// One grid cell exactly as the browser lays it out: the icon at the thumbnail size, the cell padded by the engine's
// numbers (24 across, 44 down), and the file name under the icon.
void drawCell(Image& img, int x, int baseline, int px, const std::string& name, float hue) {
    const int cw = thumbGridWidth(px);
    const int ch = thumbGridHeight(px);
    const int top = baseline - ch;
    strokeRect(img, x, top, cw, ch, glm::vec3(0.24f, 0.28f, 0.33f));
    const int iconX = x + (cw - px) / 2;
    const int iconY = top + 6;
    drawThumbArt(img, iconX, iconY, px, hue);
    if (px >= 48) drawTextClipped(img, x + 4, iconY + px + 6, name, 1, cw - 8, kDim);
}

// ---- the asset list and the queries ------------------------------------------------------------

struct Query {
    std::string text;
    std::string note;
};

}  // namespace

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "docs/browser-qol.png";
    const int W = argc > 2 ? std::atoi(argv[2]) : 1600;
    const int H = argc > 3 ? std::atoi(argv[3]) : 1000;

    Image img;
    img.reset(W, H);
    for (int y = 0; y < H; ++y) {
        const float t = float(y) / float(std::max(1, H - 1));
        fillRect(img, 0, y, W, 1, glm::mix(kBg, glm::vec3(0.075f, 0.085f, 0.10f), t));
    }

    const int margin = 60;

    // ---- header -------------------------------------------------------------------------------
    drawText(img, margin, 40, "THE PROJECT BROWSER: WHEEL ZOOM AND GLOB SEARCH", 4, kText);
    drawText(img, margin, 80, "GODOT 4.8 GH-119103, IN UKIYO'S EDITOR: SRC/ENGINE/BROWSER_ZOOM.CPP AND SRC/ENGINE/SEARCH.CPP", 2, kDim);
    fillRect(img, margin, 108, W - 2 * margin, 1, kRule);

    // ---- zoom band ----------------------------------------------------------------------------
    drawText(img, margin, 128, "CTRL + WHEEL: SIX OF THE THIRTEEN STOPS, DRAWN AT THEIR REAL SIZE", 2, kCyan);

    const int stops[] = {32, 64, 96, 128, 160, 224};
    const float hues[] = {0.2f, 0.45f, 0.6f, 0.0f, 0.75f, 0.35f};
    const int baseline = 396;
    int total = 0;
    for (int s : stops) total += thumbGridWidth(s);
    const int gap = (W - 2 * margin - total) / (int(std::size(stops)) - 1);
    int x = margin;
    for (int i = 0; i < int(std::size(stops)); ++i) {
        const int px = stops[i];
        drawCell(img, x, baseline, px, "HERO.VRM", hues[i]);
        if (px == kThumbDefault) {
            // The size the browser has always opened at, marked once.
            strokeRect(img, x - 3, baseline - thumbGridHeight(px) - 3, thumbGridWidth(px) + 6, thumbGridHeight(px) + 6,
                       kAmber, 2);
        }
        char label[48];
        std::snprintf(label, sizeof(label), "%d PX", px);
        const int lw = textWidth(label, 2);
        drawText(img, x + (thumbGridWidth(px) - lw) / 2, baseline + 12, label, 2, px == kThumbDefault ? kAmber : kText);
        x += thumbGridWidth(px);
        if (i + 1 < int(std::size(stops))) x += gap;
    }
    drawText(img, margin, baseline + 36,
             "THE GRID CELL IS THE ICON PLUS 24 PX ACROSS AND 44 DOWN. THE AMBER CELL IS THE DEFAULT, 96 PX.", 2, kDim);

    // The slider track: every stop, the current value, and the two ends.
    const int trackY = baseline + 66;
    fillRect(img, margin, trackY, W - 2 * margin, 3, kRule);
    const int trackW = W - 2 * margin;
    for (int i = 0; i < kThumbStops; ++i) {
        const int v = kThumbMin + i * kThumbStep;
        const int tx = margin + int(float(v - kThumbMin) / float(kThumbMax - kThumbMin) * float(trackW));
        fillRect(img, tx - 1, trackY - 6, 3, 15, i == 0 || i == kThumbStops - 1 ? kText : kRule);
        char tick[8];
        std::snprintf(tick, sizeof(tick), "%d", v);
        const int tw = textWidth(tick, 1);
        drawText(img, std::clamp(tx - tw / 2, margin, W - margin - tw), trackY + 14, tick, 1, kDim);
    }
    // The default's marker on the track.
    {
        const int tx = margin + int(float(kThumbDefault - kThumbMin) / float(kThumbMax - kThumbMin) * float(trackW));
        fillRect(img, tx - 2, trackY - 12, 5, 27, kAmber);
    }
    drawText(img, margin, trackY + 34, "ONE NOTCH IS +16 PX. THE SLIDER, THE WHEEL AND THE GRID ALL READ THIS LADDER.", 2, kDim);

    // ---- search band --------------------------------------------------------------------------
    fillRect(img, margin, trackY + 58, W - 2 * margin, 1, kRule);
    drawText(img, margin, trackY + 70, "GLOB SEARCH: A WORD IS A SUBSTRING, A TOKEN WITH WILDCARDS IS A GLOB", 2, kCyan);

    const std::vector<std::string> assets{"Hero.vrm",      "Brass Sword.vrm",   "Sword Idle.ukscene",
                                          "Sword Run.ukscene", "Sword.png",     "Swords/Blade.vrm",
                                          "Swords/Blade.ukscene", "readme.txt"};

    // The list itself, as chips in one row.
    int chipX = margin;
    const int chipY = trackY + 96;
    for (const std::string& name : assets) {
        const int cw = textWidth(name, 1) + 14;
        if (chipX + cw > W - margin) break;
        fillRect(img, chipX, chipY, cw, 20, glm::vec3(0.13f, 0.16f, 0.19f));
        drawText(img, chipX + 7, chipY + 6, name, 1, kText);
        chipX += cw + 8;
    }
    drawText(img, margin, chipY + 28, "EIGHT ASSETS, TWO OF THEM INSIDE A FOLDER CALLED SWORDS", 1, kDim);

    const std::vector<Query> queries{
        {"sword", "A WORD: EVERY NAME THAT CONTAINS IT"},
        {"*.vrm", "A GLOB: ONE TYPE OF FILE"},
        {"sword *.vrm", "BOTH TOKENS HAVE TO MATCH"},
        {"s?ord.png", "ONE CHARACTER OF ANY KIND"},
        {"*{vrm,ukscene}", "A BRACE SET PICKS BETWEEN ENDINGS"},
        {"**/*.ukscene", "DOUBLE STAR: HERE OR IN ANY FOLDER"},
        {"hero.vrm", "A DOT IS A LITERAL, NOT A WILDCARD"},
        {"*.png sword", "ORDER DOES NOT MATTER"},
    };

    int rowY = chipY + 54;
    const int rowH = 44;
    int totalMatched = 0;
    std::printf("browser figure: %d assets\n", int(assets.size()));
    for (const Query& q : queries) {
        std::vector<std::string> kept;
        for (const std::string& name : assets)
            if (matchesNameTokens(name, q.text)) kept.push_back(name);
        totalMatched += int(kept.size());
        std::string joined;
        for (size_t i = 0; i < kept.size(); ++i) joined += (i ? ", " : "") + kept[i];

        std::printf("  %-16s keeps %d: %s\n", q.text.c_str(), int(kept.size()), joined.c_str());

        // Query column, then the names it keeps, then the count on the right.
        drawText(img, margin, rowY + 4, q.text, 2, kGreen);
        const int countW = textWidth(std::to_string(kept.size()) + " OF 8", 2);
        drawTextClipped(img, margin + 250, rowY + 4, joined.empty() ? "NOTHING" : joined, 2, W - 2 * margin - 250 - countW - 30,
                        kept.empty() ? kDim : kText);
        drawText(img, W - margin - countW, rowY + 4, std::to_string(kept.size()) + " OF 8", 2, kDim);
        drawText(img, margin, rowY + 24, q.note, 1, kDim);
        fillRect(img, margin, rowY + rowH - 8, W - 2 * margin, 1, glm::vec3(0.13f, 0.15f, 0.18f));
        rowY += rowH;
    }

    // ---- footer -------------------------------------------------------------------------------
    char footer[256];
    std::snprintf(footer, sizeof(footer),
                  "%d ASSETS, %d MATCHES ACROSS EIGHT QUERIES. THE GRAMMAR IS ENGINE/SEARCH.H; THE LADDER IS ENGINE/BROWSER_ZOOM.H.",
                  int(assets.size()), totalMatched);
    drawText(img, margin, H - 34, footer, 2, kDim);
    drawText(img, margin, H - 16, "THE SAME RULES RUN IN THE EDITOR (EDITOR/SRC/MAIN_WINDOW.CPP) AND COMPILE FOR ANDROID IN CI.", 1, glm::vec3(0.33f, 0.36f, 0.40f));

    // ---- write --------------------------------------------------------------------------------
    std::vector<uint8_t> rgb(size_t(W) * H * 3);
    for (size_t i = 0; i < img.c.size(); ++i) {
        const glm::vec3 c = toSrgb(img.c[i]);
        rgb[i * 3 + 0] = uint8_t(std::clamp(c.x, 0.0f, 1.0f) * 255.0f + 0.5f);
        rgb[i * 3 + 1] = uint8_t(std::clamp(c.y, 0.0f, 1.0f) * 255.0f + 0.5f);
        rgb[i * 3 + 2] = uint8_t(std::clamp(c.z, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    if (!stbi_write_png(out, W, H, 3, rgb.data(), W * 3)) {
        std::fprintf(stderr, "could not write %s\n", out);
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", out, W, H);
    return 0;
}
