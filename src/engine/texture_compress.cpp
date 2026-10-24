// The BCn codecs; the contract is in texture_compress.h.
//
// Bit layout notes, from the Khronos Data Format Specification chapter 22 (S3TC) and the RGTC chapter, because these
// are the two things that make or break the stream:
//   * A BC1 block is two RGB565 endpoints (little-endian) then sixteen 2-bit indices, packed LSB-first, texel order
//     row-major x-fastest. When c0 <= c1 numerically the block is the "punchthrough" mode and index 3 is transparent
//     black; this engine only emits BC1 for opaque textures, so the encoder keeps c0 > c1 and all four palette
//     entries are colours.
//   * A BC4/alpha block is two 8-bit endpoints then sixteen 3-bit indices (48 bits, six bytes). With a0 > a1 the
//     palette is a0, a1 and six interpolations; with a0 <= a1 it is a0, a1 and four interpolations plus two zeros.
//     The encoder always orders a0 > a1 (the 8-value mode).
//   * BC3 is an alpha block followed by a BC1 colour block; BC5 is a red block followed by a green block.
#include "engine/texture_compress.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <cmath>
#include <cstring>

#include "astcenc.h"

namespace ukiyo::texc {

bool compressed(rhi::Format f) {
    switch (f) {
    case rhi::Format::BC1:
    case rhi::Format::BC1_SRGB:
    case rhi::Format::BC3:
    case rhi::Format::BC3_SRGB:
    case rhi::Format::BC5:
    case rhi::Format::ASTC_4x4:
    case rhi::Format::ASTC_4x4_SRGB: return true;
    default: return false;
    }
}

bool isASTC(rhi::Format f) { return f == rhi::Format::ASTC_4x4 || f == rhi::Format::ASTC_4x4_SRGB; }

int blockBytes(rhi::Format f) {
    switch (f) {
    case rhi::Format::BC1:
    case rhi::Format::BC1_SRGB: return 8;
    case rhi::Format::BC3:
    case rhi::Format::BC3_SRGB:
    case rhi::Format::BC5:
    case rhi::Format::ASTC_4x4:
    case rhi::Format::ASTC_4x4_SRGB: return 16;   // ASTC 4x4: every block is exactly 128 bits
    default: return 0;
    }
}

int blocksWide(int w) { return (std::max(w, 1) + kBlockWidth - 1) / kBlockWidth; }
int blocksHigh(int h) { return (std::max(h, 1) + kBlockHeight - 1) / kBlockHeight; }

size_t levelBytes(rhi::Format f, int w, int h) {
    return static_cast<size_t>(blocksWide(w)) * static_cast<size_t>(blocksHigh(h)) *
           static_cast<size_t>(blockBytes(f));
}

namespace {

// ---- RGB565 packing and the 4-colour palette ----

uint16_t pack565(const int c[3]) {
    const int r = std::clamp(c[0], 0, 255) >> 3;
    const int g = std::clamp(c[1], 0, 255) >> 2;
    const int b = std::clamp(c[2], 0, 255) >> 3;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void unpack565(uint16_t v, int out[3]) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

// Squared distance between two 3-channel colours, the metric the indices are chosen by.
int colourDistance(const int a[3], const int b[3]) {
    const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

// Clamped fetch of texel (x, y) from a w x h RGBA8 image.
void fetchTexel(const uint8_t* rgba, int w, int h, int x, int y, uint8_t out[4]) {
    const int cx = std::clamp(x, 0, w - 1), cy = std::clamp(y, 0, h - 1);
    const uint8_t* p = rgba + (static_cast<size_t>(cy) * w + cx) * 4;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
}

// ---- BC1: one 4x4 colour block -> eight bytes ----

void encodeBlockBC1(const uint8_t* rgba, int w, int h, int bx, int by, uint8_t out[8]) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    uint8_t t[4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            fetchTexel(rgba, w, h, bx * 4 + x, by * 4 + y, t);
            for (int c = 0; c < 3; ++c) {
                mn[c] = std::min(mn[c], int(t[c]));
                mx[c] = std::max(mx[c], int(t[c]));
            }
        }

    // Endpoints: the bounding box in 565, then a greedy refinement — each endpoint walks one step along one 565
    // axis at a time and keeps the step that lowers the block's total squared error. A handful of evaluations per
    // block, and a visible gain on gradients over the raw box corners.
    int e0[3] = {mx[0], mx[1], mx[2]}, e1[3] = {mn[0], mn[1], mn[2]};
    auto blockError = [&](const int a[3], const int b[3]) {
        int p[4][3];
        unpack565(pack565(a), p[0]);
        unpack565(pack565(b), p[1]);
        for (int c = 0; c < 3; ++c) {
            p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
            p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
        }
        long long err = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                fetchTexel(rgba, w, h, bx * 4 + x, by * 4 + y, t);
                const int c0[3] = {t[0], t[1], t[2]};
                int best = colourDistance(c0, p[0]);
                for (int i = 1; i < 4; ++i) best = std::min(best, colourDistance(c0, p[i]));
                err += best;
            }
        return err;
    };
    for (int endpoint = 0; endpoint < 2; ++endpoint) {
        int* e = endpoint == 0 ? e0 : e1;
        for (int axis = 0; axis < 3; ++axis) {
            for (int step = -1; step <= 1; step += 2) {
                const int old = e[axis];
                e[axis] = std::clamp(old + step * 8, 0, 255);
                const long long with = blockError(e0, e1);
                e[axis] = old;
                if (with < blockError(e0, e1)) e[axis] = std::clamp(old + step * 8, 0, 255);
            }
        }
    }

    uint16_t c0 = pack565(e0), c1 = pack565(e1);
    if (c0 < c1) std::swap(c0, c1);   // keep the four-colour mode: the encoder is opaque-only by contract
    if (c0 == c1) {
        // A flat block: index 0 decodes to c0 everywhere, which is the right answer regardless of mode.
        std::memset(out, 0, 8);
        out[0] = static_cast<uint8_t>(c0 & 0xFF);
        out[1] = static_cast<uint8_t>(c0 >> 8);
        return;
    }

    int p[4][3];
    unpack565(c0, p[0]);
    unpack565(c1, p[1]);
    for (int c = 0; c < 3; ++c) {
        p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
        p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
    }
    uint64_t indices = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            fetchTexel(rgba, w, h, bx * 4 + x, by * 4 + y, t);
            const int c4[3] = {t[0], t[1], t[2]};
            int best = 0;
            long long bestErr = colourDistance(c4, p[0]);
            for (int i = 1; i < 4; ++i) {
                const long long err = colourDistance(c4, p[i]);
                if (err < bestErr) {
                    bestErr = err;
                    best = i;
                }
            }
            indices |= static_cast<uint64_t>(best) << (2 * (y * 4 + x));
        }
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &indices, 4);   // 16 indices x 2 bits = 32 bits, LSB first
}

