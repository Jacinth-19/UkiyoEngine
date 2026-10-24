#include "engine/grass.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>

#include "engine/terrain_layers.h"

namespace ukiyo {
namespace {

// MurmurHash3's finalizer, the same mixer the foliage scatter uses. Only the scatter draws use it.
uint32_t hashMix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

uint32_t cellHash(uint32_t seed, uint32_t x, uint32_t z, uint32_t channel) {
    uint32_t h = hashMix(seed * 0x9E3779B9u + channel * 0x85EBCA6Bu);
    h = hashMix(h ^ (x * 0xC2B2AE35u));
    h = hashMix(h ^ (z * 0x27D4EB2Fu + 0x165667B1u));
    return h;
}

// One independent draw in [0, 1) for a cell and a channel.
float cellDraw(uint32_t seed, uint32_t x, uint32_t z, uint32_t channel) {
    return float(cellHash(seed, x, z, channel) >> 8) * (1.0f / 16777216.0f);
}

// Channels of the per-candidate draws.
enum Channel : uint32_t { kKeep = 0, kJitterX, kJitterZ, kYaw, kScale, kStride, kChannelCount };

// A soft 0-to-1 ramp over [0, edge]. Edge 0 makes it a step, so a threshold of zero means "any coverage".
float coverageRamp(float coverage, float edge) {
    if (edge <= 1e-4f) return coverage > 0.0f ? 1.0f : 0.0f;
    return glm::smoothstep(0.0f, edge, coverage);
}

} // namespace

GrassMesh buildGrassMesh(const GrassSettings& s) {
    GrassMesh m;
    const uint32_t blades = std::clamp(s.blades, 1u, 16u);
    const uint32_t segments = std::clamp(s.segments, 1u, 12u);
    const float height = std::max(0.01f, s.height);
    const float halfWidth = std::max(0.001f, s.width) * 0.5f;
    // A tuft is not a star of blades from one point: the roots sit on a small disc, so at a metre or two the tuft reads
    // as a clump of grass instead of a spike. The radius follows the blade height, so a tuft keeps its proportions.
    const float tuftRadius = height * 0.55f;
    // The blades fan out on the golden angle, so a tuft never looks like a symmetric star.
    const float kGolden = 2.39996323f;
    for (uint32_t b = 0; b < blades; ++b) {
        const float phi = float(b) * kGolden + 0.35f;
        const float outX = std::cos(phi), outZ = std::sin(phi);
        // Two hashes per blade, from the tuft seed: how far it leans out, and how tall it grows. A tuft of one height
        // with every blade fanned the same looks planted; a real clump has short blades in the middle of tall ones.
        const float h1 = std::fabs(std::sin(float(b) * 12.9898f + float(s.seed) * 78.233f) * 43758.5453f);
        const float h2 = std::fabs(std::sin(float(b) * 39.3467f + float(s.seed) * 11.135f) * 24634.6345f);
        const float r1 = h1 - std::floor(h1), r2 = h2 - std::floor(h2);
        const float lean = 0.65f + 0.85f * r2;
        const float bladeHeight = height * (0.66f + 0.34f * r1);
        const float root = tuftRadius * std::sqrt(float((b * 3) % 4) / 3.0f);
        const uint32_t first = uint32_t(m.positions.size());
        // Mostly-up normal with a third of the blade's facing, so the field shades as a soft mass instead of as flat
        // cards that go black when the sun is overhead.
        const glm::vec3 facing(outX, 0.0f, outZ);
        const glm::vec3 normal = glm::normalize(glm::vec3(0.0f, 0.9f, 0.0f) + facing * 0.45f);
        for (uint32_t j = 0; j <= segments; ++j) {
            const float t = float(j) / float(segments);
            const float w = halfWidth * (1.0f - 0.92f * t * t);   // tapers to a near point
            // The blade leans outward as it rises: a cantilever, so the base never moves.
            const float reach = root + tuftRadius * lean * t * t;
            const glm::vec3 mid(outX * reach, bladeHeight * t, outZ * reach);
            const glm::vec3 side(-outZ * w, 0.0f, outX * w);
            m.positions.push_back(mid - side);
            m.normals.push_back(normal);
            m.uvs.push_back({0.0f, t});
            m.positions.push_back(mid + side);
            m.normals.push_back(normal);
            m.uvs.push_back({1.0f, t});
        }
        for (uint32_t j = 0; j < segments; ++j) {
            const uint32_t a = first + j * 2;
            const uint32_t idx[6] = {a, a + 1, a + 3, a, a + 3, a + 2};
            for (uint32_t i : idx) m.indices.push_back(i);
        }
    }
    return m;
}

float grassDensityAt(float distance, const GrassSettings& s) {
    const float nearBand = std::min(s.bandNear, s.bandMid);
    const float midBand = std::max(s.bandNear, s.bandMid);
    const float cull = std::max(s.cullDistance, midBand + 0.001f);
    if (!(distance > nearBand)) return 1.0f;   // also catches NaN
    if (distance <= midBand) return glm::mix(1.0f, 0.5f, glm::smoothstep(nearBand, midBand, distance));
    return 0.5f * (1.0f - glm::smoothstep(midBand, cull, distance));
}

float grassBladeHash(glm::vec2 groundPos) {
    // Must match grass.vert: fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453).
    const float v = std::sin(glm::dot(groundPos, glm::vec2(12.9898f, 78.233f))) * 43758.5453f;
    return v - std::floor(v);
}

std::vector<GrassInstance> scatterGrass(const Heightmap& map, const std::vector<TerrainLayer>& layers,
                                        const std::vector<uint8_t>& paint, const GrassSettings& s,
                                        const std::vector<uint8_t>& detailField) {
    std::vector<GrassInstance> out;
    if (!map.valid() || layers.empty()) return out;
    const size_t samples = size_t(map.size) * map.size;
    if (!detailField.empty() && detailField.size() != samples) return out;
    if (!paint.empty() && paint.size() != samples * kTerrainMaxLayers) return out;

    const float density = std::clamp(s.density, 0.0f, 64.0f);
    if (density <= 0.0f) return out;

    // One jittered candidate per cell of a grid whose side is 1 / sqrt(density) metres. The grid is stretched to the
    // tile exactly, so the last row and column are not short.
    const float span = float(map.size - 1) * map.cellSize;
    const uint32_t cells = std::clamp(uint32_t(std::lround(double(span) * std::sqrt(double(density)))), 1u, 4096u);
    const float cell = span / float(cells);

    // Past the instance budget, keep every n-th candidate by a fixed draw, so the same blades survive every build.
    uint64_t wanted = uint64_t(cells) * cells;
    uint32_t stride = 1;
    if (wanted > kGrassMaxInstances) stride = uint32_t((wanted + kGrassMaxInstances - 1) / kGrassMaxInstances);

    const float heightMin = std::min(s.heightMin, s.heightMax);
    const float heightMax = std::max(s.heightMin, s.heightMax);
    const float scaleLo = std::min(s.scaleMin, s.scaleMax);
    const float scaleHi = std::max(s.scaleMin, s.scaleMax);
    const float align = std::clamp(s.groundAlign, 0.0f, 1.0f);
    const float includeEdge = std::clamp(s.includeThreshold, 0.0f, 1.0f);
    const float excludeEdge = std::clamp(s.excludeThreshold, 0.0f, 1.0f);
    const int last = int(map.size) - 1;
    out.reserve(size_t(std::min<uint64_t>(wanted, kGrassMaxInstances)));

    for (uint32_t cz = 0; cz < cells; ++cz) {
        for (uint32_t cx = 0; cx < cells; ++cx) {
            if (stride > 1 && cellHash(s.seed, cx, cz, kStride) % stride != 0) continue;
            const float u = cellDraw(s.seed, cx, cz, kJitterX);
            const float v = cellDraw(s.seed, cx, cz, kJitterZ);
            const float px = (float(cx) + u) * cell;
            const float pz = (float(cz) + v) * cell;

            // Height and slope are read at the nearest sample, so the limits agree with the ground the blade stands on.
            const int sx = std::clamp(int(std::lround(px / map.cellSize)), 0, last);
            const int sz = std::clamp(int(std::lround(pz / map.cellSize)), 0, last);
            const size_t sample = size_t(sz) * map.size + sx;
            const float h = map.samples[sample];
            if (h < heightMin || h > heightMax) continue;
            if (slopeDegAt(map, sx, sz) > s.slopeMax) continue;

            // The layer masks, the adaptation of the reference's splat channels and terrain alpha maps: the largest
            // weight among the included layers has to reach the threshold, and an excluded layer at its threshold
            // clears the blade. Both edges are soft so the field thins out instead of ending on a line.
            float keep = detailField.empty() ? 1.0f : float(detailField[sample]) * (1.0f / 255.0f);
            if ((s.includeLayers != 0u && s.includeLayers != 0xFFFFFFFFu) || s.excludeLayers != 0u) {
                float w[kTerrainMaxLayers];
                layerWeightsAtSample(map, layers, paint, sample, w);
                if (s.includeLayers != 0u) {
                    float best = 0.0f;
                    for (uint32_t k = 0; k < kTerrainMaxLayers; ++k)
                        if (s.includeLayers & (1u << k)) best = std::max(best, w[k]);
                    keep *= coverageRamp(best, includeEdge);
                }
                if (s.excludeLayers != 0u) {
                    float worst = 0.0f;
                    for (uint32_t k = 0; k < kTerrainMaxLayers; ++k)
                        if (s.excludeLayers & (1u << k)) worst = std::max(worst, w[k]);
                    keep *= 1.0f - coverageRamp(worst, excludeEdge);
                }
            }
            if (keep <= 0.0f) continue;
            if (cellDraw(s.seed, cx, cz, kKeep) >= keep) continue;

            GrassInstance g;
            g.pos = glm::vec3(px, map.heightAtMetres(px, pz), pz);
            const glm::vec3 n = map.normalAt(sx, sz);
            g.up = glm::normalize(glm::mix(glm::vec3(0.0f, 1.0f, 0.0f), n, align));
            g.yaw = cellDraw(s.seed, cx, cz, kYaw) * glm::two_pi<float>();
            g.scale = scaleLo + (scaleHi - scaleLo) * cellDraw(s.seed, cx, cz, kScale);
            out.push_back(g);
        }
    }
    return out;
}

std::vector<std::vector<GrassInstance>> groupGrassByChunk(const std::vector<GrassInstance>& blades, float cellSize,
                                                          uint32_t chunkCells, uint32_t chunksPerSide) {
    std::vector<std::vector<GrassInstance>> groups(size_t(chunksPerSide) * chunksPerSide);
    if (chunksPerSide == 0 || chunkCells == 0 || cellSize <= 0.0f) return groups;
    const float span = cellSize * float(chunkCells);
    const int last = int(chunksPerSide) - 1;
    for (const GrassInstance& g : blades) {
        const int cx = std::clamp(int(std::floor(g.pos.x / span)), 0, last);
        const int cz = std::clamp(int(std::floor(g.pos.z / span)), 0, last);
        groups[size_t(cz) * chunksPerSide + size_t(cx)].push_back(g);
    }
    return groups;
}

void packGrassRows(const GrassInstance* instances, uint32_t count, glm::vec4* rows) {
    for (uint32_t i = 0; i < 3 * kGrassBatch; ++i) rows[i] = glm::vec4(0.0f);
    count = std::min(count, kGrassBatch);
    for (uint32_t i = 0; i < count; ++i) {
        const GrassInstance& g = instances[i];
        const glm::vec3 up = glm::length(g.up) > 1e-5f ? glm::normalize(g.up) : glm::vec3(0.0f, 1.0f, 0.0f);
        // A yaw around `up`: the blade's local x (width) and z (facing) axes come from any vector not parallel to up.
        const glm::vec3 helper = std::fabs(up.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 side = glm::normalize(glm::cross(helper, up));
        const float c = std::cos(g.yaw), sn = std::sin(g.yaw);
        const glm::vec3 x = side * c + glm::cross(up, side) * sn;   // width axis
        const glm::vec3 z = glm::cross(x, up);                      // facing axis
        const float s = g.scale;
        for (int r = 0; r < 3; ++r) {
            // Column r of the 3 by 3 basis, times the scale, and the translation in the fourth slot.
            const glm::vec3 basis(x[r] * s, up[r] * s, z[r] * s);
            rows[3 * i + size_t(r)] = glm::vec4(basis, g.pos[r]);
        }
    }
}

} // namespace ukiyo
