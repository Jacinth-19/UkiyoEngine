// Block-compressed textures, CPU side (batch 7): the encoders, decoders and mip-chain packing that feed the RHI's
// compressed formats. The GPU side is a format — these are the bytes.
//
// The engine ships its own BCn codecs rather than a vendored encoder, for the same reason it ships its own toon
// shader rather than a vendored one: the whole point of the batch is that the bytes are ours to measure. The
// algorithms are the standard ones (a bounding-box colour palette with 565 endpoints and two interpolated midpoints;
// an eight-value alpha palette with 3-bit indices), implemented from the Khronos block-compression specification,
// with our own endpoint refinement. Quality against the reference formulas is a test, not a hope: the test decodes
// every block back on the CPU and bounds the error.
//
// What each format is for here:
//   * BC1  — opaque colour, 4 bits per pixel (8 bytes per 4x4 block). Model base and emissive textures.
//   * BC3  — colour plus a full 8-bit alpha path, 8 bits per pixel (16 bytes per block). Cut-out materials keep
//           their alpha test; the alpha-coverage-preserving chain runs first and every level is compressed after.
//   * BC5  — two independent channels, 8 bits per pixel. Normal maps (the blue is reconstructed in the shader).
//
// ASTC for the phone lands in the second half of the batch; the plumbing (formats, upload paths, block maths) is
// written so ASTC is another entry in the same tables, not a second system.
//
// Everything is pure CPU maths with no device behind it, so the codecs and their error bounds are tested on every
// machine CI has — see tests/texture_compress_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rhi/rhi.h"   // Format: BC1, BC1_SRGB, BC3, BC3_SRGB, BC5 live next to the formats they become

namespace ukiyo::texc {

// True when the format is one of the block-compressed ones the encoders below produce.
bool compressed(rhi::Format f);

// The compressed texel block is 4x4 for every format here.
constexpr int kBlockWidth = 4;
constexpr int kBlockHeight = 4;

// Bytes in one 4x4 block: 8 for BC1, 16 for BC3 and BC5.
int blockBytes(rhi::Format f);

// The number of blocks covering an image of w x h texels — a partial edge block is a whole block, which is why a
// 1x1 tail mip still costs a full block.
int blocksWide(int w);
int blocksHigh(int h);

// Exact byte size of a whole mip level of w x h texels in `f`.
size_t levelBytes(rhi::Format f, int w, int h);

// Encodes one RGBA8 image (tightly packed, row 0 at the top) into tightly packed blocks. Alpha is ignored by BC1 —
// pick BC3 when the texture has alpha, which the model loader does. w and h need not be multiples of four; edge
// texels beyond the image are clamped copies of the edge.
std::vector<uint8_t> encodeBC1(const uint8_t* rgba, int w, int h);
std::vector<uint8_t> encodeBC3(const uint8_t* rgba, int w, int h);
std::vector<uint8_t> encodeBC5(const uint8_t* rgba, int w, int h);

// Decodes one compressed image back to RGBA8 (alpha 255 for BC1 and BC5's unused channels). The tests decode
// everything they encode; the error bounds in them are measured against this.
void decodeBC1(const uint8_t* blocks, int w, int h, uint8_t* rgbaOut);
void decodeBC3(const uint8_t* blocks, int w, int h, uint8_t* rgbaOut);
void decodeBC5(const uint8_t* blocks, int w, int h, uint8_t* rgbaOut);

// A whole mip chain: the RGBA8 levels the engine's CPU chain builder produces, one compressed level out per level in,
// smallest last. `f` picks the codec; the sRGB flag only travels with the format — the block bytes are identical.
struct Chain {
    rhi::Format format = rhi::Format::RGBA8;
    int width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;   // level 0 first, as the RHI's MipChain is
};

// Compresses the chain. `rgbaLevels` must carry every level (level 0 is w x h, then halving to 1) in RGBA8.
Chain compressChain(const std::vector<std::vector<uint8_t>>& rgbaLevels, int w, int h, rhi::Format f);

// A utility for the loader: does level 0 use alpha anywhere? Opaque textures go to BC1 at half the memory of BC3.
bool hasAlpha(const uint8_t* rgba, int w, int h);

// A plain box-filtered mip chain for a texture the coverage-preserving builder did not build (models with no cutout
// still want compressed chains). Matches the level count the RHI allocates for the same size.
std::vector<std::vector<uint8_t>> rgbaMipChain(const uint8_t* rgba, int w, int h);

} // namespace ukiyo::texc
