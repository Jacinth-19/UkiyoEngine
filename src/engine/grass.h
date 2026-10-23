// Grass over terrain (item 19), adapted from MagicGrass (UnityGrassIndirectRenderingExample).
//
// The reference is a GPU-driven system: a compute shader scatters jittered candidates per terrain cell, samples a
// detail map and the layer splat masks, emits one 48-byte MeshData per blade, a second compute pass frustum- and
// Hi-Z-culls them into an append buffer, and a third pass draws the survivors with DrawMeshInstancedIndirect. Ukiyo's
// RHI now has all of that (storage buffers, compute dispatch, drawIndexedIndirect — batch 4, docs/GRASS.md §27),
// and this item still adapts on the CPU on purpose: the scatter and the layer masking run here once per tile, the
// distance bands, the wind and the fade run in the vertex shader, and the result is the same look and the same
// knobs with one draw call per 256 blades. Moving it onto the queue is now a grass project, not an RHI one.
//
// Everything in this file is pure CPU code with no device, so the tests can run it without a GPU.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain.h"

namespace ukiyo {

// Blades per draw, the same budget as foliage: 3 vec4 rows each (48 bytes), so one batch is 12 KB plus the colour
// block, inside the 16 KB minimum uniform block size.
constexpr uint32_t kGrassBatch = 256;

// One tile keeps at most this many tufts. A 256 m tile at the template's 4 tufts per square metre asks for exactly
// this many, so the meadow a new project opens with fits without the fallback stride. Past the budget the scatter thins
// by a fixed stride, the same subset every build, so the result stays deterministic. 1024 batches is 12 MB of instance
// rows. Eight blades of four quads is 64 triangles a tuft, so a whole tile held at once would be 16.8 M triangles; the
// distance bands are what keep a real view near 3 M, and a project can lower `density`, `blades` or `cullDistance` to
// trade it back.
constexpr uint32_t kGrassMaxInstances = 262144;

// The knobs themselves are GrassSettings, in terrain.h with the rest of a tile's settings: a TerrainSettings holds one
// by value. This header is only the work over it.

// One scattered blade tuft, in tile space.
struct GrassInstance {
    glm::vec3 pos{0.0f};          // the root, on the terrain
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float yaw = 0.0f;             // radians about `up`
    float scale = 1.0f;
};

// A tuft mesh: `blades` blades around the origin, each `segments` quads tall, tapering to a point. `uv.y` runs 0 at
// the root to 1 at the tip, which is what the vertex shader uses for the wind curve and the tint. Normals point
// sideways out of each blade, so the field shades as a mass rather than as flat cards.
struct GrassMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
};

GrassMesh buildGrassMesh(const GrassSettings& settings);

// The density factor at `distance` metres from the camera: 1 inside bandNear, falling smoothly to 0.5 at bandMid and
// to 0 at cullDistance. Monotone, so a closer view always keeps every blade a farther view keeps. The vertex shader
// applies the same curve, with a per-blade hash deciding which blades survive.
float grassDensityAt(float distance, const GrassSettings& settings);

// The per-blade draw in [0, 1). Hash of the blade's ground position; the shader computes the same value from the
// instance origin. Kept here so a test can apply the shader's rule on the CPU.
float grassBladeHash(glm::vec2 groundPos);

// Scatters blades over `map`. The layer tests read `layers` and `paint` the same way the terrain shader does (see
// layerWeightsAtSample), evaluated only where a candidate lands. `detailField` is the density map resampled to the
// heightmap, or empty for full density. Deterministic for a seed. Past kGrassMaxInstances the scatter keeps every
// n-th candidate, with n the smallest stride that fits the budget.
std::vector<GrassInstance> scatterGrass(const Heightmap& map, const std::vector<TerrainLayer>& layers,
                                        const std::vector<uint8_t>& paint, const GrassSettings& settings,
                                        const std::vector<uint8_t>& detailField);

// Groups blades by the chunk that holds them, row major like the terrain's chunks.
std::vector<std::vector<GrassInstance>> groupGrassByChunk(const std::vector<GrassInstance>& blades, float cellSize,
                                                          uint32_t chunkCells, uint32_t chunksPerSide);

// Writes the 3 rows per instance for `count` instances (count <= kGrassBatch). Row r is the r-th row of the
// translate * basis * scale matrix, so the shader rebuilds the blade's world position and its normal from them.
// Rows past `count` are zeroed.
void packGrassRows(const GrassInstance* instances, uint32_t count, glm::vec4* rows);

} // namespace ukiyo
