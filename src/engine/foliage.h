// Foliage scatter for a terrain tile (item 18, step 3). Pure CPU code: the placements are deterministic for a seed, and
// the GPU side (terrain_gpu) uploads them in batches. The renderer draws each batch as one instanced call.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain.h"

namespace ukiyo {

// Instances per draw. Each takes 3 vec4 rows (48 bytes), so one batch is 12 KB, inside the 16 KB minimum uniform size.
constexpr uint32_t kFoliageBatch = 256;

// One placed instance. The position is in tile space, in metres from the heightmap origin.
struct FoliageInstance {
    glm::vec3 pos{0.0f};
    float yaw = 0.0f;     // radians, about the vertical axis
    float scale = 1.0f;
};

// Reads an image and resamples its red channel to size * size bytes (nearest). 255 means full density. Returns false
// with *error set when the file cannot be read.
bool loadDensityField(const std::string& path, uint32_t size, std::vector<uint8_t>* out, std::string* error);

// Places instances of one type on the heightmap. Each cell of the grid has one candidate, with fixed random draws from
// the seed. A candidate is kept when its draw is below density times the density field, and when its height and slope
// are inside the type's limits. Since the draws do not depend on density, a lower density keeps a subset of a higher
// one's instances. An empty field means full density everywhere. The result is deterministic.
std::vector<FoliageInstance> scatterFoliage(const Heightmap& map, const FoliageType& type,
                                            const std::vector<uint8_t>& densityField);

// Groups instances by the chunk that holds them. The result has chunksPerSide * chunksPerSide lists, row major like
// the chunks of the terrain. Instances on the far edge go into the last chunk.
std::vector<std::vector<FoliageInstance>> groupByChunk(const std::vector<FoliageInstance>& instances, float cellSize,
                                                       uint32_t chunkCells, uint32_t chunksPerSide);

// Unit-scale geometry of a shape, in metres: the tuft is 1 m tall and the cone 2 m. Normals point roughly outward.
struct FoliageMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<uint32_t> indices;   // triangles, double-sided in drawing
};
FoliageMesh buildFoliageMesh(FoliageShape shape);

// Writes the batch rows for `count` instances (count <= kFoliageBatch). Each instance takes three rows of the 3 x 4
// affine transform: row r is (m[0][r], m[1][r], m[2][r], m[3][r]). The translation is the w of each row. Rows past
// `count` are zeroed.
void packFoliageRows(const FoliageInstance* instances, uint32_t count, glm::vec4* rows);

} // namespace ukiyo
