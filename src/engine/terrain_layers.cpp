#include "engine/terrain_layers.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "stb_image.h"

namespace ukiyo {

namespace {

// 1 inside the band, falling to 0 over `fade` outside each edge. A zero fade is a hard edge.
float band(float v, float lo, float hi, float fade) {
    if (fade <= 0.0f) return (v >= lo && v <= hi) ? 1.0f : 0.0f;
    const float rise = glm::smoothstep(lo - fade, lo, v);
    const float fall = 1.0f - glm::smoothstep(hi, hi + fade, v);
    return rise * fall;
}

// Resamples an RGBA8 image to one atlas cell by bilinear filtering.
void resampleToCell(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, std::vector<uint8_t>& cell) {
    const uint32_t n = kTerrainAtlasCell;
    cell.assign(size_t(n) * n * 4, 0);
    for (uint32_t y = 0; y < n; ++y) {
        const float fy = std::clamp((float(y) + 0.5f) * float(sh) / float(n) - 0.5f, 0.0f, float(sh - 1));
        const uint32_t y0 = uint32_t(fy), y1 = std::min(y0 + 1, sh - 1);
        const float ty = fy - float(y0);
        for (uint32_t x = 0; x < n; ++x) {
            const float fx = std::clamp((float(x) + 0.5f) * float(sw) / float(n) - 0.5f, 0.0f, float(sw - 1));
            const uint32_t x0 = uint32_t(fx), x1 = std::min(x0 + 1, sw - 1);
            const float tx = fx - float(x0);
            for (int c = 0; c < 4; ++c) {
                auto at = [&](uint32_t xx, uint32_t yy) { return float(src[(size_t(yy) * sw + xx) * 4 + c]); };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                cell[(size_t(y) * n + x) * 4 + c] = uint8_t(std::lround(top + (bottom - top) * ty));
            }
        }
    }
}

} // namespace

float slopeDegAt(const Heightmap& map, int x, int z) {
    // Central differences over the span actually used. At an edge the span is one cell, not two, so the slope is not
    // halved there (this matches Heightmap::normalAt).
    const int last = int(map.size) - 1;
    const int xa = std::max(x - 1, 0), xb = std::min(x + 1, last);
    const int za = std::max(z - 1, 0), zb = std::min(z + 1, last);
    const float dx = (map.heightAt(xb, z) - map.heightAt(xa, z)) / (float(xb - xa) * map.cellSize);
    const float dz = (map.heightAt(x, zb) - map.heightAt(x, za)) / (float(zb - za) * map.cellSize);
    return glm::degrees(std::atan(std::sqrt(dx * dx + dz * dz)));
}

std::vector<float> layerWeightMap(const Heightmap& map, const std::vector<TerrainLayer>& layers,
                                  const std::vector<uint8_t>& paint) {
    const size_t count = std::min<size_t>(layers.size(), kTerrainMaxLayers);
    if (!map.valid() || count == 0) return {};
    const size_t samples = size_t(map.size) * map.size;
    if (!paint.empty() && paint.size() != samples * kTerrainMaxLayers) return {};

    std::vector<float> out(samples * kTerrainMaxLayers, 0.0f);
    for (size_t s = 0; s < samples; ++s) layerWeightsAtSample(map, layers, paint, s, &out[s * kTerrainMaxLayers]);
    return out;
}

void layerWeightsAtSample(const Heightmap& map, const std::vector<TerrainLayer>& layers,
                          const std::vector<uint8_t>& paint, size_t sample, float* out) {
    const size_t count = std::min<size_t>(layers.size(), kTerrainMaxLayers);
    if (!out) return;
    for (uint32_t k = 0; k < kTerrainMaxLayers; ++k) out[k] = 0.0f;
    if (count == 0 || !map.valid() || sample >= size_t(map.size) * map.size) return;
    const uint32_t x = uint32_t(sample % map.size);
    const uint32_t z = uint32_t(sample / map.size);
    const bool painted = !paint.empty() && paint.size() >= (sample + 1) * kTerrainMaxLayers;
    const float h = map.samples[sample];
    const float slope = slopeDegAt(map, int(x), int(z));
    float total = 0.0f;
    for (size_t k = 0; k < count; ++k) {
        const TerrainLayer& L = layers[k];
        float v = band(h, L.heightMin, L.heightMax, L.heightFade) * band(slope, L.slopeMin, L.slopeMax, L.slopeFade);
        if (painted) v *= float(paint[sample * kTerrainMaxLayers + k]) / 255.0f;
        out[k] = v;
        total += v;
    }
    if (total > 1e-6f) {
        for (size_t k = 0; k < count; ++k) out[k] /= total;
    } else {
        out[0] = 1.0f;   // nothing matched or everything is painted off: the base layer takes the sample
    }
}

bool loadRgba8(const std::string& path, uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgba, std::string* error) {
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!px || w <= 0 || h <= 0) {
        if (error) *error = "could not read the image " + path;
        return false;
    }
    *width = uint32_t(w);
    *height = uint32_t(h);
    rgba->assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

bool buildTerrainAtlases(const std::vector<TerrainLayer>& layers, TerrainAtlases* out, std::string* error) {
    if (!out) return false;
    const size_t count = std::min<size_t>(layers.size(), kTerrainMaxLayers);
    TerrainAtlases a;
    const size_t texels = size_t(a.width) * a.height;
    a.diffuse.assign(texels * 4, 255);
    a.normal.assign(texels * 4, 255);
    for (size_t t = 0; t < texels; ++t) {
        a.normal[t * 4 + 0] = 128;
        a.normal[t * 4 + 1] = 128;
    }

    const uint32_t n = kTerrainAtlasCell;
    std::vector<uint8_t> img, cell;
    for (size_t k = 0; k < kTerrainMaxLayers; ++k) {
        const uint32_t x0 = uint32_t(k % kTerrainAtlasCols) * n;
        const uint32_t y0 = uint32_t(k / kTerrainAtlasCols) * n;
        const TerrainLayer* L = k < count ? &layers[k] : nullptr;
        const float constRough = L ? L->roughness : 0.8f;

        // Diffuse: white unless a file is given. The roughness goes in the alpha.
        std::vector<uint8_t> diffuse(size_t(n) * n * 4, 255);
        if (L && !L->diffuseFile.empty()) {
            uint32_t w = 0, h = 0;
            if (!loadRgba8(L->diffuseFile, &w, &h, &img, error)) return false;
            resampleToCell(img, w, h, diffuse);
        }
        std::vector<uint8_t> rough(size_t(n) * n, uint8_t(std::lround(std::clamp(constRough, 0.0f, 1.0f) * 255.0f)));
        if (L && !L->roughnessFile.empty()) {
            uint32_t w = 0, h = 0;
            if (!loadRgba8(L->roughnessFile, &w, &h, &img, error)) return false;
            resampleToCell(img, w, h, cell);
            for (size_t t = 0; t < rough.size(); ++t) rough[t] = cell[t * 4];
        }
        for (uint32_t y = 0; y < n; ++y) {
            for (uint32_t x = 0; x < n; ++x) {
                const size_t src = (size_t(y) * n + x);
                const size_t dst = (size_t(y0 + y) * a.width + (x0 + x)) * 4;
                a.diffuse[dst + 0] = diffuse[src * 4 + 0];
                a.diffuse[dst + 1] = diffuse[src * 4 + 1];
                a.diffuse[dst + 2] = diffuse[src * 4 + 2];
                a.diffuse[dst + 3] = rough[src];
            }
        }

        // Normal: flat unless a file is given.
        if (L && !L->normalFile.empty()) {
            uint32_t w = 0, h = 0;
            if (!loadRgba8(L->normalFile, &w, &h, &img, error)) return false;
            resampleToCell(img, w, h, cell);
            for (uint32_t y = 0; y < n; ++y) {
                for (uint32_t x = 0; x < n; ++x) {
                    const size_t src = (size_t(y) * n + x) * 4;
                    const size_t dst = (size_t(y0 + y) * a.width + (x0 + x)) * 4;
                    for (int c = 0; c < 3; ++c) a.normal[dst + c] = cell[src + c];
                }
            }
        }
    }
    *out = std::move(a);
    return true;
}

std::vector<TerrainLayer> defaultTerrainLayers() {
    TerrainLayer grass;
    grass.name = "grass";
    grass.colour = glm::vec3(0.30f, 0.42f, 0.16f);
    grass.roughness = 0.9f;
    grass.heightMin = 0.0f;
    grass.heightMax = 0.55f;
    grass.heightFade = 0.08f;
    grass.slopeMin = 0.0f;
    grass.slopeMax = 32.0f;
    grass.slopeFade = 6.0f;

    TerrainLayer rock;
    rock.name = "rock";
    rock.colour = glm::vec3(0.36f, 0.33f, 0.30f);
    rock.roughness = 0.7f;
    rock.heightMin = 0.0f;
    rock.heightMax = 1.0f;
    rock.heightFade = 0.0f;
    rock.slopeMin = 32.0f;
    rock.slopeMax = 90.0f;
    rock.slopeFade = 6.0f;

    TerrainLayer pale;
    pale.name = "pale rock";
    pale.colour = glm::vec3(0.62f, 0.60f, 0.56f);
    pale.roughness = 0.5f;
    pale.heightMin = 0.72f;
    pale.heightMax = 1.0f;
    pale.heightFade = 0.06f;
    pale.slopeMin = 0.0f;
    pale.slopeMax = 90.0f;
    pale.slopeFade = 0.0f;

    return {grass, rock, pale};
}

} // namespace ukiyo
