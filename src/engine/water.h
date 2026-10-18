#pragma once
// Water surface model (item 16). The CPU functions here match shaders/water.vert term by term, so the drawn
// surface and the buoyancy query (item 17) agree. Waves are a sum of four Gerstner waves, written from the public
// formula (Tessendorf, GPU Gems ch. 1). No third-party water code.
#include <glm/glm.hpp>

namespace ukiyo {

struct WaterWave {
    glm::vec2 dir{1.0f, 0.0f};   // horizontal travel direction (x, z); normalised on use, zero falls back to +x
    float wavelength = 8.0f;     // metres
    float amplitude = 0.12f;     // metres
    float speed = 1.0f;          // phase-speed multiplier on the deep-water dispersion
    float steepness = 0.35f;     // 0..1; the sum of the four waves stays below 1, so the surface cannot loop
};

// An authored rectangle of water. The surface is flat at `level` when the waves are off.
struct WaterVolume {
    glm::vec2 centre{0.0f};      // world (x, z)
    glm::vec2 half{10.0f};       // half extent (x, z) in metres
    float level = 0.0f;          // world y of the rest surface
    WaterWave waves[4];
    glm::vec3 shallow{0.10f, 0.42f, 0.50f};   // scattering colour near the surface
    glm::vec3 deep{0.01f, 0.10f, 0.18f};      // scattering colour in deep water
    float absorption = 0.35f;    // per metre, scales the Beer-style colour falloff
    float foamWidth = 0.40f;     // metres of scene depth under the surface that get edge foam
    float refraction = 0.04f;    // screen-space refraction offset scale
    float density = 1000.0f;     // kg/m^3 of the water. A body floats when its own density is below this (item 17)
};

struct WaterSurface {
    glm::vec3 pos;      // world position on the surface
    glm::vec3 normal;   // unit, pointing up for calm water
    float crest = 0.0f; // 0..1, high where the waves peak (crest foam)
};

// Rest-point displacement. For a flat rest point (x0, z0) at time t, returns the offset of the displaced point
// (x: horizontal, y: height above level, z: horizontal), and writes the surface normal.
glm::vec3 waterOffset(const WaterVolume& v, float x0, float z0, float t, glm::vec3* normalOut);

// Surface at world (x, z): solves the rest point whose displaced position lands on (x, z) (a few fixed-point
// steps, enough for steepness below 1). Vertices of the rendered grid are rest points, so the picture and this
// query agree.
WaterSurface waterSurface(const WaterVolume& v, float x, float z, float t);

// Height and normal at world (x, z). Buoyancy reads these.
float waterHeight(const WaterVolume& v, float x, float z, float t);
glm::vec3 waterNormal(const WaterVolume& v, float x, float z, float t);

// Whether a world point lies inside the rectangle (horizontal test only).
bool waterContainsXZ(const WaterVolume& v, float x, float z);

} // namespace ukiyo
