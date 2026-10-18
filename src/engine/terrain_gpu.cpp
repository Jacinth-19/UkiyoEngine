#include "engine/terrain_gpu.h"

#include <algorithm>
#include <cmath>

#include "engine/foliage.h"
#include "engine/terrain_layers.h"

namespace ukiyo {

bool uploadTerrain(rhi::Device& device, const Heightmap& map, uint32_t cells, const std::vector<TerrainLayer>& layers,
                   const std::vector<uint8_t>& paint, TerrainGpu* out, std::string* error) {
    if (!out) return false;
    const uint32_t n = chunksPerSide(map, cells);
    if (n == 0) {
        if (error) *error = "the grid does not divide into chunks";
        return false;
    }
    const std::vector<float> weights = layerWeightMap(map, layers, paint);
    if (weights.empty()) {
        if (error) *error = "the terrain needs at least one layer";
        return false;
    }
    TerrainAtlases atlases;
    if (!buildTerrainAtlases(layers, &atlases, error)) return false;

    TerrainGpu g;
    g.map = map;
    g.chunks.reserve(size_t(n) * n);
    for (uint32_t cz = 0; cz < n; ++cz) {
        for (uint32_t cx = 0; cx < n; ++cx) {
            const ChunkMesh m = buildChunk(map, cx, cz, cells);
            const uint32_t side = cells + 1;
            std::vector<rhi::Vertex> verts(m.positions.size());   // value-initialised
            for (uint32_t j = 0; j < side; ++j) {
                for (uint32_t i = 0; i < side; ++i) {
                    const size_t k = size_t(j) * side + i;
                    const size_t s = size_t(cz * cells + j) * map.size + (cx * cells + i);   // the sample it reads
                    const float* w = &weights[s * kTerrainMaxLayers];
                    rhi::Vertex& v = verts[k];
                    v.pos = m.positions[k];
                    v.normal = m.normals[k];
                    v.uv = m.uvs[k];
                    v.weights = glm::vec4(w[0], w[1], w[2], w[3]);
                    for (int q = 0; q < 4; ++q)
                        v.joints[q] = static_cast<uint16_t>(std::lround(std::clamp(w[4 + q], 0.0f, 1.0f) * 65535.0f));
                }
            }
            TerrainChunkGpu c;
            c.vertices = device.createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
            c.vertices->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
            c.indices = device.createBuffer(rhi::BufferUsage::Index, m.indices.size() * sizeof(uint32_t));
            c.indices->upload(m.indices.data(), m.indices.size() * sizeof(uint32_t));
            c.indexCount = static_cast<uint32_t>(m.indices.size());
            c.boundsMin = m.boundsMin;
            c.boundsMax = m.boundsMax;
            g.chunks.push_back(std::move(c));
        }
    }

    TerrainLayerBlock block{};
    for (size_t k = 0; k < kTerrainMaxLayers && k < layers.size(); ++k) {
        block.layer[k] = glm::vec4(layers[k].colour, 1.0f);
        block.tile[k] = glm::vec4(std::max(0.5f, layers[k].tileMetres), 0.0f, 0.0f, 0.0f);
    }
    for (size_t k = layers.size(); k < kTerrainMaxLayers; ++k) block.tile[k] = glm::vec4(8.0f, 0, 0, 0);
    g.layerBuffer = device.createBuffer(rhi::BufferUsage::Uniform, sizeof(TerrainLayerBlock));
    g.layerBuffer->upload(&block, sizeof(block));

    rhi::TextureDesc td;
    td.width = atlases.width;
    td.height = atlases.height;
    td.format = rhi::Format::RGBA8;
    td.mipmaps = false;   // a cell's mips would bleed into its neighbours
    td.filter = rhi::Filter::Linear;
    td.wrap = rhi::Wrap::Clamp;
    g.diffuseAtlas = device.createTexture(td, atlases.diffuse.data());
    g.normalAtlas = device.createTexture(td, atlases.normal.data());

    g.layerSet = device.createDescriptorSet(rhi::SetType::ShaderMaterial);
    g.layerSet->setTexture(0, g.diffuseAtlas.get());
    g.layerSet->setTexture(1, g.normalAtlas.get());
    g.layerSet->setBuffer(15, g.layerBuffer.get());

    *out = std::move(g);
    return true;
}

bool uploadFoliage(rhi::Device& device, const std::vector<FoliageType>& types,
                   const std::vector<std::vector<uint8_t>>& densities, TerrainGpu* gpu, std::string* error) {
    if (!gpu) return false;
    gpu->foliageMeshes.clear();
    gpu->foliage.clear();
    if (types.size() > kFoliageMaxTypes) {
        if (error) *error = "a tile has at most " + std::to_string(kFoliageMaxTypes) + " foliage types";
        return false;
    }
    const uint32_t n = chunksPerSide(gpu->map, kTerrainChunkCells);
    if (n == 0 || gpu->chunks.size() != size_t(n) * n) {
        if (error) *error = "the terrain has no chunks for the foliage";
        return false;
    }
    // Headroom above each chunk, so the tops of the tallest foliage are inside its bounds (the cone is 2 m, scaled).
    constexpr float kHeadroom = 4.0f;
    const std::vector<uint8_t> noField;

    std::vector<FoliageMeshGpu> meshes;
    std::vector<FoliageBatchGpu> batches;
    for (uint32_t t = 0; t < types.size(); ++t) {
        const FoliageType& type = types[t];
        if (t < densities.size() && !densities[t].empty() && densities[t].size() != size_t(gpu->map.size) * gpu->map.size) {
            if (error) *error = "density map of '" + type.name + "' does not match the tile size";
            return false;
        }

        // The mesh, uploaded once for the type.
        const FoliageMesh m = buildFoliageMesh(type.shape);
        std::vector<rhi::Vertex> verts(m.positions.size());   // value-initialised
        for (size_t k = 0; k < verts.size(); ++k) {
            verts[k].pos = m.positions[k];
            verts[k].normal = m.normals[k];
        }
        FoliageMeshGpu mg;
        mg.vertices = device.createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
        mg.vertices->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
        mg.indices = device.createBuffer(rhi::BufferUsage::Index, m.indices.size() * sizeof(uint32_t));
        mg.indices->upload(m.indices.data(), m.indices.size() * sizeof(uint32_t));
        mg.indexCount = uint32_t(m.indices.size());
        meshes.push_back(std::move(mg));

        // Scatter, then one batch of rows per kFoliageBatch instances in each chunk.
        const std::vector<uint8_t>& field = t < densities.size() ? densities[t] : noField;
        const auto groups = groupByChunk(scatterFoliage(gpu->map, type, field), gpu->map.cellSize, kTerrainChunkCells, n);
        const float fadeStart = std::max(0.0f, std::min(type.fadeStart, type.fadeEnd));
        const float fadeEnd = std::max(std::max(type.fadeStart, type.fadeEnd), fadeStart + 0.001f);
        std::vector<glm::vec4> rows(3 * kFoliageBatch);
        for (size_t c = 0; c < groups.size(); ++c) {
            const std::vector<FoliageInstance>& list = groups[c];
            for (size_t first = 0; first < list.size(); first += kFoliageBatch) {
                const uint32_t count = uint32_t(std::min<size_t>(kFoliageBatch, list.size() - first));
                packFoliageRows(list.data() + first, count, rows.data());
                FoliageBatchGpu b;
                b.rows = device.createBuffer(rhi::BufferUsage::Uniform, rows.size() * sizeof(glm::vec4));
                b.rows->upload(rows.data(), rows.size() * sizeof(glm::vec4));
                b.set = device.createDescriptorSet(rhi::SetType::ShaderMaterial);
                b.set->setBuffer(15, b.rows.get());
                b.count = count;
                b.type = t;
                b.boundsMin = gpu->chunks[c].boundsMin;
                b.boundsMax = gpu->chunks[c].boundsMax + glm::vec3(0.0f, kHeadroom, 0.0f);
                b.fade = glm::vec2(fadeStart, fadeEnd);
                b.tint = type.tint;
                batches.push_back(std::move(b));
            }
        }
    }
    gpu->foliageMeshes = std::move(meshes);
    gpu->foliage = std::move(batches);
    return true;
}

} // namespace ukiyo
