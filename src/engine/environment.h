// Environment lighting: the cubemap, the spherical-harmonic ambient, and the light probes (batch 6).
//
// Until now a surface's ambient was two colours — sky above, ground below — mixed by the normal's height, and the
// RealToon reflection sampled the same two colours along the reflected ray. Real scenes are not two colours: a
// lantern-lit street is bright orange near the lantern and nearly black ten metres away, and a chrome badge should
// show the street, not a gradient. This module gives the renderer three real things, each usable without the others:
//
//   * a Cubemap  — six float faces, either baked from the procedural sky (a CPU twin of shaders/sky.frag) or
//                  converted from an equirectangular panorama PNG. It goes to the GPU as a prefiltered mip chain
//                  for the toon reflection (shaders/toon_extras.glsl).
//   * Sh         — second-order spherical harmonics (9 coefficients per channel), the standard probe representation
//                  (Unity's light probes, Godot 4's irradiance volumes). Baked from the environment plus the scene's
//                  lights, evaluated in the shader as nine dot products.
//   * ProbeVolume— a lattice of probes in a box, trilinearly interpolated like Unity's light probe volumes. One
//                  interpolated probe is packed into the frame block per frame, evaluated at the camera's focus —
//                  the same precedent as scene-light ranking (engine/lights.h): per-object SH would need a
//                  per-draw uniform the push-constant draw path does not have, and the real-time GI pass adds the
//                  screen-space variation on top.
//
// The toon contract is MToon's GI equalisation (VRMC_materials_mtoon 1.0): gi(n) = lerp(rawGi(n), uniformedGi, f)
// with uniformedGi = (rawGi(up) + rawGi(down)) / 2 and f defaulting to 0.9. A scene can ship a fully directional
// ambient and still read as toon. The shader side of that formula lives in shaders/mtoon.frag; the C++ side is
// equalizedGi() here, and the two are tested against each other's conventions in tests/environment_test.cpp.
//
// Everything in this module is pure CPU maths with no GPU or window behind it, so the whole chain — panorama in,
// coefficients and mip chain out — runs in the local battery and on every CI machine.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo::env {

constexpr int kCubeFaces = 6;

// ---------------------------------------------------------------- Cubemaps

// A cubemap in linear RGBA floats, row-major, row 0 at the top. Faces follow the GPU order +X, -X, +Y, -Y, +Z, -Z
// (glTF and Vulkan agree on this; OpenGL's six targets enumerate the same way).
struct Cubemap {
    int size = 0;                                        // texels per face edge
    std::array<std::vector<glm::vec4>, kCubeFaces> face;

    bool empty() const { return size <= 0; }
    // Nearest-texel read (no filtering): the direction must be normalised.
    glm::vec4 sample(const glm::vec3& dir) const;
};

// The world direction the fraction (u, v) of a face looks along, u right and v down from the top row, both in
// [0, 1]. Matches what the GPU's samplerCube will do with the same bytes.
glm::vec3 faceDirection(int faceIndex, float u, float v);

// The inverse: which face and fraction a normalised direction lands on. Returns false for a zero direction.
bool dirToFace(const glm::vec3& dir, int* faceIndex, float* u, float* v);

// Solid angle of the face fraction (u, v)'s texel in steradians. The six faces' texels together cover (almost
// exactly) 4 pi; the environment test sums them to hold the projector honest.
float texelSolidAngle(int size, float u, float v);

// A cubemap converted from an equirectangular panorama (RGBA8, row 0 at the top, conventionally width = 2*height)
// at `size` texels per face. The mapping is the common one: u = atan2(dir.z, dir.x) / 2pi + 0.5, v = acos(dir.y) / pi.
Cubemap fromEquirect(const uint8_t* rgba8, int width, int height, int size);

// Where a direction samples a panorama of the given size, in texel coordinates (unclamped). The inverse of the
// mapping above; exposed so tests and tools can paint a panorama that decodes to chosen colours.
glm::vec2 dirToEquirect(const glm::vec3& dir, int width, int height);

