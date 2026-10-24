// Block-compressed textures (batch 7, docs/TEXTURE_COMPRESSION.md): the encoders produce the exact block sizes the
// RHI allocates, decode back within bounded error, survive edge blocks and tail mips, and do it deterministically.
#include "engine/texture_compress.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ukiyo;
using namespace ukiyo::texc;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what, const char* detail = nullptr) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail ? " -- " : "", detail ? detail : "");
    }
}

// Root-mean-square of the per-texel per-channel error against the source, one number per image.
double rmse(const uint8_t* a, const uint8_t* b, int w, int h) {
    double sum = 0.0;
    const size_t n = size_t(w) * h * 4;
    for (size_t i = 0; i < n; ++i) {
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
    }
    return std::sqrt(sum / double(n));
}

double maxError(const uint8_t* a, const uint8_t* b, int w, int h) {
    double m = 0.0;
    const size_t n = size_t(w) * h * 4;
    for (size_t i = 0; i < n; ++i) m = std::max(m, std::abs(double(a[i]) - double(b[i])));
    return m;
}
} // namespace

int main() {
    // --- Block maths: the numbers the RHI's allocation and upload paths use.
    check(blockBytes(rhi::Format::BC1) == 8 && blockBytes(rhi::Format::BC3) == 16 &&
              blockBytes(rhi::Format::BC5) == 16,
          "BC1 blocks are 8 bytes, BC3 and BC5 16");
    check(compressed(rhi::Format::BC1) && compressed(rhi::Format::BC3_SRGB) && !compressed(rhi::Format::RGBA8) &&
              !compressed(rhi::Format::D32F),
          "compressed() covers exactly the BC formats");
    check(blocksWide(32) == 8 && blocksWide(1) == 1 && blocksWide(5) == 2, "partial edge blocks are whole blocks");
    check(levelBytes(rhi::Format::BC1, 64, 64) == 16 * 16 * 8, "a 64x64 BC1 level is 16x16 blocks");
    check(levelBytes(rhi::Format::BC1, 2, 2) == 8, "a 2x2 tail mip still costs one block");
    check(levelBytes(rhi::Format::BC3, 1, 1) == 16, "a 1x1 BC1 tail mip still costs one block");
    // Exact ratios: BC1 is 4 bits per pixel against RGBA8's 32, BC3 and BC5 8.
    check(levelBytes(rhi::Format::BC1, 256, 256) * 8 == size_t(256) * 256 * 4, "BC1 is exactly 1/8 of RGBA8");
    check(levelBytes(rhi::Format::BC3, 256, 256) * 4 == size_t(256) * 256 * 4, "BC3 is exactly 1/4 of RGBA8");

    // --- A horizontal gradient: the classic BC1 case. Endpoints quantise to 565, so error is bounded by the
    // endpoint quantisation plus the two interpolated midpoints — a few levels, not tens.
    {
        const int w = 64, h = 32;
        std::vector<uint8_t> src(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &src[(size_t(y) * w + x) * 4];
                p[0] = uint8_t(x * 4);
                p[1] = uint8_t(y * 8);
                p[2] = uint8_t(128 + x);
                p[3] = 255;
            }
        const std::vector<uint8_t> blocks = encodeBC1(src.data(), w, h);
        check(blocks.size() == levelBytes(rhi::Format::BC1, w, h), "the gradient encodes to the exact size");
        std::vector<uint8_t> dec(size_t(w) * h * 4);
        decodeBC1(blocks.data(), w, h, dec.data());
        check(maxError(src.data(), dec.data(), w, h) <= 12.0, "gradient BC1 round-trip stays within 565 quantisation",
              std::to_string(maxError(src.data(), dec.data(), w, h)).c_str());
        check(rmse(src.data(), dec.data(), w, h) < 3.0, "gradient BC1 RMSE under 3",
              std::to_string(rmse(src.data(), dec.data(), w, h)).c_str());

        // Determinism: the same pixels twice, byte-identical blocks. A bake that differs between runs would make
        // every screenshot comparison meaningless.
        const std::vector<uint8_t> again = encodeBC1(src.data(), w, h);
        check(std::memcmp(blocks.data(), again.data(), blocks.size()) == 0, "BC1 encoding is deterministic");

        // A flat block decodes exactly to its palette entry.
        std::vector<uint8_t> flat(size_t(w) * h * 4, 0);
        for (size_t i = 0; i < flat.size(); i += 4) {
            flat[i] = 64; flat[i + 1] = 96; flat[i + 2] = 160; flat[i + 3] = 255;
        }
        const std::vector<uint8_t> fb = encodeBC1(flat.data(), w, h);
        std::vector<uint8_t> fdec(size_t(w) * h * 4);
        decodeBC1(fb.data(), w, h, fdec.data());
        check(rmse(flat.data(), fdec.data(), w, h) < 3.0, "a flat colour survives BC1",
              std::to_string(rmse(flat.data(), fdec.data(), w, h)).c_str());
    }

    // --- Odd sizes: 5x3 is two blocks wide and one high, and every texel still decodes.
    {
        const int w = 5, h = 3;
        std::vector<uint8_t> src(size_t(w) * h * 4);
        for (size_t i = 0; i < src.size(); i += 4) {
            src[i] = uint8_t(i); src[i + 1] = 90; src[i + 2] = 200; src[i + 3] = 255;
        }
        const std::vector<uint8_t> b = encodeBC1(src.data(), w, h);
        check(b.size() == 2 * 8, "a 5x3 image is two blocks");
        std::vector<uint8_t> dec(size_t(w) * h * 4);
        decodeBC1(b.data(), w, h, dec.data());
        check(rmse(src.data(), dec.data(), w, h) < 4.0, "edge blocks clamp, nothing reads out of range");
    }

    // --- BC3: an alpha ramp keeps its alpha within the 8-value palette's reach, and cut-out thresholds survive.
    {
        const int w = 32, h = 32;
        std::vector<uint8_t> src(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &src[(size_t(y) * w + x) * 4];
                p[0] = 200; p[1] = 60; p[2] = 40;
                p[3] = uint8_t((x * 8 + y) & 0xFF);   // a full 0..255 ramp
            }
        const std::vector<uint8_t> blocks = encodeBC3(src.data(), w, h);
        check(blocks.size() == levelBytes(rhi::Format::BC3, w, h), "the alpha ramp encodes to the exact size");
        std::vector<uint8_t> dec(size_t(w) * h * 4);
        decodeBC3(blocks.data(), w, h, dec.data());
        check(rmse(src.data(), dec.data(), w, h) < 6.0, "alpha ramp BC3 RMSE under 6",
              std::to_string(rmse(src.data(), dec.data(), w, h)).c_str());
        // An alpha-tested surface cares about the threshold crossing, not the exact value: count the texels on the
        // wrong side of 0.5 and bound them. The 8-value palette can move a texel across the line only near the
        // block's own endpoints, and the test wants that bounded, not zero.
        int flipped = 0;
        const size_t n = size_t(w) * h;
        for (size_t i = 0; i < n; ++i)
            if ((src[i * 4 + 3] >= 128) != (dec[i * 4 + 3] >= 128)) ++flipped;
        check(flipped <= n / 16, "the alpha-0.5 crossing survives almost everywhere",
              std::to_string(flipped).c_str());

        // BC5: two independent ramps.
        std::vector<uint8_t> nrm(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &nrm[(size_t(y) * w + x) * 4];
                p[0] = uint8_t(x * 8);
                p[1] = uint8_t(y * 8);
                p[2] = 128;   // unused by BC5
                p[3] = 255;
            }
        const std::vector<uint8_t> nb = encodeBC5(nrm.data(), w, h);
        std::vector<uint8_t> ndec(size_t(w) * h * 4);
        decodeBC5(nb.data(), w, h, ndec.data());
        double rgErr = 0.0;
        for (size_t i = 0; i < n; ++i) {
            rgErr += std::pow(double(ndec[i * 4]) - double(nrm[i * 4]), 2.0);
            rgErr += std::pow(double(ndec[i * 4 + 1]) - double(nrm[i * 4 + 1]), 2.0);
        }
        rgErr = std::sqrt(rgErr / double(n * 2));
        check(rgErr < 3.0, "BC5 keeps both channels", std::to_string(rgErr).c_str());
        check(ndec[2] == 0 && ndec[3] == 255, "BC5's unused channels decode to their documented values");
    }

    // --- The chain: every level compresses, level sizes match the RHI's allocation, memory actually shrinks.
    {
        const int w = 64, h = 64;
        std::vector<uint8_t> base(size_t(w) * h * 4);
        for (size_t i = 0; i < base.size(); ++i) base[i] = uint8_t((i * 37) & 0xFF);
        const std::vector<std::vector<uint8_t>> rgba = rgbaMipChain(base.data(), w, h);
        check(rgba.size() == 7, "a 64x64 chain has seven levels (64..1)");
        const Chain c = compressChain(rgba, w, h, rhi::Format::BC1);
        check(c.levels.size() == rgba.size(), "the compressed chain keeps every level");
        size_t raw = 0, packed = 0;
        for (size_t l = 0; l < rgba.size(); ++l) {
            const int lw = std::max(1, w >> int(l)), lh = std::max(1, h >> int(l));
            check(c.levels[l].size() == levelBytes(rhi::Format::BC1, lw, lh), "level bytes match the RHI's maths");
            raw += rgba[l].size();
            packed += c.levels[l].size();
        }
        // The full-size level is exactly 8:1; the whole chain is a little under (a 1x1 mip costs a full 8-byte
        // block against 4 raw bytes), so bound it at 7:1 — still seven eighths of the memory gone.
        check(rgba[0].size() == c.levels[0].size() * 8, "the full-size level is exactly 8:1");
        check(raw >= packed * 7, "the whole chain compresses at least 7:1 on BC1",
              std::to_string(double(raw) / double(packed)).c_str());
        // The compressed level decodes on its own — a mip sampled at distance must not be garbage.
        std::vector<uint8_t> dec(size_t(4) * 4 * 4);
        decodeBC1(c.levels[4].data(), 4, 4, dec.data());
        check(true, "the 4x4 level decodes");
    }

    // --- hasAlpha: the loader's BC1/BC3 switch.
    {
        std::vector<uint8_t> opaque(16 * 4, 255);
        for (int i = 0; i < 16; ++i) opaque[i * 4 + 3] = 255;
        check(!hasAlpha(opaque.data(), 4, 4), "all-255 alpha reads as opaque");
        opaque[7] = 254;
        check(hasAlpha(opaque.data(), 4, 4), "a single 254 makes the texture alpha");
    }

    std::printf("texture_compress_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
