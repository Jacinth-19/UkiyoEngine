// Terrain editing (item 18, steps 4 and 5): brush dabs on the heights and on the masks, noise, terrace and stamp
// brushes, hydraulic erosion, and a ray pick onto the heightmap. Pure CPU code. Each operation returns its edits
// instead of applying them, so the editor can undo them exactly.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/terrain.h"

namespace ukiyo {

// One brush dab. The centre is in tile space, in metres from the heightmap origin (x and z).
struct BrushDab {
    glm::vec2 centre{0.0f};
    float radius = 8.0f;     // metres
    float falloff = 0.5f;    // 0 is a soft edge, 1 a hard one. Full weight inside falloff * radius
    float strength = 1.0f;   // 0 to 1 per dab; negative values reverse the direction of a height brush
};

// Weight of a dab at `distance` metres from its centre. 1 inside the hard part, smooth to 0 at the radius.
float brushWeight(const BrushDab& d, float distance);

// One changed sample of the heightmap (normalised heights) or of a mask byte.
struct HeightEdit { uint32_t index; float before; float after; };
struct PaintEdit { uint32_t index; uint8_t before; uint8_t after; };

// The edits of a height dab. `target(x, z, h)` is the height the brush moves towards at a sample, and the dab moves
// the sample by (target - h) * weight * strength, clamped to [0, 1]. Samples the dab does not change are left out.
std::vector<HeightEdit> heightDab(const Heightmap& map, const BrushDab& d,
                                  const std::function<float(int x, int z, float h)>& target);
void applyHeightEdits(Heightmap& map, const std::vector<HeightEdit>& edits, bool useAfter);

// Value noise in [-1, 1] at (x, z), and a sum of `octaves` of it (each octave twice the frequency, half the amplitude).
float valueNoise(float x, float z, uint32_t seed);
float fractalNoise(float x, float z, uint32_t seed, uint32_t octaves);

// Terrace: the height rounded down to one of `steps` levels (steps of at least 1).
float terraceLevel(float h, uint32_t steps);

// Stamp: a square image of heights in [0, 1], laid over the brush. The default is a smooth dome.
struct Stamp {
    uint32_t size = 0;
    std::vector<float> values;   // size * size, row major
};
Stamp domeStamp(uint32_t size = 64);
// Reads the red channel of an image, mapped to [0, 1]. Returns false with *error set when it cannot be read.
bool loadStamp(const std::string& path, Stamp* out, std::string* error);
// The stamp value at a point (metres, tile space) for a dab. 0 outside the brush.
float stampAt(const Stamp& s, const BrushDab& d, float x, float z);

// Hydraulic erosion settings: particle-based, the common public method (droplets roll downhill, carry sediment, and
// deposit it when they slow down).
struct ErosionSettings {
    uint32_t droplets = 80000;
    uint32_t maxSteps = 40;
    float inertia = 0.05f;      // how much a droplet keeps its direction, 0 to 1
    float capacity = 4.0f;      // sediment carried per unit of speed and water
    float minSlope = 0.01f;     // a floor on capacity, in metres per sample
    float erodeRate = 0.3f;
    float depositRate = 0.3f;
    float evaporate = 0.02f;    // water lost per step
    float gravity = 4.0f;
    uint32_t seed = 1;
};

// Erodes a copy of the heightmap and returns the samples that changed. Heights are used in metres (normalised * scale).
std::vector<HeightEdit> erodeHeightmap(const Heightmap& map, const ErosionSettings& s);

// Paint masks: `channels` bytes per sample, index = sample * channels + channel. A paint dab moves the byte of each
// sample towards `target` (255 paints, 0 erases) by weight * strength. `cell` is the metres between samples.
std::vector<PaintEdit> paintDab(const std::vector<uint8_t>& field, uint32_t size, uint32_t channels, uint32_t channel,
                                const BrushDab& d, uint8_t target, float cell);
void applyPaintEdits(std::vector<uint8_t>& field, const std::vector<PaintEdit>& edits, bool useAfter);

// Folds the edits of a later dab into the record of its stroke. A sample keeps the `before` it had first, and takes
// the latest `after`, so undoing the record returns the sample to where the stroke found it.
void mergeHeightEdits(std::vector<HeightEdit>& acc, const std::vector<HeightEdit>& more);
void mergePaintEdits(std::vector<PaintEdit>& acc, const std::vector<PaintEdit>& more);

// Where a ray meets the heightmap surface, in tile space (metres). Marches the ray, then bisects the crossing.
// Returns false when the ray misses the grid within maxDistance.
bool raycastHeightmap(const Heightmap& map, glm::vec3 origin, glm::vec3 dir, float maxDistance, glm::vec3* hit);

} // namespace ukiyo