// ---- BC4: one 4x4 single-channel block -> eight bytes (the alpha half of BC3, red or green half of BC5) ----

void encodeBlockBC4(const uint8_t* data, int stride, int w, int h, int bx, int by, uint8_t out[8]) {
    uint8_t a0 = 0, a1 = 255;
    uint8_t t;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int cx = std::clamp(bx * 4 + x, 0, w - 1), cy = std::clamp(by * 4 + y, 0, h - 1);
            t = data[(static_cast<size_t>(cy) * w + cx) * stride];
            a0 = std::max(a0, t);
            a1 = std::min(a1, t);
        }
    if (a0 == a1) {
        // Flat: endpoint 0 with all indices 0 decodes to a0.
        out[0] = a0;
        out[1] = a1;
        std::memset(out + 2, 0, 6);
        return;
    }
    out[0] = a0;
    out[1] = a1;
    uint8_t palette[8];
    palette[0] = a0;
    palette[1] = a1;
    // The spec's eight-value mode: six interpolated steps of (7-s)/7 across the endpoints, s = 1..6.
    for (int s = 1; s <= 6; ++s) palette[1 + s] = static_cast<uint8_t>(((7 - s) * a0 + s * a1) / 7);
    uint64_t indices = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int cx = std::clamp(bx * 4 + x, 0, w - 1), cy = std::clamp(by * 4 + y, 0, h - 1);
            t = data[(static_cast<size_t>(cy) * w + cx) * stride];
            int best = 0;
            int bestErr = std::abs(int(t) - int(palette[0]));
            for (int i = 1; i < 8; ++i) {
                const int err = std::abs(int(t) - int(palette[i]));
                if (err < bestErr) {
                    bestErr = err;
                    best = i;
                }
            }
            indices |= static_cast<uint64_t>(best) << (3 * (y * 4 + x));
        }
    // 16 x 3 bits = 48 bits, LSB first, over the six bytes after the endpoints.
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>((indices >> (8 * i)) & 0xFF);
}

} // namespace

