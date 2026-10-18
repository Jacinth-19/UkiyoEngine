#include "engine/foliage.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/terrain_layers.h"

namespace ukiyo {
namespace {

// Integer hash (the finalizer of MurmurHash3). Used only for the per-cell draws.
uint32_t hashMix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// One draw in [0, 1) for a cell, a channel (what the draw is for) and the seed.
float cellDraw(uint32_t seed, uint32_t x, uint32_t z, uint32_t channel) {
    uint32_t h = hashMix(seed * 0x9E3779B9u + channel * 0x85EBCA6Bu);
    h = hashMix(h ^ (x * 0xC2B2AE35u));
    h = hashMix(h ^ (z * 0x27D4EB2Fu + 0x165667B1u));
    return float(h >> 8) * (1.0f / 16777216.0f);
}

// Channels of the per-cell draws. Each one is independent of the others.
enum Channel : uint32_t { kKeep = 0, kOffsetX, kOffsetZ, kYaw, kScale };

} // namespace

bool loadDensityField(const std::string& path, uint32_t size, std::vector<uint8_t>* out, std::string* error) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!loadRgba8(path, &w, &h, &rgba, error)) return false;
    if (w == 0 || h == 0) {
        if (error) *error = "density map is empty: " + path;
        return false;
    }
    out->assign(size_t(size) * size, 0);
    for (uint32_t z = 0; z < size; ++z) {
        const uint32_t sy = std::min(h - 1, uint32_t((uint64_t(z) * h) / size));
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t sx = std::min(w - 1, uint32_t((uint64_t(x) * w) / size));
            (*out)[size_t(z) * size + x] = rgba[(size_t(sy) * w + sx) * 4];
        }
    }
    return true;
}

std::vector<FoliageInstance> scatterFoliage(const Heightmap& map, const FoliageType& type,
                                            const std::vector<uint8_t>& densityField) {
    std::vector<FoliageInstance> out;
    if (!map.valid()) return out;
    const uint32_t size = map.size;
    if (!densityField.empty() && densityField.size() != size_t(size) * size) return out;

    const float density = std::clamp(type.density, 0.0f, 1.0f);
    if (density <= 0.0f) return out;
    const float heightMin = std::min(type.heightMin, type.heightMax);
    const float heightMax = std::max(type.heightMin, type.heightMax);
    const float scaleLo = std::min(type.scaleMin, type.scaleMax);
    const float scaleHi = std::max(type.scaleMin, type.scaleMax);
    const uint32_t seed = type.seed;

    for (uint32_t z = 0; z + 1 < size; ++z) {
        for (uint32_t x = 0; x + 1 < size; ++x) {
            const float field = densityField.empty() ? 1.0f : densityField[size_t(z) * size + x] / 255.0f;
            if (cellDraw(seed, x, z, kKeep) >= density * field) continue;

            const float u = cellDraw(seed, x, z, kOffsetX);
            const float v = cellDraw(seed, x, z, kOffsetZ);
            const float px = (float(x) + u) * map.cellSize;
            const float pz = (float(z) + v) * map.cellSize;

            // Height and slope are read at the nearest sample, so the limits match the terrain the instance stands on.
            const int sx = std::min(int(size) - 1, int(std::lround(float(x) + u)));
            const int sz = std::min(int(size) - 1, int(std::lround(float(z) + v)));
            const float h = map.samples[size_t(sz) * size + sx];
            if (h < heightMin || h > heightMax) continue;
            if (slopeDegAt(map, sx, sz) > type.slopeMax) continue;

            FoliageInstance f;
            f.pos = glm::vec3(px, map.heightAtMetres(px, pz), pz);
            f.yaw = cellDraw(seed, x, z, kYaw) * glm::two_pi<float>();
            f.scale = scaleLo + (scaleHi - scaleLo) * cellDraw(seed, x, z, kScale);
            out.push_back(f);
        }
    }
    return out;
}

std::vector<std::vector<FoliageInstance>> groupByChunk(const std::vector<FoliageInstance>& instances, float cellSize,
                                                       uint32_t chunkCells, uint32_t chunksPerSide) {
    std::vector<std::vector<FoliageInstance>> groups(size_t(chunksPerSide) * chunksPerSide);
    if (chunksPerSide == 0 || chunkCells == 0 || cellSize <= 0.0f) return groups;
    const float span = cellSize * float(chunkCells);
    const int last = int(chunksPerSide) - 1;
    for (const FoliageInstance& f : instances) {
        const int cx = std::clamp(int(std::floor(f.pos.x / span)), 0, last);
        const int cz = std::clamp(int(std::floor(f.pos.z / span)), 0, last);
        groups[size_t(cz) * chunksPerSide + size_t(cx)].push_back(f);
    }
    return groups;
}

FoliageMesh buildFoliageMesh(FoliageShape shape) {
    FoliageMesh m;
    if (shape == FoliageShape::Tuft) {
        // Two quads crossed at a right angle. Each is 0.5 m wide at the base, tapering to 0.16 m at the 1 m top.
        const glm::vec3 base[4] = {{-0.25f, 0.0f, 0.0f}, {0.25f, 0.0f, 0.0f}, {0.08f, 1.0f, 0.0f}, {-0.08f, 1.0f, 0.0f}};
        const glm::vec3 nrm = glm::normalize(glm::vec3(0.0f, 0.6f, 0.8f));
        for (int q = 0; q < 2; ++q) {
            const float a = q * glm::half_pi<float>();
            const float c = std::cos(a), s = std::sin(a);
            const uint32_t first = uint32_t(m.positions.size());
            for (const glm::vec3& p : base) {
                m.positions.push_back({p.x * c + p.z * s, p.y, -p.x * s + p.z * c});
                m.normals.push_back({nrm.x * c + nrm.z * s, nrm.y, -nrm.x * s + nrm.z * c});
            }
            const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
            for (uint32_t i : idx) m.indices.push_back(first + i);
        }
        return m;
    }

    // Cone: an apex 2 m up and an 8-sided ring of radius 0.6 m at the ground. The ground face is left open, because
    // the foliage sits on the terrain and the base is hidden.
    constexpr int kSides = 8;
    constexpr float kRadius = 0.6f;
    constexpr float kHeight = 2.0f;
    m.positions.push_back({0.0f, kHeight, 0.0f});
    m.normals.push_back({0.0f, 1.0f, 0.0f});
    for (int i = 0; i < kSides; ++i) {
        const float t = float(i) / kSides * glm::two_pi<float>();
        const float c = std::cos(t), s = std::sin(t);
        m.positions.push_back({kRadius * c, 0.0f, kRadius * s});
        m.normals.push_back(glm::normalize(glm::vec3(c * kHeight, kRadius, s * kHeight)));
    }
    for (int i = 0; i < kSides; ++i) {
        m.indices.push_back(0);
        m.indices.push_back(uint32_t(1 + i));
        m.indices.push_back(uint32_t(1 + (i + 1) % kSides));
    }
    return m;
}

void packFoliageRows(const FoliageInstance* instances, uint32_t count, glm::vec4* rows) {
    for (uint32_t i = 0; i < 3 * kFoliageBatch; ++i) rows[i] = glm::vec4(0.0f);
    count = std::min(count, kFoliageBatch);
    for (uint32_t i = 0; i < count; ++i) {
        const FoliageInstance& f = instances[i];
        const glm::mat4 m = glm::translate(glm::mat4(1.0f), f.pos) *
                            glm::rotate(glm::mat4(1.0f), f.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(f.scale));
        for (int r = 0; r < 3; ++r) {
            rows[3 * i + r] = glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]);
        }
    }
}

} // namespace ukiyo
