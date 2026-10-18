#include "engine/terrain_edit.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/constants.hpp>

#include "engine/terrain_layers.h"

namespace ukiyo {
namespace {

uint32_t hashMix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// A lattice value in [-1, 1] for an integer point and a seed.
float lattice(int x, int z, uint32_t seed) {
    uint32_t h = hashMix(seed * 0x9E3779B9u ^ (uint32_t(x) * 0x85EBCA6Bu) ^ (uint32_t(z) * 0xC2B2AE35u));
    return float(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Bilinear height in metres at a point given in samples (clamped to the grid).
float sampleSamples(const std::vector<float>& h, uint32_t n, float x, float z) {
    const int last = int(n) - 1;
    const float cx = std::clamp(x, 0.0f, float(last));
    const float cz = std::clamp(z, 0.0f, float(last));
    const int x0 = std::min(int(cx), last - 1), z0 = std::min(int(cz), last - 1);
    const float fx = cx - x0, fz = cz - z0;
    const float h00 = h[size_t(z0) * n + x0], h10 = h[size_t(z0) * n + x0 + 1];
    const float h01 = h[size_t(z0 + 1) * n + x0], h11 = h[size_t(z0 + 1) * n + x0 + 1];
    return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
}

// Adds `amount` metres to the four samples around a point, weighted bilinearly.
void addBilinear(std::vector<float>& h, uint32_t n, float x, float z, float amount) {
    const int last = int(n) - 1;
    const int x0 = std::clamp(int(x), 0, last - 1), z0 = std::clamp(int(z), 0, last - 1);
    const float fx = x - x0, fz = z - z0;
    h[size_t(z0) * n + x0] += amount * (1 - fx) * (1 - fz);
    h[size_t(z0) * n + x0 + 1] += amount * fx * (1 - fz);
    h[size_t(z0 + 1) * n + x0] += amount * (1 - fx) * fz;
    h[size_t(z0 + 1) * n + x0 + 1] += amount * fx * fz;
}

} // namespace

float brushWeight(const BrushDab& d, float distance) {
    if (d.radius <= 0.0f || distance >= d.radius) return 0.0f;
    const float inner = std::clamp(d.falloff, 0.0f, 1.0f) * d.radius;
    if (distance <= inner) return 1.0f;
    const float t = (distance - inner) / (d.radius - inner);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

std::vector<HeightEdit> heightDab(const Heightmap& map, const BrushDab& d,
                                  const std::function<float(int x, int z, float h)>& target) {
    std::vector<HeightEdit> out;
    if (!map.valid() || d.radius <= 0.0f || d.strength == 0.0f || !target) return out;
    const int last = int(map.size) - 1;
    const float cell = map.cellSize;
    const int x0 = std::max(0, int(std::floor((d.centre.x - d.radius) / cell)));
    const int x1 = std::min(last, int(std::ceil((d.centre.x + d.radius) / cell)));
    const int z0 = std::max(0, int(std::floor((d.centre.y - d.radius) / cell)));
    const int z1 = std::min(last, int(std::ceil((d.centre.y + d.radius) / cell)));
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float w = brushWeight(d, glm::distance(glm::vec2(x * cell, z * cell), d.centre));
            if (w <= 0.0f) continue;
            const uint32_t idx = uint32_t(z) * map.size + uint32_t(x);
            const float h = map.samples[idx];
            const float t = target(x, z, h);
            const float after = std::clamp(h + (t - h) * w * d.strength, 0.0f, 1.0f);
            if (after != h) out.push_back({idx, h, after});
        }
    }
    return out;
}

void applyHeightEdits(Heightmap& map, const std::vector<HeightEdit>& edits, bool useAfter) {
    for (const HeightEdit& e : edits)
        if (e.index < map.samples.size()) map.samples[e.index] = useAfter ? e.after : e.before;
}

float valueNoise(float x, float z, uint32_t seed) {
    const float fx0 = std::floor(x), fz0 = std::floor(z);
    const int x0 = int(fx0), z0 = int(fz0);
    const float sx = smooth(x - fx0), sz = smooth(z - fz0);
    const float a = lattice(x0, z0, seed), b = lattice(x0 + 1, z0, seed);
    const float c = lattice(x0, z0 + 1, seed), d = lattice(x0 + 1, z0 + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz;
}

float fractalNoise(float x, float z, uint32_t seed, uint32_t octaves) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (uint32_t o = 0; o < std::max(1u, octaves); ++o) {
        sum += amp * valueNoise(x * freq, z * freq, seed + o * 101u);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

float terraceLevel(float h, uint32_t steps) {
    const float s = float(std::max(1u, steps));
    return std::floor(std::clamp(h, 0.0f, 1.0f) * s) / s;
}

Stamp domeStamp(uint32_t size) {
    Stamp s;
    s.size = std::max(2u, size);
    s.values.resize(size_t(s.size) * s.size);
    for (uint32_t z = 0; z < s.size; ++z) {
        for (uint32_t x = 0; x < s.size; ++x) {
            const float u = (float(x) + 0.5f) / s.size * 2.0f - 1.0f;
            const float v = (float(z) + 0.5f) / s.size * 2.0f - 1.0f;
            const float d = std::sqrt(u * u + v * v);
            s.values[size_t(z) * s.size + x] = d < 1.0f ? 0.5f + 0.5f * std::cos(glm::pi<float>() * d) : 0.0f;
        }
    }
    return s;
}

bool loadStamp(const std::string& path, Stamp* out, std::string* error) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!loadRgba8(path, &w, &h, &rgba, error)) return false;
    if (w != h || w < 2) {
        if (error) *error = "a stamp image must be square, at least 2 by 2: " + path;
        return false;
    }
    out->size = w;
    out->values.resize(size_t(w) * h);
    for (size_t i = 0; i < out->values.size(); ++i) out->values[i] = rgba[i * 4] / 255.0f;
    return true;
}

float stampAt(const Stamp& s, const BrushDab& d, float x, float z) {
    if (s.size == 0 || s.values.size() != size_t(s.size) * s.size || d.radius <= 0.0f) return 0.0f;
    const float u = (x - d.centre.x) / d.radius * 0.5f + 0.5f;
    const float v = (z - d.centre.y) / d.radius * 0.5f + 0.5f;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return 0.0f;
    const uint32_t ix = std::min(s.size - 1, uint32_t(u * (s.size - 1) + 0.5f));
    const uint32_t iz = std::min(s.size - 1, uint32_t(v * (s.size - 1) + 0.5f));
    return s.values[size_t(iz) * s.size + ix];
}

std::vector<HeightEdit> erodeHeightmap(const Heightmap& map, const ErosionSettings& s) {
    std::vector<HeightEdit> out;
    if (!map.valid() || map.heightScale <= 0.0f || s.droplets == 0) return out;
    const uint32_t n = map.size;
    const float last = float(n - 1);

    // Work in metres, so the carrying capacity and the speeds mean the same thing on any height scale.
    std::vector<float> h(map.samples.size());
    for (size_t i = 0; i < h.size(); ++i) h[i] = map.samples[i] * map.heightScale;

    uint64_t state = uint64_t(s.seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    auto next = [&state]() {   // splitmix64, a float in [0, 1)
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return float(z >> 40) * (1.0f / 16777216.0f);
    };

    for (uint32_t i = 0; i < s.droplets; ++i) {
        float px = next() * last, pz = next() * last;
        glm::vec2 dir(0.0f);
        float speed = 1.0f, water = 1.0f, sediment = 0.0f;
        for (uint32_t step = 0; step < s.maxSteps; ++step) {
            if (px < 0.0f || pz < 0.0f || px >= last || pz >= last) break;
            const int x0 = int(px), z0 = int(pz);
            const float fx = px - x0, fz = pz - z0;
            const float h00 = h[size_t(z0) * n + x0], h10 = h[size_t(z0) * n + x0 + 1];
            const float h01 = h[size_t(z0 + 1) * n + x0], h11 = h[size_t(z0 + 1) * n + x0 + 1];
            const glm::vec2 grad((h10 - h00) * (1 - fz) + (h11 - h01) * fz, (h01 - h00) * (1 - fx) + (h11 - h10) * fx);
            const float hOld = h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz;

            dir = dir * s.inertia - grad * (1.0f - s.inertia);
            const float len = glm::length(dir);
            if (len < 1e-6f) {
                const float a = next() * glm::two_pi<float>();
                dir = glm::vec2(std::cos(a), std::sin(a));
            } else {
                dir /= len;
            }
            const float npx = px + dir.x, npz = pz + dir.y;
            if (npx < 0.0f || npz < 0.0f || npx >= last || npz >= last) {
                addBilinear(h, n, px, pz, sediment);   // leaves the tile: it drops what it carries
                sediment = 0.0f;
                break;
            }
            const float hNew = sampleSamples(h, n, npx, npz);
            const float dh = hNew - hOld;
            const float capacity = std::max(-dh * speed * water * s.capacity, s.minSlope);
            if (sediment > capacity || dh > 0.0f) {
                const float amount = dh > 0.0f ? std::min(dh, sediment) : (sediment - capacity) * s.depositRate;
                sediment -= amount;
                addBilinear(h, n, px, pz, amount);
            } else {
                const float amount = std::min((capacity - sediment) * s.erodeRate, -dh);
                sediment += amount;
                addBilinear(h, n, px, pz, -amount);
            }
            speed = std::sqrt(std::max(speed * speed + dh * s.gravity, 0.0f));
            water *= 1.0f - s.evaporate;
            px = npx;
            pz = npz;
        }
    }

    for (size_t i = 0; i < h.size(); ++i) {
        const float after = std::clamp(h[i] / map.heightScale, 0.0f, 1.0f);
        if (std::fabs(after - map.samples[i]) > 1e-6f) out.push_back({uint32_t(i), map.samples[i], after});
    }
    return out;
}

std::vector<PaintEdit> paintDab(const std::vector<uint8_t>& field, uint32_t size, uint32_t channels, uint32_t channel,
                                const BrushDab& d, uint8_t target, float cell) {
    std::vector<PaintEdit> out;
    if (size < 2 || channels == 0 || channel >= channels || field.size() != size_t(size) * size * channels) return out;
    if (d.radius <= 0.0f || d.strength == 0.0f || cell <= 0.0f) return out;
    const int last = int(size) - 1;
    const int x0 = std::max(0, int(std::floor((d.centre.x - d.radius) / cell)));
    const int x1 = std::min(last, int(std::ceil((d.centre.x + d.radius) / cell)));
    const int z0 = std::max(0, int(std::floor((d.centre.y - d.radius) / cell)));
    const int z1 = std::min(last, int(std::ceil((d.centre.y + d.radius) / cell)));
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float w = brushWeight(d, glm::distance(glm::vec2(x * cell, z * cell), d.centre));
            if (w <= 0.0f) continue;
            const size_t idx = (size_t(z) * size + x) * channels + channel;
            const float before = field[idx];
            const float after = std::clamp(before + (float(target) - before) * w * d.strength, 0.0f, 255.0f);
            const uint8_t a = uint8_t(std::lround(after));
            if (a != field[idx]) out.push_back({uint32_t(idx), field[idx], a});
        }
    }
    return out;
}

void applyPaintEdits(std::vector<uint8_t>& field, const std::vector<PaintEdit>& edits, bool useAfter) {
    for (const PaintEdit& e : edits)
        if (e.index < field.size()) field[e.index] = useAfter ? e.after : e.before;
}

void mergeHeightEdits(std::vector<HeightEdit>& acc, const std::vector<HeightEdit>& more) {
    std::unordered_map<uint32_t, size_t> at;
    at.reserve(acc.size());
    for (size_t i = 0; i < acc.size(); ++i) at[acc[i].index] = i;
    for (const HeightEdit& e : more) {
        auto it = at.find(e.index);
        if (it != at.end()) {
            acc[it->second].after = e.after;
        } else {
            at[e.index] = acc.size();
            acc.push_back(e);
        }
    }
}

void mergePaintEdits(std::vector<PaintEdit>& acc, const std::vector<PaintEdit>& more) {
    std::unordered_map<uint32_t, size_t> at;
    at.reserve(acc.size());
    for (size_t i = 0; i < acc.size(); ++i) at[acc[i].index] = i;
    for (const PaintEdit& e : more) {
        auto it = at.find(e.index);
        if (it != at.end()) {
            acc[it->second].after = e.after;
        } else {
            at[e.index] = acc.size();
            acc.push_back(e);
        }
    }
}

bool raycastHeightmap(const Heightmap& map, glm::vec3 origin, glm::vec3 dir, float maxDistance, glm::vec3* hit) {
    if (!map.valid() || !hit || maxDistance <= 0.0f) return false;
    const float len = glm::length(dir);
    if (len < 1e-9f) return false;
    dir /= len;
    const float extent = float(map.size - 1) * map.cellSize;
    const float step = 0.5f * map.cellSize;
    auto inside = [&](const glm::vec3& p) { return p.x >= 0.0f && p.z >= 0.0f && p.x <= extent && p.z <= extent; };
    auto above = [&](const glm::vec3& p) { return p.y - map.heightAtMetres(p.x, p.z); };

    float prevT = 0.0f;
    for (float t = 0.0f; t <= maxDistance; t += step) {
        const glm::vec3 p = origin + dir * t;
        if (inside(p) && above(p) <= 0.0f) {
            // The crossing is between prevT (above, or outside the grid) and t (below). Bisect it.
            float lo = prevT, hi = t;
            for (int i = 0; i < 12; ++i) {
                const float mid = 0.5f * (lo + hi);
                const glm::vec3 q = origin + dir * mid;
                if (inside(q) && above(q) > 0.0f) lo = mid;
                else hi = mid;
            }
            glm::vec3 r = origin + dir * hi;
            r.y = map.heightAtMetres(r.x, r.z);
            *hit = r;
            return true;
        }
        prevT = t;
    }
    return false;
}

} // namespace ukiyo
