// Layer weights and layer textures for a terrain tile (item 18, steps 2 and 2b). Pure CPU code, so the tests run without
// a device. The GPU uploads the weights per vertex, and the atlases as two textures, and the terrain shader blends them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/terrain.h"

namespace ukiyo {

// Slope in degrees at sample (x, z), from central differences of the scaled heights. Clamped at the edges.
float slopeDegAt(const Heightmap& map, int x, int z);

// Blend weights of the layers at every sample: kTerrainMaxLayers floats per sample, sample-major
// (index = sample * kTerrainMaxLayers + layer). `paint` holds one byte per sample per layer in the same layout, where
// 255 is full paint. An empty `paint` means full paint everywhere.
//
// A layer's weight is its height band times its slope band times its paint. The weights are normalised to sum to one.
// Where no layer matches, layer 0 takes the whole sample, so every sample still sums to one. Layers beyond the first
// kTerrainMaxLayers are ignored. Returns an empty vector when there are no layers, or when `paint` has the wrong size.
std::vector<float> layerWeightMap(const Heightmap& map, const std::vector<TerrainLayer>& layers,
                                  const std::vector<uint8_t>& paint);

// The weights at one sample (sample = z * size + x), written as kTerrainMaxLayers floats. Same rule as
// layerWeightMap, which is this function applied to every sample. A paint map of the wrong size is ignored here: the
// caller is expected to have validated it. Used by the grass scatter, which needs the weights only where a blade
// lands and must not build the whole map for a large tile.
void layerWeightsAtSample(const Heightmap& map, const std::vector<TerrainLayer>& layers,
                          const std::vector<uint8_t>& paint, size_t sample, float* out);

// Layer textures are packed into two atlases. Each layer owns one cell: layer k is at column k % kTerrainAtlasCols and
// row k / kTerrainAtlasCols. A cell is kTerrainAtlasCell texels square, and the atlas is always full size, so unused
// cells hold white diffuse, flat normals and no occlusion.
//
// 512 is the production size: a layer tiles every few metres, and at 256 texels a cell the ground under the camera
// shows individual texels as flat patches. The vendored reference textures are 512 square, so this is also the size at
// which they arrive with no resample at all.
constexpr uint32_t kTerrainAtlasCols = 4;
constexpr uint32_t kTerrainAtlasRows = 2;
constexpr uint32_t kTerrainAtlasCell = 512;

struct TerrainAtlases {
    uint32_t width = kTerrainAtlasCols * kTerrainAtlasCell;    // 2048
    uint32_t height = kTerrainAtlasRows * kTerrainAtlasCell;   // 1024
    std::vector<uint8_t> diffuse;   // RGBA8: rgb albedo, a roughness
    std::vector<uint8_t> normal;    // RGBA8: rgb tangent-space normal (128, 128, 255 is flat), a occlusion
};

// Decodes an image file to RGBA8. Returns false with *error set when the file cannot be read.
bool loadRgba8(const std::string& path, uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgba, std::string* error);

// Packs the textures of the first kTerrainMaxLayers layers into the atlases. A missing file for a layer is not an error:
// that map is the default. A file that is named but cannot be read is an error. The roughness is the alpha of the
// diffuse atlas: from the red channel of the roughness file, or the layer's constant. The occlusion is the alpha of the
// normal atlas: from the red channel of the occlusion file, or white (no occlusion).
bool buildTerrainAtlases(const std::vector<TerrainLayer>& layers, TerrainAtlases* out, std::string* error);

} // namespace ukiyo
