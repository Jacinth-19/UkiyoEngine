#include "engine/texture_mips.h"

#include <algorithm>
#include <cmath>

namespace ukiyo {

namespace {

inline const uint8_t* texelAt(const uint8_t* rgba, uint32_t w, uint32_t h, int x, int y) {
    x = std::clamp(x, 0, int(w) - 1);
    y = std::clamp(y, 0, int(h) - 1);
    return rgba + (size_t(y) * w + size_t(x)) * 4u;
}

// One bilinear alpha sample at (fx, fy) inside the footprint of texel (x, y); (0, 0) is the texel's own corner.
inline float bilinearAlpha(const uint8_t* rgba, uint32_t w, uint32_t h, int x, int y, float fx, float fy) {
    const float a00 = texelAt(rgba, w, h, x, y)[3] / 255.0f;
    const float a10 = texelAt(rgba, w, h, x + 1, y)[3] / 255.0f;
    const float a01 = texelAt(rgba, w, h, x, y + 1)[3] / 255.0f;
    const float a11 = texelAt(rgba, w, h, x + 1, y + 1)[3] / 255.0f;
    return a00 * (1.0f - fx) * (1.0f - fy) + a10 * fx * (1.0f - fy) + a01 * (1.0f - fx) * fy + a11 * fx * fy;
}

} // namespace

float alphaTestCoverage(const uint8_t* rgba, uint32_t width, uint32_t height, float threshold, float alphaScale) {
    if (!rgba || width == 0 || height == 0) return 0.0f;
    constexpr int kSub = 4;
    double covered = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            int hits = 0;
            for (int sy = 0; sy < kSub; ++sy) {
                const float fy = (sy + 0.5f) / kSub;
                for (int sx = 0; sx < kSub; ++sx) {
                    const float fx = (sx + 0.5f) / kSub;
                    const float a = std::min(bilinearAlpha(rgba, width, height, int(x), int(y), fx, fy) * alphaScale, 1.0f);
                    if (a > threshold) ++hits;
                }
            }
            covered += double(hits) / double(kSub * kSub);
        }
    }
    return float(covered / (double(width) * double(height)));
}

float alphaScaleForCoverage(const uint8_t* rgba, uint32_t width, uint32_t height, float threshold, float desired) {
    // Fast path: most masked textures are opaque over most of their area, and level 1 already matches.
    if (std::fabs(alphaTestCoverage(rgba, width, height, threshold, 1.0f) - desired) <= 2e-3f) return 1.0f;

    float lo = 0.0f, hi = 4.0f;
    float scale = 1.0f;
    float bestScale = 1.0f;
    float bestError = 1e9f;
    for (int i = 0; i < 10; ++i) {
        const float coverage = alphaTestCoverage(rgba, width, height, threshold, scale);
        const float error = std::fabs(coverage - desired);
        if (error < bestError) {
            bestError = error;
            bestScale = scale;
        }
        if (error <= 2e-3f) break;
        if (coverage < desired) {
            lo = scale;
        } else {
            hi = scale;
        }
        scale = (lo + hi) * 0.5f;
    }
    return bestScale;
}

std::vector<MipLevel> buildMipChain(const uint8_t* rgba, uint32_t width, uint32_t height,
                                    bool preserveAlphaTestCoverage, float alphaTestThreshold) {
    std::vector<MipLevel> chain;
    if (!rgba || width == 0 || height == 0) return chain;

    uint32_t levels = 1;
    for (uint32_t m = std::max(width, height); m > 1; m >>= 1) ++levels;

    chain.resize(levels);
    chain[0].width = width;
    chain[0].height = height;
    chain[0].rgba.assign(rgba, rgba + size_t(width) * height * 4u);

    const float desired =
        preserveAlphaTestCoverage ? alphaTestCoverage(rgba, width, height, alphaTestThreshold) : 0.0f;

    for (uint32_t i = 1; i < levels; ++i) {
        const MipLevel& src = chain[i - 1];
        MipLevel& dst = chain[i];
        dst.width = std::max(1u, src.width / 2u);
        dst.height = std::max(1u, src.height / 2u);
        dst.rgba.assign(size_t(dst.width) * dst.height * 4u, 0);

        // 2 x 2 box filter with rounding, the average a driver's blit produces. Odd sizes read the last column or row
        // twice, which is what a clamped bilinear fetch of that footprint would do.
        for (uint32_t y = 0; y < dst.height; ++y) {
            const uint32_t y0 = std::min(y * 2u, src.height - 1u);
            const uint32_t y1 = std::min(y * 2u + 1u, src.height - 1u);
            for (uint32_t x = 0; x < dst.width; ++x) {
                const uint32_t x0 = std::min(x * 2u, src.width - 1u);
                const uint32_t x1 = std::min(x * 2u + 1u, src.width - 1u);
                for (int c = 0; c < 4; ++c) {
                    const unsigned sum = unsigned(src.rgba[(size_t(y0) * src.width + x0) * 4u + c]) +
                                         unsigned(src.rgba[(size_t(y0) * src.width + x1) * 4u + c]) +
                                         unsigned(src.rgba[(size_t(y1) * src.width + x0) * 4u + c]) +
                                         unsigned(src.rgba[(size_t(y1) * src.width + x1) * 4u + c]);
                    dst.rgba[(size_t(y) * dst.width + x) * 4u + c] = uint8_t((sum + 2u) / 4u);
                }
            }
        }

        if (preserveAlphaTestCoverage) {
            const float scale = alphaScaleForCoverage(dst.rgba.data(), dst.width, dst.height, alphaTestThreshold, desired);
            if (scale != 1.0f) {
                for (size_t t = 0; t < dst.rgba.size(); t += 4) {
                    const float a = float(dst.rgba[t + 3]) * scale;
                    dst.rgba[t + 3] = uint8_t(std::clamp(a, 0.0f, 255.0f) + 0.5f);
                }
            }
        }
    }
    return chain;
}

} // namespace ukiyo