std::vector<uint8_t> encodeBC1(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> out(levelBytes(rhi::Format::BC1, w, h));
    for (int by = 0; by < blocksHigh(h); ++by)
        for (int bx = 0; bx < blocksWide(w); ++bx)
            encodeBlockBC1(rgba, w, h, bx, by, out.data() + (static_cast<size_t>(by) * blocksWide(w) + bx) * 8);
    return out;
}

std::vector<uint8_t> encodeBC3(const uint8_t* rgba, int w, int h) {
    // Alpha channel first, then the colour block: the format's own order.
    std::vector<uint8_t> out(levelBytes(rhi::Format::BC3, w, h));
    for (int by = 0; by < blocksHigh(h); ++by)
        for (int bx = 0; bx < blocksWide(w); ++bx) {
            uint8_t* block = out.data() + (static_cast<size_t>(by) * blocksWide(w) + bx) * 16;
            encodeBlockBC4(rgba + 3, 4, w, h, bx, by, block);          // stride 4: alpha lives in byte 3
            encodeBlockBC1(rgba, w, h, bx, by, block + 8);
        }
    return out;
}

std::vector<uint8_t> encodeBC5(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> out(levelBytes(rhi::Format::BC5, w, h));
    for (int by = 0; by < blocksHigh(h); ++by)
        for (int bx = 0; bx < blocksWide(w); ++bx) {
            uint8_t* block = out.data() + (static_cast<size_t>(by) * blocksWide(w) + bx) * 16;
            encodeBlockBC4(rgba, 4, w, h, bx, by, block);              // red
            encodeBlockBC4(rgba + 1, 4, w, h, bx, by, block + 8);      // green
        }
    return out;
}

// ------------------------------------------------------------------ ASTC (vendored astc-encoder)

namespace {

// One compression context per call. astc-encoder's context carries the partition tables and the working queues —
// about 180 KB — and this loader compresses one texture at a time on a phone, so the alloc/free cost per level is
// noise next to the encode itself.
struct AstcContext {
    astcenc_context* ctx = nullptr;
    ~AstcContext() { if (ctx) astcenc_context_free(ctx); }
};

astcenc_swizzle identitySwizzle() { return {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A}; }

std::vector<uint8_t> astcEncode(const uint8_t* rgba, int w, int h, bool srgb, unsigned flags) {
    astcenc_config config{};
    astcenc_error err = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1,
                                            ASTCENC_PRE_FAST, flags, &config);
    if (err != ASTCENC_SUCCESS) throw std::runtime_error(std::string("astcenc config: ") +
                                                         astcenc_get_error_string(err));
    AstcContext holder;
    err = astcenc_context_alloc(&config, 1, &holder.ctx, nullptr);
    if (err != ASTCENC_SUCCESS) throw std::runtime_error(std::string("astcenc context: ") +
                                                         astcenc_get_error_string(err));
    std::vector<uint8_t> out(levelBytes(rhi::Format::ASTC_4x4, w, h));
    // The slice-pointer array must outlive the call: astcenc_image.data is read inside compress.
    void* slice = const_cast<uint8_t*>(rgba);
    astcenc_image image{static_cast<unsigned int>(w), static_cast<unsigned int>(h), 1u, ASTCENC_TYPE_U8, &slice};
    const astcenc_swizzle swz = identitySwizzle();
    err = astcenc_compress_image(holder.ctx, &image, &swz, out.data(), out.size(), 0);
    if (err != ASTCENC_SUCCESS) throw std::runtime_error(std::string("astcenc compress: ") +
                                                         astcenc_get_error_string(err));
    return out;
}

} // namespace

std::vector<uint8_t> encodeASTC(const uint8_t* rgba, int w, int h, bool srgb) {
    return astcEncode(rgba, w, h, srgb, 0);
}

// ------------------------------------------------------------------ decoding

