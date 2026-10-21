// Scene lights: the list the renderer shades with, and the packing into the frame block.
//
// Until now a scene had exactly one directional sun plus two coloured fill points baked into `renderer.cpp` at fixed
// coordinates. A lantern row, a neon sign or a muzzle flash could not exist. This module holds authored lights, picks
// the ones that matter for a camera, and packs them into the std140 arrays that `shaders/mtoon.frag` and
// `shaders/frame.glsl` read.
//
// The selection is deliberately simple and deterministic: a light is scored by the irradiance it can plausibly add at
// the camera's focus (intensity times radius squared over distance squared), lights entirely behind the camera are
// penalised, and ties keep author order so a scene renders identically every frame. Culling per object would be better,
// but it would need the light list in the vertex stage and the RHI has no way to pass a per-draw light mask yet; this
// gets the visible result without that machinery.
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/gpu_types.h"

namespace ukiyo {

enum class LightType : uint8_t { Directional = 0, Point = 1, Spot = 2 };

struct SceneLight {
    LightType type = LightType::Point;
    glm::vec3 position{0.0f, 2.0f, 0.0f};      // point and spot
    glm::vec3 direction{0.0f, -1.0f, 0.0f};    // spot: the direction the cone points
    glm::vec3 color{1.0f, 0.9f, 0.8f};
    float intensity = 2.0f;
    float radius = 8.0f;                        // reach in metres; 0 means "unlimited", clamped on pack
    float spotAngleDeg = 50.0f;                 // full cone angle
    float spotSoftness = 0.25f;                 // fraction of the cone used for the soft edge
    bool enabled = true;
};

// What the shader sees per light. Four vec4s, std140-aligned.
struct GpuLight {
    glm::vec4 posRadius;      // xyz position, w radius
    glm::vec4 dirCosOuter;    // xyz direction (point/spot), w cos(outer half-angle)
    glm::vec4 colorIntensity; // rgb colour, w intensity
    glm::vec4 params;         // x type (0 dir, 1 point, 2 spot), y cos(inner half-angle), z reserved, w reserved
};
static_assert(sizeof(GpuLight) == 64, "GpuLight crosses the C++/GLSL boundary as four vec4s");

// How many lights the frame block carries. The sun is separate, so this is in addition to it.
constexpr int kMaxSceneLights = 16;

// Distance from the focus point at which a light's score reaches zero, used to drop lights that cannot contribute.
float lightScore(const SceneLight& light, const glm::vec3& focus, const glm::vec3& forward);

// Picks at most `max` enabled lights for a camera and writes them packed, brightest first. Returns the count.
// Directional lights are always kept first (they are key lights by definition) up to `max`; the rest are ranked.
int packSceneLights(const std::vector<SceneLight>& lights, const glm::vec3& focus, const glm::vec3& forward, int max,
                    GpuLight* out);

// Packs one light unconditionally, for the editor's preview and for tests.
GpuLight packLight(const SceneLight& light);

// The cosine of half the given angle in degrees, clamped to a sane range.
float cosHalfAngle(float deg);

} // namespace ukiyo
