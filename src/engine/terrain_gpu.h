// Uploads the chunks of a heightmap to the GPU (item 18, steps 1 and 2). The chunk positions stay in tile space; the
// tile's instance transform places them.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain.h"
#include "rhi/rhi.h"

namespace ukiyo {

struct TerrainChunkGpu {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    uint32_t indexCount = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// The layer data the terrain shader reads, in the ShaderMaterial set (step 2). The uniform buffer is at binding 15:
// per layer, `layer` holds rgb tint (the diffuse atlas is multiplied by it), and `tile` holds the metres per repeat in
// x. The textures are at bindings 0 (diffuse atlas, roughness in alpha) and 1 (normal atlas).
struct TerrainLayerBlock {
    glm::vec4 layer[kTerrainMaxLayers];
    glm::vec4 tile[kTerrainMaxLayers];
};

// One foliage type's mesh (step 3). Shared by all of the type's batches.
struct FoliageMeshGpu {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    uint32_t indexCount = 0;
};

// Up to kFoliageBatch instances of one type, all in one chunk. The set holds the instance rows at binding 15. The
// bounds are the chunk's, in tile space, grown upward so that the tops of the foliage are inside them.
struct FoliageBatchGpu {
    std::unique_ptr<rhi::Buffer> rows;      // declared before the set, so it outlives it
    std::unique_ptr<rhi::DescriptorSet> set;
    uint32_t count = 0;
    uint32_t type = 0;                      // index into TerrainGpu::foliageMeshes
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    glm::vec2 fade{60.0f, 90.0f};           // metres from the camera: full size inside x, gone past y
    glm::vec3 tint{0.3f, 0.5f, 0.2f};
};

// One uploaded tile. The heightmap stays on the CPU for colliders and height queries.
//
// Each vertex carries its layer weights. Layers 0 to 3 are in `weights` (floats). Layers 4 to 7 are in the `joints`
// slots as u16 values scaled by 65535, because the vertex has no spare float field. The terrain pipeline does not
// skin, so those slots are free.
struct TerrainGpu {
    Heightmap map;
    std::vector<TerrainChunkGpu> chunks;   // row major: chunk (cx, cz) is at cz * n + cx
    // Declared in dependency order, so the destructors run in reverse: the set first, then the textures and buffer.
    std::unique_ptr<rhi::Buffer> layerBuffer;
    std::unique_ptr<rhi::Texture> diffuseAtlas;
    std::unique_ptr<rhi::Texture> normalAtlas;
    std::unique_ptr<rhi::DescriptorSet> layerSet;
    // Foliage (step 3). Filled by uploadFoliage, empty when the tile has none.
    std::vector<FoliageMeshGpu> foliageMeshes;   // one per type
    std::vector<FoliageBatchGpu> foliage;
};

// Builds and uploads every chunk of `map`, with the blend weights and the layer textures of `layers`. `paint` is as in
// layerWeightMap. Returns false with *error set when the grid does not divide into chunks of `cells`, the layers are
// empty, or a layer's image file cannot be read.
bool uploadTerrain(rhi::Device& device, const Heightmap& map, uint32_t cells, const std::vector<TerrainLayer>& layers,
                   const std::vector<uint8_t>& paint, TerrainGpu* out, std::string* error = nullptr);

// Scatters the foliage of `types` over the tile in `gpu` (from uploadTerrain) and uploads it in batches. `densities[t]`
// is the density field of type t, or empty for uniform. Returns false with *error set when there are more than
// kFoliageMaxTypes types or a density field has the wrong size. On failure the foliage of `gpu` is cleared.
bool uploadFoliage(rhi::Device& device, const std::vector<FoliageType>& types,
                   const std::vector<std::vector<uint8_t>>& densities, TerrainGpu* gpu, std::string* error = nullptr);

} // namespace ukiyo