namespace {

void decodeBlockBC1(const uint8_t* in, int out[16][4]) {
    uint16_t c0, c1;
    std::memcpy(&c0, in, 2);
    std::memcpy(&c1, in + 2, 2);
    uint32_t indices;
    std::memcpy(&indices, in + 4, 4);
    int p[4][3];
    unpack565(c0, p[0]);
    unpack565(c1, p[1]);
    for (int c = 0; c < 3; ++c) {
        p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
        p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
    }
    const bool punchthrough = c0 <= c1;
    for (int i = 0; i < 16; ++i) {
        const int idx = (indices >> (2 * i)) & 3;
        out[i][0] = p[idx][0];
        out[i][1] = p[idx][1];
        out[i][2] = p[idx][2];
        out[i][3] = (punchthrough && idx == 3) ? 0 : 255;
    }
}

// The sixteenth 3-bit index ends at bit 47, crossing byte after byte, so a read is assembled from two bytes.
int readBC4Index(const uint8_t* indices, int i) {
    const int bit = 3 * i;
    uint16_t two;
    std::memcpy(&two, indices + (bit >> 3), 2);
    return (two >> (bit & 7)) & 7;
}

uint8_t decodePixelBC4(const uint8_t* in, int index3bit) {
    const int a0 = in[0], a1 = in[1];
    int p[8];
    p[0] = a0;
    p[1] = a1;
    if (a0 > a1) {
        for (int s = 1; s <= 6; ++s) p[1 + s] = ((7 - s) * a0 + s * a1) / 7;
    } else {
        // The six-value mode: four interpolated steps of (5-s)/5, then two hard zeros (spec).
        for (int s = 1; s <= 4; ++s) p[1 + s] = ((5 - s) * a0 + s * a1) / 5;
        p[6] = 0;
        p[7] = 255;
    }
    return static_cast<uint8_t>(p[index3bit & 7]);
}

} // namespace

void decodeBC1(const uint8_t* blocks, int w, int h, uint8_t* rgbaOut) {
    for (int by = 0; by < blocksHigh(h); ++by)
        for (int bx = 0; bx < blocksWide(w); ++bx) {
            int px[16][4];
            decodeBlockBC1(blocks + (static_cast<size_t>(by) * blocksWide(w) + bx) * 8, px);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const int cy = by * 4 + y, cx = bx * 4 + x;
                    if (cy >= h || cx >= w) continue;
                    uint8_t* dst = rgbaOut + (static_cast<size_t>(cy) * w + cx) * 4;
                    const int* src = px[y * 4 + x];
                    dst[0] = static_cast<uint8_t>(src[0]);
                    dst[1] = static_cast<uint8_t>(src[1]);
                    dst[2] = static_cast<uint8_t>(src[2]);
                    dst[3] = static_cast<uint8_t>(src[3]);
                }
        }
}

void decodeBC3(const uint8_t* blocks, int w, int h, uint8_t* rgbaOut) {
    for (int by = 0; by < blocksHigh(h); ++by)
        for (int bx = 0; bx < blocksWide(w); ++bx) {
            const uint8_t* block = blocks + (static_cast<size_t>(by) * blocksWide(w) + bx) * 16;
            int px[16][4];
            decodeBlockBC1(block + 8, px);
            for (int i = 0; i < 16; ++i) {
                const int idx = readBC4Index(block + 2, i);
                px[i][3] = decodePixelBC4(block, idx);
            }
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const int cy = by * 4 + y, cx = bx * 4 + x;
                    if (cy >= h || cx >= w) continue;
                    uint8_t* dst = rgbaOut + (static_cast<size_t>(cy) * w + cx) * 4;
                    const int* src = px[y * 4 + x];   // four ints; copied per byte or only red would land
                    dst[0] = static_cast<uint8_t>(src[0]);
                    dst[1] = static_cast<uint8_t>(src[1]);
                    dst[2] = static_cast<uint8_t>(src[2]);
                    dst[3] = static_cast<uint8_t>(src[3]);
                }
        }
}

