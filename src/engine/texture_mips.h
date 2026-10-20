// Alpha-coverage-preserving mipmaps (Godot 4.8's GH-104289).
//
// A mip chain averages alpha the same way it averages colour, and alpha-tested materials read that as thinning coverage:
// a fence, a leaf card or a hair card keeps its holes only while the mip is sharp, and at distance the averaged alpha
// sinks below the material's cutoff and the geometry dissolves. Godot's answer is to scale each level's alpha after the
// box filter so the fraction of the surface that survives the alpha test stays what it was at level 0, and this header is
// the same arithmetic on the CPU: measure the coverage, binary-search an alpha multiplier in [0, 4], apply it.
//
// The coverage is measured the way a bilinearly filtered sample at the level's own scale sees it — each texel's
// footprint is sampled 4 x 4 with bilinear interpolation and a sample counts when its alpha is above the threshold —
// rather than as a count of texel centres, because the mip is never sampled at its centres once it is minified.
#pragma once

#include <cstdint>
#include <vector>

namespace ukiyo {

// One level of an RGBA8 chain. Level 0 is the source image.
struct MipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   // 4 bytes per texel, row-major, top row first
};

// Fraction of the surface (0..1) that survives `alpha * alphaScale > threshold`.
float alphaTestCoverage(const uint8_t* rgba, uint32_t width, uint32_t height, float threshold, float alphaScale = 1.0f);

// The multiplier in [0, 4] that brings this level's coverage closest to `desired`. Ten-step binary search, the same
// search Godot's Image::_scale_alpha_to_coverage runs. Returns 1 when the level already matches within 2e-3.
float alphaScaleForCoverage(const uint8_t* rgba, uint32_t width, uint32_t height, float threshold, float desired);

// Box-filtered chain, levels 0 .. floor(log2(max(width, height))). Level 0 is copied unchanged (the caller's image is
// never modified). When `preserveAlphaTestCoverage` is set, every level after 0 has its alpha scaled to hold level 0's
// coverage; RGB is never touched, so the colour of a surviving texel is the colour it always had.
std::vector<MipLevel> buildMipChain(const uint8_t* rgba, uint32_t width, uint32_t height,
                                    bool preserveAlphaTestCoverage = false, float alphaTestThreshold = 0.5f);

} // namespace ukiyo
