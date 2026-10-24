// Environment lighting (docs/IBL.md): cubemap conventions, spherical-harmonic projection and evaluation, the MToon
// GI equalisation, the GGX prefilter, probe-volume interpolation, and the frame-block packing shared with
// shaders/frame.glsl.
#include "engine/environment.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using namespace ukiyo;
using namespace ukiyo::env;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what, const char* detail = nullptr) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail ? " -- " : "", detail ? detail : "");
    }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// A white environment at `size` texels per face.
Cubemap whiteCube(int size) {
    Cubemap c;
    c.size = size;
    for (int f = 0; f < kCubeFaces; ++f) c.face[f].assign(size_t(size) * size, glm::vec4(1.0f));
    return c;
}
} // namespace

int main() {
    // --- Cubemap conventions: the layout the GPU will see.
    {
        // Every face direction is unit length, and the six faces' texels together cover the sphere.
        bool unit = true;
        for (int f = 0; f < kCubeFaces; ++f)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const glm::vec3 d = faceDirection(f, (x + 0.5f) / 8.0f, (y + 0.5f) / 8.0f);
                    unit = unit && near(glm::length(d), 1.0f, 1e-5f);
                }
        check(unit, "face directions are unit length");
        float solid = 0.0f;
        for (int f = 0; f < kCubeFaces; ++f)
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                    solid += texelSolidAngle(32, (x + 0.5f) / 32.0f, (y + 0.5f) / 32.0f);
        check(near(solid, 4.0f * 3.14159265f, 0.01f), "the six faces cover 4 pi steradians");

        // Round-trip: a direction lands on the face texel it came from.
        bool round = true;
        for (int f = 0; f < kCubeFaces; ++f)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    const glm::vec3 d = faceDirection(f, (x + 0.5f) / 16.0f, (y + 0.5f) / 16.0f);
                    int rf = -1; float ru = 0.0f, rv = 0.0f;
                    round = round && dirToFace(d, &rf, &ru, &rv);
                    round = round && rf == f &&
                            std::fabs(ru - (x + 0.5f) / 16.0f) < 1e-4f && std::fabs(rv - (y + 0.5f) / 16.0f) < 1e-4f;
                }
        check(round, "dirToFace inverts faceDirection");
        check(near3(faceDirection(2, 0.5f, 0.5f), glm::vec3(0, 1, 0), 1e-5f), "+Y face centre looks straight up");
        check(near3(faceDirection(4, 0.5f, 0.5f), glm::vec3(0, 0, 1), 1e-5f), "+Z face centre looks forward");
    }

    // --- Equirect conversion: paint latitude bands, read them back through the cube.
    {
        const int w = 128, h = 64;
        std::vector<uint8_t> pano(size_t(w) * h * 4, 0);
        for (int y = 0; y < h; ++y) {
            uint8_t c[4] = {0, 0, 0, 255};
            if (y < 16) { c[1] = 255; }                     // the top quarter of the panorama is up: green
            else if (y >= 48) { c[2] = 255; }               // the bottom quarter is down: blue
            else { c[0] = 255; }                            // the equator band: red
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < 4; ++k) pano[(size_t(y) * w + x) * 4 + k] = c[k];
        }
        const Cubemap c = fromEquirect(pano.data(), w, h, 32);
        check(c.size == 32, "fromEquirect honours the size");
        check(near3(glm::vec3(c.sample(glm::vec3(0, 1, 0))), glm::vec3(0, 1, 0), 0.02f), "up reads the green band");
        check(near3(glm::vec3(c.sample(glm::vec3(0, -1, 0))), glm::vec3(0, 0, 1), 0.02f), "down reads the blue band");
        check(near3(glm::vec3(c.sample(glm::vec3(1, 0, 0))), glm::vec3(1, 0, 0), 0.02f), "the equator reads red");
    }

    // --- SH: a uniform environment is constant, in colour units.
    {
        const Sh white = irradianceFromCubemap(whiteCube(16));
        const glm::vec3 up = evalSh(white, {0, 1, 0});
        const glm::vec3 down = evalSh(white, {0, -1, 0});
        const glm::vec3 side = evalSh(white, {1, 0, 0});
        check(near3(up, glm::vec3(1.0f), 0.02f) && near3(down, glm::vec3(1.0f), 0.02f) &&
                  near3(side, glm::vec3(1.0f), 0.02f),
              "a white cubemap evaluates to white everywhere");
    }

    // --- SH: a directional light projects to a known irradiance function.
    {
        // max(0, cos) of a unit colour along +z: the DC term is exactly pi * Y00 * 1/pi = 1/4.
        EnvLight sun;
        sun.type = 0;
        sun.direction = {0, 0, -1};   // shines towards -z, so it lights surfaces facing +z
        sun.color = {1, 1, 1};
        sun.intensity = 1.0f;
        Sh sh = Sh::zero();
        addLightToSh(&sh, {0, 0, 0}, sun);
        check(near(sh.c[0][0], 0.8862f, 0.02f), "a unit sun's DC irradiance is pi * Y00");
        const float at = evalSh(sh, {0, 0, 1}).x;      // facing the sun
        const float away = evalSh(sh, {0, 0, -1}).x;   // facing away
        const float side = evalSh(sh, {1, 0, 0}).x;    // a quarter turn
        check(at > 0.9f && at < 1.2f, "a unit sun lights a facing surface at about 1", std::to_string(at).c_str());
        check(away > -0.15f && away < 0.15f, "a unit sun barely wraps to the far side",
              std::to_string(away).c_str());
        check(side > -0.05f && side < 0.3f && side < at * 0.5f, "a quarter turn is far dimmer than facing",
              std::to_string(side).c_str());
    }

    // --- MToon GI equalisation (VRMC_materials_mtoon: gi(n) = lerp(rawGi(n), uniformedGi, f)).
    {
        EnvLight sun;
        sun.type = 0;
        sun.direction = {0, -1, 0};   // straight down
        Sh sh = Sh::zero();
        addLightToSh(&sh, {0, 0, 0}, sun);
        const glm::vec3 top = equalizedGi(sh, {0, 1, 0}, 0.0f);
        const glm::vec3 sideEq = equalizedGi(sh, {1, 0, 0}, 0.0f);
        check(top.x > sideEq.x * 2.0f, "raw GI follows the normal", (std::to_string(top.x) + " vs " +
                                                                    std::to_string(sideEq.x)).c_str());
        const glm::vec3 eq = equalizedGi(sh, {0, 1, 0}, 1.0f);
        const glm::vec3 eq2 = equalizedGi(sh, {0, 0, 1}, 1.0f);
        check(near3(eq, eq2, 1e-5f), "equalization 1 is direction-independent");
        const glm::vec3 uniformed = (evalSh(sh, {0, 1, 0}) + evalSh(sh, {0, -1, 0})) * 0.5f;
        check(near3(eq, uniformed, 1e-5f), "equalized GI is the up/down mean, as the MToon spec writes it");
    }

    // --- Probes: a point light reaches probes near it and fades with the shader's own window.
    {
        EnvLight lamp;
        lamp.type = 1;
        lamp.position = {2, 1, 0};
        lamp.color = {1.0f, 0.6f, 0.2f};
        lamp.intensity = 4.0f;
        lamp.radius = 8.0f;
        Sh nearL = Sh::zero(), farL = Sh::zero();
        addLightToSh(&nearL, {2.5f, 1.0f, 0.0f}, lamp);
        addLightToSh(&farL, {2.5f, 1.0f, 9.0f}, lamp);   // 9 m from the lamp: past its 8 m reach
        check(evalSh(nearL, {0, 0, 1}).x > evalSh(farL, {0, 0, 1}).x * 10.0f, "a point light falls off with distance");
        check(near(evalSh(farL, {0, 0, 1}).x, 0.0f, 1e-6f), "a light's reach clips to nothing");
    }

    // --- Probe volumes: the lattice, trilinear interpolation, clamping.
    {
        ProbeVolume v;
        v.center = {0, 2, 0};
        v.halfExtent = {4, 2, 4};
        v.divX = 3; v.divY = 2; v.divZ = 3;
        bakeVolume(&v, Sh::zero(), {});
        check(v.probes.size() == 18, "the lattice holds divX*divY*divZ probes");
        check(near3(v.probePosition(0, 0, 0), glm::vec3(-4, 0, -4), 1e-5f), "corner probes sit on the box");
        check(near3(v.probePosition(2, 1, 2), glm::vec3(4, 4, 4), 1e-5f), "the last probe is the far corner");
        check(near3(v.probePosition(1, 0, 1), glm::vec3(0, 0, 0), 1e-5f), "the centre axis pins to the middle");

        // Paint the probes by x position and check the interpolation slides between them.
        for (size_t i = 0; i < v.probes.size(); ++i) {
            const int ix = int(i % 3);
            for (int ch = 0; ch < 3; ++ch) v.probes[i].c[ch][0] = float(ix);   // DC = the colour
        }
        const Sh left = v.sample({-4, 2, 0});
        const Sh right = v.sample({4, 2, 0});
        const Sh mid = v.sample({0, 2, 0});
        check(near(left.c[0][0], 0.0f, 1e-4f) && near(right.c[0][0], 2.0f, 1e-4f), "volume sampling is exact at probes");
        check(near(mid.c[0][0], 1.0f, 1e-4f), "the box centre mixes x neighbours evenly");
        const Sh quarter = v.sample({-2, 2, 0});
        check(near(quarter.c[0][0], 0.5f, 1e-4f), "a quarter along x is half the step");
        const Sh out = v.sample({9, 2, 0});
        check(near(out.c[0][0], 2.0f, 1e-4f), "outside the box clamps to the edge");
    }

    // --- Baking: probes carry the environment plus the lights that reach them.
    {
        const Sh white = irradianceFromCubemap(whiteCube(8));
        ProbeVolume v;
        v.divX = 2; v.divY = 1; v.divZ = 2;
        std::vector<EnvLight> lights;
        EnvLight lamp;
        lamp.type = 1;
        lamp.position = {-3, 1, -3};
        lamp.intensity = 30.0f;
        lamp.radius = 12.0f;
        lights.push_back(lamp);
        bakeVolume(&v, white, lights);
        const Sh nearLamp = v.sample({-2, 1, -2});
        const Sh farSide = v.sample({2, 1, 2});
        check(near(evalSh(white, {0, 1, 0}).x, 1.0f, 0.02f), "the baked environment alone is white");
        check(evalSh(nearLamp, {0, 1, 0}).x > evalSh(farSide, {0, 1, 0}).x, "the lamp warms the probes near it");
        check(evalSh(nearLamp, {0, 1, 0}).x > 1.2f, "a nearby lamp lifts the ambient above the environment");
    }

    // --- Prefilter: energy-holding on white, sharp at zero roughness, blurrier as roughness grows.
    {
        // A half-black half-white sky: the top hemisphere white, the bottom black. Roughness must only ever mix
        // towards the average, never overshoot.
        Cubemap sky;
        sky.size = 32;
        for (int f = 0; f < kCubeFaces; ++f) sky.face[f].assign(size_t(32) * 32, glm::vec4(0.0f));
        for (int f = 0; f < kCubeFaces; ++f)
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    const glm::vec3 d = faceDirection(f, (x + 0.5f) / 32.0f, (y + 0.5f) / 32.0f);
                    sky.face[f][size_t(y * 32 + x)] = glm::vec4(d.y > 0.0f ? glm::vec3(1.0f) : glm::vec3(0.0f), 1.0f);
                }
        const Cubemap white = whiteCube(16);
        const Cubemap r0 = prefilter(white, 0.0f);
        const Cubemap r1 = prefilter(white, 1.0f);
        check(near(r0.sample({0, 1, 0}).x, 1.0f, 0.02f) && near(r1.sample({0, 1, 0}).x, 1.0f, 0.02f),
              "a white environment prefilters to white at any roughness");
        // Sharpness: on the split sky, roughness 0 keeps a white pole white, and roughness 1 mixes hemispheres.
        const Cubemap blur = prefilter(sky, 1.0f);
        const float sharpAtPole = prefilter(sky, 0.0f).sample({0, 1, 0}).x;
        const float blurAtPole = blur.sample({0, 1, 0}).x;
        check(sharpAtPole > 0.95f, "roughness 0 keeps a white pole white");
        check(blurAtPole > 0.45f && blurAtPole < 1.0f, "roughness 1 mixes the hemispheres",
              std::to_string(blurAtPole).c_str());
        // The chain: levels from size down to 1, matching the mip count the RHI allocates for the same size.
        const std::vector<Cubemap> chain = prefilteredChain(sky);
        check(chain.size() == 6, "a 32-texel cubemap prefilters into the full mip count (32..1)");
        check(chain.back().size == 1, "the last level is a single texel");
        check(chain[1].size == 16 && chain[4].size == 2, "the levels halve");
    }

    // --- Packing: the mip chain is face-major RGBA8 bytes, ready for the RHI.
    {
        const std::vector<Cubemap> chain = prefilteredChain(whiteCube(8));
        const std::vector<std::vector<uint8_t>> bytes = packMipChain(whiteCube(8), chain);
        check(bytes.size() == 4, "an 8-texel cubemap packs four levels");
        check(bytes[0].size() == size_t(6) * 8 * 8 * 4, "level 0 is six 8x8 RGBA faces");
        check(bytes[3].size() == size_t(6) * 1 * 1 * 4, "level 3 is six single texels",
              std::to_string(bytes[3].size()).c_str());
        bool white8 = true;
        for (size_t i = 0; i < bytes[0].size(); i += 4) white8 = white8 && bytes[0][i] >= 250;
        check(white8, "white packs to white bytes");
        // The plain-downsample fallback fills a chain without a prefilter.
        const std::vector<std::vector<uint8_t>> plain = packMipChain(whiteCube(8), {});
        check(plain.size() == 4 && plain[3].size() == size_t(6) * 4, "the fallback chain is complete");
    }

    // --- Frame packing: the slots shaders/frame.glsl reads.
    {
        const Sh sh = irradianceFromCubemap(whiteCube(8));
        const FrameEnv env = packFrameEnv({}, sh, {100.0f, 0.0f, -50.0f}, 0.8f, true, 5);
        check(near(env.sh[0].r, sh.c[0][0], 1e-6f) && near(env.sh[8].g, sh.c[1][8], 1e-6f) &&
                  near(env.sh[4].b, sh.c[2][4], 1e-6f),
              "coefficients pack channel-major per slot");
        check(near(env.params.x, 1.0f) && near(env.params.y, 1.0f) && near(env.params.z, 0.8f) &&
                  near(env.params.w, 5.0f),
              "envParams carries weight, presence, intensity and mip count");
        // An empty volume set falls back to the whole-scene probe; intensity 0 switches the ambient off.
        const FrameEnv off = packFrameEnv({}, Sh::zero(), {0, 0, 0}, 0.0f, false, 1);
        check(near(off.params.x, 0.0f) && near(off.params.y, 0.0f), "intensity 0 disables the ambient path");
    }

    // --- The sky bake matches the ambient colours it came from.
    {
        const glm::vec3 zenith(0.20f, 0.46f, 0.92f), horizon(0.80f, 0.91f, 1.00f);
        const Cubemap sky = bakeSkyCubemap(zenith, horizon, {0.3f, 0.9f, 0.2f}, 0.0f, 0.0f, 16);
        check(near3(glm::vec3(sky.sample({0, 1, 0})), zenith, 0.03f), "the baked sky is the zenith colour overhead");
        const glm::vec3 hz = sky.sample({0.6f, 0.05f, -0.7f});
        check(hz.g > hz.b - 0.2f && hz.g > 0.6f, "the horizon stays light");
        const glm::vec3 ground = sky.sample({0, -1, 0});
        check(near3(ground, horizon, 0.02f), "below the horizon the gradient holds the horizon colour");
    }

    // The frame block additions are std140-safe: vec4s at the end, 16-byte aligned.
    static_assert(sizeof(FrameEnv) == sizeof(glm::vec4) * 10, "FrameEnv is nine coefficients plus params");

    std::printf("environment_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