void decodeBC5(const uint8_t* blocks, int w, int h, uint8_t* rgbaOut) {
    for (int by = 0; by < blocksHigh(h); ++by)
        for (int bx = 0; bx < blocksWide(w); ++bx) {
            const uint8_t* block = blocks + (static_cast<size_t>(by) * blocksWide(w) + bx) * 16;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const int cy = by * 4 + y, cx = bx * 4 + x;
                    if (cy >= h || cx >= w) continue;
                    const int i = y * 4 + x;
                    const int rIdx = readBC4Index(block + 2, i);
                    const int gIdx = readBC4Index(block + 10, i);
                    uint8_t* dst = rgbaOut + (static_cast<size_t>(cy) * w + cx) * 4;
                    dst[0] = decodePixelBC4(block, rIdx);
                    dst[1] = decodePixelBC4(block + 8, gIdx);
                    dst[2] = 0;
                    dst[3] = 255;
                }
        }
}

void decodeASTC(const uint8_t* blocks, int w, int h, bool srgb, uint8_t* rgbaOut) {
    astcenc_config config{};
    astcenc_error err = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1,
                                            ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (err != ASTCENC_SUCCESS) throw std::runtime_error("astcenc config (decode)");
    AstcContext holder;
    err = astcenc_context_alloc(&config, 1, &holder.ctx, nullptr);
    if (err != ASTCENC_SUCCESS) throw std::runtime_error("astcenc context (decode)");
    void* slice = rgbaOut;
    astcenc_image image{static_cast<unsigned int>(w), static_cast<unsigned int>(h), 1u, ASTCENC_TYPE_U8, &slice};
    const astcenc_swizzle swz = identitySwizzle();
    err = astcenc_decompress_image(holder.ctx, blocks, blocksWide(w) * blocksHigh(h) * 16, &image, &swz, 0);
    if (err != ASTCENC_SUCCESS) throw std::runtime_error(std::string("astcenc decompress: ") +
                                                         astcenc_get_error_string(err));
}

// ------------------------------------------------------------------ chains

Chain compressChain(const std::vector<std::vector<uint8_t>>& rgbaLevels, int w, int h, rhi::Format f) {
    Chain out;
    if (!compressed(f)) return out;
    out.format = f;
    out.width = w;
    out.height = h;
    out.levels.reserve(rgbaLevels.size());
    for (size_t l = 0; l < rgbaLevels.size(); ++l) {
        const int lw = std::max(1, w >> static_cast<int>(l));
        const int lh = std::max(1, h >> static_cast<int>(l));
        std::vector<uint8_t> encoded;
        switch (f) {
        case rhi::Format::BC1:
        case rhi::Format::BC1_SRGB: encoded = encodeBC1(rgbaLevels[l].data(), lw, lh); break;
        case rhi::Format::BC3:
        case rhi::Format::BC3_SRGB: encoded = encodeBC3(rgbaLevels[l].data(), lw, lh); break;
        case rhi::Format::ASTC_4x4:
        case rhi::Format::ASTC_4x4_SRGB:
            encoded = encodeASTC(rgbaLevels[l].data(), lw, lh, f == rhi::Format::ASTC_4x4_SRGB);
            break;
        default: encoded = encodeBC5(rgbaLevels[l].data(), lw, lh); break;
        }
        out.levels.push_back(std::move(encoded));
    }
    return out;
}

bool hasAlpha(const uint8_t* rgba, int w, int h) {
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i)
        if (rgba[i * 4 + 3] != 255) return true;
    return false;
}

std::vector<std::vector<uint8_t>> rgbaMipChain(const uint8_t* rgba, int w, int h) {
    int levels = 0;
    while ((std::max(w, h) >> levels) > 0) ++levels;
    std::vector<std::vector<uint8_t>> chain;
    chain.reserve(static_cast<size_t>(levels));
    std::vector<uint8_t> current(rgba, rgba + static_cast<size_t>(w) * h * 4);
    int cw = w, ch = h;
    for (int l = 0; l < levels; ++l) {
        chain.push_back(current);
        if (cw == 1 && ch == 1) break;
        const int nw = std::max(1, cw / 2), nh = std::max(1, ch / 2);
        std::vector<uint8_t> down(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0, count = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sy = y * 2 + dy, sx = x * 2 + dx;
                            if (sy >= ch || sx >= cw) continue;
                            sum += current[(static_cast<size_t>(sy) * cw + sx) * 4 + c];
                            ++count;
                        }
                    down[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>(sum / std::max(count, 1));
                }
            }
        current = std::move(down);
        cw = nw;
        ch = nh;
    }
    return chain;
}

} // namespace ukiyo::texc
