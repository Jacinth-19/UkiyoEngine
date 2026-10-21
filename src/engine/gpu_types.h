// CPU-side mirrors of the uniform blocks declared in shaders/*.glsl.
// std140 rules: every member is a vec4 or mat4, so offsets are already aligned.
#pragma once

#include <glm/glm.hpp>

namespace ukiyo {

// What the shader sees per scene light. Four vec4s: xyz position, w radius; xyz direction, w cos(outer half-angle);
// rgb colour, w intensity; x type (0 directional, 1 point, 2 spot), y cos(inner half-angle).
struct GpuLight {
    glm::vec4 posRadius{0.0f};
    glm::vec4 dirCosOuter{0.0f};
    glm::vec4 colorIntensity{0.0f};
    glm::vec4 params{0.0f};
};
static_assert(sizeof(GpuLight) == 64, "GpuLight crosses the C++/GLSL boundary as four vec4s");

// Scene lights in one frame block. The sun is separate (it needs the cascade matrices), so these are point and spot
// lights only; anything more is dropped by the selection in lights.h.
constexpr int kFrameLights = 8;

struct FrameUBO {                 // set 0, binding 0 (frame.glsl)
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::mat4 shadowMatrix[3];
    glm::vec4 camPos;
    glm::vec4 sunDir;
    glm::vec4 sunColor;
    glm::vec4 skyColor;
    glm::vec4 groundColor;
    glm::vec4 cascadeSplits;
    glm::vec4 shadowParams;
    glm::vec4 screenParams;
    glm::vec4 pointPos[4];     // the two stylistic fills a scene without lights gets, as before
    glm::vec4 pointColor[4];
    glm::vec4 lightParams;     // x = scene light count (0 = use the fills), y = 1 when the sun comes from a scene light
    GpuLight lights[kFrameLights];
    glm::vec4 toonA;     // RealToon: x hair highlight, y hair shift, z hair exponent, w self shadow on
    glm::vec4 toonB;     // RealToon: x self threshold, y self hardness, z gloss, w gloss power
    glm::vec4 toonC;     // x cel band on (1 = three-step ramp in mtoon; 0 = off), y normal map strength, z material outline noise, w reflection strength
    glm::vec4 toonD;     // xy head forward (x, z) for face shadow, z face shadow on, w smear strength
    glm::vec4 toonE;     // x soft light, y light sweep strength, z sweep speed (cycles/s)
    glm::vec4 toonF;     // camera dither fade: x fully dithered out distance, y fully visible distance (off when y <= x)
    glm::vec4 toonG;     // x stylised PBR strength, y eye flatten, z eye parallax depth (UV), w eye matcap strength
    glm::vec4 toonH;     // x eye highlight map strength
    glm::vec4 toonI;     // occlusion target: xyz world point (the character), w radius (0 = off)
    glm::vec4 waterArea; // water: x centre x, y level, z centre z, w 1 = a volume is on
    glm::vec4 waterHalf; // water: x half x, y foam width, z half z, w refraction offset scale
    glm::vec4 waterShallow; // water: rgb shallow colour, w absorption
    glm::vec4 waterDeep;    // water: rgb deep colour, w 1 = camera is under the volume
    glm::vec4 waterWaveD[4]; // water: per wave xy unit direction, z wavelength, w amplitude
    glm::vec4 waterWaveS[4]; // water: per wave x speed multiplier, y steepness
};

struct MaterialUBO {              // set 1, binding 8 (material.glsl)
    glm::vec4 baseColor{1.0f};
    glm::vec4 shadeColor{0.97f, 0.81f, 0.86f, 1.0f};
    glm::vec4 emissive{0.0f};
    glm::vec4 toon{0.0f, 0.9f, 0.9f, 1.0f};   // shift, toony, giEq, rimLightingMix
    glm::vec4 rim{0.0f, 0.0f, 0.0f, 5.0f};    // rgb, fresnel power
    glm::vec4 rimLift{0.0f};
    glm::vec4 outline{0.0f};                  // rgb, width
    glm::vec4 flags{0.0f};                    // alpha mode, cutoff, doubleSided, unlit
    glm::vec4 flags2{0.0f};                   // outline mode
    glm::vec4 toon2{0.0f};                    // RealToon: x gloss texture present, y normal texture present, z face SDF material, w eye material
    // Per-character effects (set per character by setCharacterFx / setHairFace / setOccludable in model.h).
    glm::vec4 fx0{0.0f};                      // x dissolve cut (0 = off), y dissolve glow width, z darken, w get-hit flash
    glm::vec4 fx1{0.0f};                      // rgb dissolve glow colour, w upgrade pulse amplitude
    glm::vec4 fx2{0.0f};                      // xyz hair face anchor (world), w hair dither radius (0 = off)
    glm::vec4 fx3{0.0f};                      // rgb get-hit colour, w 1 = excluded from occlusion (the target itself)
    glm::vec4 fx4{0.0f};                      // rgb upgrade pulse colour
    glm::vec4 leaf{0.0f};                     // x leaf transmission strength (0 = off), y view falloff power
};

struct JointUBO {                 // set 2, binding 9 (skin.glsl)
    glm::mat4 joints[128];
};

struct MotionUBO {                // set 3, binding 10 (velocity.vert): last frame's transforms
    glm::mat4 joints[128];        // joint palette (skinned), or the previous model in [0] (rigid)
    glm::mat4 prevViewProj;       // previous frame, unjittered
};

struct PushConstants {            // <= 128 bytes, see common.glsl
    glm::mat4 model;
    glm::vec4 p0;
    glm::vec4 p1;
    glm::vec4 p2;       // x: distance fade, the fraction dithered out (colour passes)
    glm::vec4 p3;       // effects pass: SplitScreen line tints and colour, packed (see renderer.cpp)
};
static_assert(sizeof(PushConstants) <= 128, "push constant budget");

} // namespace ukiyo