// The procedural sky, on the CPU: a texel-for-texel twin of shaders/sky.frag's colour function (gradient, the warm
// lift toward the sun, the disc and halo), so the environment ambient and the reflections come from the same sky
// the screen draws. Stars are left out on purpose — a sub-texel twinkling field averages to nothing in an
// irradiance bake — and clouds participate with their cover fraction. Radiance above `clamp` (the sun disc is HDR)
// is clamped so an RGBA8 mip chain can hold the cubemap without wrapping.
Cubemap bakeSkyCubemap(const glm::vec3& zenith, const glm::vec3& horizon, const glm::vec3& sunDir, float sunDisc,
                       float cloudCover, int size);

// One sky texel, the colour the sky shader would draw towards `rd` with these settings — exposed so a tool or a test
// can ask the CPU sky without baking all six faces.
glm::vec3 skyRadiance(const glm::vec3& zenith, const glm::vec3& horizon, const glm::vec3& sunDir, float sunDisc,
                      float cloudCover, const glm::vec3& rd);

// The radiance clamp from bakeSkyCubemap, shared by the packer: environment bytes are RGBA8.
constexpr float kEnvRadianceClamp = 3.0f;

// Packs a cubemap for the RHI: RGBA8 bytes, mip levels clamped to full-size..1, face-major (face 0's whole chain,
// then face 1's). `prefiltered` is the full prefiltered chain from prefilteredChain (entry 0, the base, is not read
// again); when it is empty the chain is a plain box-filter downsample, which a scene that only wants the ambient can
// live with.
std::vector<std::vector<uint8_t>> packMipChain(const Cubemap& base, const std::vector<Cubemap>& prefiltered);

// ------------------------------------------------------------ Spherical harmonics

// Second-order SH, 9 coefficients per channel, index order L00, L1-1(y), L10(z), L11(x), L2-2(xy), L2-1(yz),
// L20(zz), L21(xz), L22(xx-yy) — the layout Unity documents for light probe data. Coefficients hold irradiance
// (colour units, so a uniform white environment evaluates to 1), not radiance.
struct Sh {
    float c[3][9] = {};

    static Sh zero();
    void addScaled(const Sh& other, float scale);
    static Sh lerp(const Sh& a, const Sh& b, float t);
};

// The real SH basis at a normalised direction, with the standard normalisations (Y00 = 0.282095, ...). Y is written
// into `out[9]` in the coefficient order above.
void shBasis(const glm::vec3& n, float out[9]);

// Projects `radiance(dir)` (irradiance-flavoured: pass what a surface facing `dir` should receive) onto the basis
// with a Fibonacci-sphere sample of `samples` directions. `samples` below 64 is clamped up: fewer cannot represent
// L2.
Sh projectIrradiance(const std::function<glm::vec3(const glm::vec3&)>& radiance, int samples = 512);

// The irradiance coefficients of a radiance cubemap: project every texel with its solid angle, then scale the bands
// by (1, 2/3, 1/4) — the Ramamoorthi cosine-convolution constants over pi. A uniform white cubemap evaluates to 1.
Sh irradianceFromCubemap(const Cubemap& env);

// Evaluates the probe at a normalised direction.
glm::vec3 evalSh(const Sh& sh, const glm::vec3& n);

// The MToon GI equalisation: gi(n) = lerp(rawGi(n), (rawGi(up) + rawGi(down)) / 2, equalization).
glm::vec3 equalizedGi(const Sh& sh, const glm::vec3& n, float equalization);

// A light in bake terms: the SceneLight fields the bake reads, without pulling engine/lights.h (which drags
// gpu_types.h in) into every test.
struct EnvLight {
    int type = 1;                 // 0 directional, 1 point, 2 spot (SceneLight's numbering)
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f, 0.9f, 0.8f};
    float intensity = 2.0f;
    float radius = 8.0f;
    float spotAngleDeg = 50.0f;
    float spotSoftness = 0.25f;
    bool enabled = true;
};

// One scene light's contribution to a probe at `p`: a directional light is max(0, n . -direction) of its colour; a
// point or spot light is its colour times the same windowed inverse-square the toon shader draws
// (window^2 / (1 + d^2), the reach clipping it to nothing), as max(0, n . l) with the spot's cone factor. The bake
// deliberately reuses the shader's attenuation so baked ambient and direct light agree at the same spot.
void addLightToSh(Sh* sh, const glm::vec3& p, const EnvLight& light);

// ------------------------------------------------------------------ Probes

// A lattice of probes filling a world-space box: divisions counts probes per axis, at least 1 (the corners). Probe
// (i, j, k) sits at center + halfExtent * ((i / (divX-1)) * 2 - 1) per axis, so divX=1 pins that axis to the centre.
struct ProbeVolume {
    glm::vec3 center{0.0f};
    glm::vec3 halfExtent{4.0f, 2.0f, 4.0f};
    int divX = 2, divY = 1, divZ = 2;
    std::vector<Sh> probes;      // divX * divY * divZ, x fastest, then y, then z

    glm::vec3 probePosition(int i, int j, int k) const;
    // Trilinear over the lattice; a point outside the box clamps to the box edge, like Unity's volumes.
    Sh sample(const glm::vec3& p) const;
};

// Bakes one volume: every probe starts from `envIrradiance` (the environment's coefficients, projected once by the
// caller) and accumulates every enabled light that can reach it.
void bakeVolume(ProbeVolume* volume, const Sh& envIrradiance, const std::vector<EnvLight>& lights);

// ------------------------------------------------------------------ Filter

// The prefiltered environment for the specular mip chain: for every texel, an average of the environment over a GGX
// lobe of roughness `roughness` around the mirror direction (Karis' split-sum prefilter, low-discrepancy samples,
// weights N.L, normalised — energy-conserving for a uniform environment, which the raw filter is not).
Cubemap prefilter(const Cubemap& env, float roughness, int samples = 64);

// The whole mip chain a reflection needs: level 0 is `base`, level l is prefilter(base, l / (levels-1)). Levels are
// chosen so the last is 1 texel. A scene with no reflections still uses level 0.
std::vector<Cubemap> prefilteredChain(const Cubemap& base);

// ------------------------------------------------------------------ Pack

// The frame block fields the renderer fills (shaders/frame.glsl, appended at the end like every addition):
// envSH[i] = (r, g, b, unused) of coefficient i; envParams = x ambient weight (0 = the shader keeps the
// hemisphere path), y cubemap present, z intensity, w mip count.
struct FrameEnv {
    glm::vec4 sh[9];
    glm::vec4 params{0.0f};
};

// Interpolates the volumes at `focus` (later volumes win where boxes overlap; with none, a single whole-scene
// probe is used), applies `intensity`, and packs.
FrameEnv packFrameEnv(const std::vector<ProbeVolume>& volumes, const Sh& wholeScene, const glm::vec3& focus,
                      float intensity, bool cubePresent, int mipCount);

// ------------------------------------------------------------- Scene data

// What a scene's "environment" block parses into (engine/scene_runtime.cpp). Defaults mean "no environment": the
// renderer keeps the hemisphere path and nothing changes for existing scenes.
struct Settings {
    bool enabled = false;
    bool fromSky = true;              // false: `panorama` names a project-relative RGBA8 PNG, width 2 * height
    std::string panorama;
    float intensity = 1.0f;           // ambient scale; the shader's own 0.55 stays on top
    float reflectionStrength = -1.0f; // < 0 keeps the project's toonC.w
    int cubeSize = 64;                // texels per face; 32 is plenty for ambient, 64 keeps reflections crisp
    struct VolumeDef {
        glm::vec3 center{0.0f};
        glm::vec3 size{8.0f, 4.0f, 8.0f};   // full extents; divisions per axis, clamped to >= 1
        int divX = 2, divY = 1, divZ = 2;
    };
    std::vector<VolumeDef> volumes;
};

} // namespace ukiyo::env
