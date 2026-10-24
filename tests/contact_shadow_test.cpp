// Screen-space contact shadows (docs/CONTACT_SHADOWS.md): the march in src/engine/contact_shadow.h, driven against an
// analytic scene.
//
// The scene is a floor and a unit cube standing on it, under a low sun, seen by a perspective camera. The depth buffer
// the march reads is produced by ray-casting that scene, so the answers are exact and the test is about the algorithm
// rather than about sampling. What matters, and what is checked:
//
//   * a point in the cube's contact region — in shadow, within the march's reach — is darkened;
//   * a lit point is never darkened (a false contact shadow is worse than a missing one);
//   * a shadowed point beyond the march's reach is left alone, because that is the shadow map's job;
//   * the strength, the reach and the thickness move the result in the direction they claim to;
//   * a surface facing away from the sun is left to the material's own N.L;
//   * every result is finite and inside [1 - strength, 1], with the jitter changing nothing but the phase.

#include "engine/contact_shadow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

using namespace ukiyo;

namespace {

int gChecks = 0;
int gFailures = 0;

void checkTrue(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

void checkNear(double got, double want, double tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol && std::isfinite(got);
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %-64s got %.5f want %.5f (tol %.5f)\n", what.c_str(), got, want, tol);
    }
}

// ---- the scene -------------------------------------------------------------------------------
//
//   floor: y = 0, albedo 0.5
//   cube : x, z in [-0.5, 0.5], y in [0, 1]
//   sun  : low and from -x, so the cube's shadow falls towards +x
//
constexpr float kCubeHalf = 0.5f;
constexpr float kCubeHeight = 1.0f;
const glm::vec3 kSunDir = glm::normalize(glm::vec3(-0.55f, 0.78f, 0.30f));   // towards the sun

struct Hit {
    bool hit = false;
    float t = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
};

// Ray vs the cube's six faces, as slabs. `tmin` is the near hit.
bool slabRay(const glm::vec3& o, const glm::vec3& d, float* tNear) {
    const glm::vec3 lo(-kCubeHalf, 0.0f, -kCubeHalf);
    const glm::vec3 hi(kCubeHalf, kCubeHeight, kCubeHalf);
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        float a = (lo[i] - o[i]) / d[i];
        float b = (hi[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    *tNear = t0;
    return t0 > 1e-4f;
}

glm::vec3 boxNormal(const glm::vec3& p) {
    // Whichever face the point is on, to within a millimetre.
    const float eps = 1e-3f;
    if (std::fabs(p.x - kCubeHalf) < eps) return {1, 0, 0};
    if (std::fabs(p.x + kCubeHalf) < eps) return {-1, 0, 0};
    if (std::fabs(p.z - kCubeHalf) < eps) return {0, 0, 1};
    if (std::fabs(p.z + kCubeHalf) < eps) return {0, 0, -1};
    if (std::fabs(p.y - kCubeHeight) < eps) return {0, 1, 0};
    return {0, -1, 0};
}

Hit trace(const glm::vec3& o, const glm::vec3& d) {
    Hit h;
    if (std::fabs(d.y) > 1e-6f) {   // floor
        const float t = -o.y / d.y;
        if (t > 1e-4f) {
            h.hit = true;
            h.t = t;
            h.position = o + d * t;
            h.normal = {0, 1, 0};
        }
    }
    float tc = 0.0f;
    if (slabRay(o, d, &tc) && tc < h.t) {
        h.hit = true;
        h.t = tc;
        h.position = o + d * tc;
        h.normal = boxNormal(h.position);
    }
    return h;
}

// True when the sun is blocked by the cube from `p`.
bool inShadow(const glm::vec3& p) {
    float t = 0.0f;
    return slabRay(p + glm::vec3(0, 1e-4f, 0) + kSunDir * 1e-3f, kSunDir, &t);
}

struct Scene {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec3 eye{2.6f, 1.9f, 3.0f};

    Scene() {
        view = glm::lookAt(eye, glm::vec3(0.0f, 0.35f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        proj = glm::perspective(glm::radians(50.0f), 16.0f / 9.0f, 0.1f, 50.0f);
    }

    glm::vec3 toView(const glm::vec3& world) const { return glm::vec3(view * glm::vec4(world, 1.0f)); }

    // Eye depth of whatever the camera sees at this UV; 0 when nothing (sky).
    float eyeDepthAt(const glm::vec2& uv) const {
        // Unproject the far-plane point for this pixel: inv(proj) takes NDC (x, y, 1) back to view space, and the
        // perspective divide has to be undone before normalising.
        const glm::vec4 clip(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 1.0f, 1.0f);
        const glm::vec4 farPoint = glm::inverse(proj) * clip;
        const glm::vec3 dirView = glm::normalize(glm::vec3(farPoint) / farPoint.w);
        const glm::mat4 invView = glm::inverse(view);
        const glm::vec3 dirWorld = glm::normalize(glm::vec3(invView * glm::vec4(dirView, 0.0f)));
        const Hit h = trace(eye, dirWorld);
        if (!h.hit) return 0.0f;
        return -toView(h.position).z;
    }
};

float visibility(const Scene& scene, const glm::vec3& worldPoint, const glm::vec3& worldNormal, const ContactShadowParams& p,
                 const glm::vec2& pixel = glm::vec2(400.0f, 300.0f)) {
    const glm::vec3 viewPos = scene.toView(worldPoint);
    const glm::vec3 viewNormal = glm::mat3(scene.view) * worldNormal;
    const glm::vec3 sunView = glm::mat3(scene.view) * kSunDir;
    const auto depth = [&scene](const glm::vec2& uv) { return scene.eyeDepthAt(uv); };
    return contactShadowVisibility(viewPos, viewNormal, sunView, scene.proj, pixel, depth, p, glm::vec2(1280, 720));
}

} // namespace

int main() {
    const Scene scene;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    // --- 1. The contact region is darkened ---------------------------------------------------------
    {
        ContactShadowParams p;
        p.strength = 1.0f;
        // Just outside the cube on the shadow side, on the floor. The sun is from -x, so the shadow falls towards +x,
        // and the march (0.35 m) reaches into the cube from here.
        const float vis = visibility(scene, glm::vec3(0.62f, 0.0f, 0.0f), up, p);
        checkTrue(vis < 0.95f, "1a: the floor at the cube's shadow edge is darkened");
        checkTrue(vis >= 0.0f, "1b: and the result is inside [0, 1]");
        // Closer to the cube: darker still.
        checkTrue(visibility(scene, glm::vec3(0.54f, 0.0f, 0.0f), up, p) <= vis, "1c: the contact is deepest at the cube");
    }

    // --- 2. A lit point is never darkened ----------------------------------------------------------
    {
        ContactShadowParams p;
        p.strength = 1.0f;
        // Opposite the shadow, and well away from it.
        checkNear(visibility(scene, glm::vec3(-2.5f, 0.0f, 0.0f), up, p), 1.0, 1e-4, "2a: lit floor on the sun side");
        checkNear(visibility(scene, glm::vec3(0.0f, 0.0f, 2.5f), up, p), 1.0, 1e-4, "2b: lit floor in front of the cube");
        // A sweep over the lit half of the floor: none of it may be darkened. This is the property that matters — a
        // false contact shadow is worse than a missing one.
        int falsePositive = 0, tested = 0;
        for (float x = -3.0f; x <= -0.8f; x += 0.2f) {
            for (float z = -1.5f; z <= 1.5f; z += 0.25f) {
                if (inShadow(glm::vec3(x, 0.0f, z))) continue;
                ++tested;
                if (visibility(scene, glm::vec3(x, 0.0f, z), up, p) < 0.999f) ++falsePositive;
            }
        }
        checkTrue(tested > 40, "2c: the sweep covers the lit floor");
        checkTrue(falsePositive == 0, "2d: no lit floor point is darkened (" + std::to_string(falsePositive) + "/" +
                                          std::to_string(tested) + ")");
        // The cube's top is lit from a low sun, and nothing occludes it.
        checkNear(visibility(scene, glm::vec3(0.0f, 1.0f, 0.0f), up, p), 1.0, 1e-4, "2e: the cube's top is untouched");
    }

    // --- 3. Beyond the march's reach, the shadow map keeps its job ---------------------------------
    {
        ContactShadowParams p;
        p.strength = 1.0f;
        // A metre from the cube on the shadow side: in the cube's shadow, but past a 0.35 m march.
        const glm::vec3 farPoint(1.0f, 0.0f, 0.0f);
        const float vis = visibility(scene, farPoint, up, p);
        checkNear(vis, 1.0, 1e-4, "3a: a shadow a metre from its caster is not a contact shadow");
        checkTrue(inShadow(farPoint), "3b: (and it really is in the cube's shadow, so the map has it)");
        // A longer march reaches further: the same point becomes occluded with a 1.2 m ray.
        p.length = 1.2f;
        checkTrue(visibility(scene, farPoint, up, p) < 0.95f, "3c: a longer ray reaches it");
    }

    // --- 4. The knobs move the result the way they claim -------------------------------------------
    {
        ContactShadowParams p;
        p.strength = 1.0f;
        const glm::vec3 q(0.62f, 0.0f, 0.0f);
        const float full = visibility(scene, q, up, p);
        p.strength = 0.5f;
        const float half = visibility(scene, q, up, p);
        checkTrue(half > full, "4a: half strength is lighter than full");
        checkTrue(half <= 1.0f && half >= full - 1e-6f, "4b: and sits between full and unoccluded");
        p.strength = 0.0f;
        checkNear(visibility(scene, q, up, p), 1.0, 1e-6, "4c: strength 0 disables the pass");

        // Thickness: a thinner accepted occluder cannot darken more.
        ContactShadowParams thin;
        thin.thickness = 0.02f;
        ContactShadowParams thick;
        thick.thickness = 0.30f;
        checkTrue(visibility(scene, q, up, thick) <= visibility(scene, q, up, thin) + 1e-6f,
                  "4d: a thicker occluder never lightens");

        // Reach: a longer ray cannot lighten either.
        ContactShadowParams shortRay;
        shortRay.length = 0.15f;
        ContactShadowParams longRay;
        longRay.length = 0.60f;
        checkTrue(visibility(scene, q, up, longRay) <= visibility(scene, q, up, shortRay) + 1e-6f,
                  "4e: a longer ray never lightens");

        // Steps: more steps refine the same answer rather than changing its sign.
        ContactShadowParams few;
        few.steps = 6;
        ContactShadowParams many;
        many.steps = 48;
        const float a = visibility(scene, q, up, few);
        const float b = visibility(scene, q, up, many);
        checkTrue(std::fabs(a - b) < 0.45f, "4f: 6 steps and 48 steps agree on the contact within their granularity");
    }

    // --- 5. Surfaces turned away from the sun, and grazing ones ------------------------------------
    {
        ContactShadowParams p;
        p.strength = 1.0f;
        // The cube's +x face looks away from the sun: left to N.L and the shadow map.
        checkNear(visibility(scene, glm::vec3(0.5f, 0.5f, 0.0f), glm::vec3(1, 0, 0), p), 1.0, 1e-6,
                  "5a: a face turned from the sun is untouched");
        // The -x face is lit by it, and is at the top of the cube's own shadow: it may darken, but must stay in range.
        const float lit = visibility(scene, glm::vec3(-0.5f, 0.5f, 0.0f), glm::vec3(-1, 0, 0), p);
        checkTrue(lit >= 0.0f && lit <= 1.0f, "5b: the lit face stays inside [0, 1]");
    }

    // --- 6. Every sample is finite and in range, with the jitter only changing the phase -----------
    {
        ContactShadowParams p;
        p.strength = 1.0f;
        int outOfRange = 0, nonFinite = 0;
        for (int i = 0; i < 64; ++i) {
            // Vary the pixel so the interleaved-gradient jitter takes a different phase for each sample.
            const glm::vec2 pixel(float(i % 8) * 13.0f + 3.0f, float(i / 8) * 7.0f + 5.0f);
            const float v = visibility(scene, glm::vec3(0.55f + 0.02f * float(i % 4), 0.0f, 0.0f), up, p, pixel);
            if (!std::isfinite(v)) ++nonFinite;
            if (v < 0.0f || v > 1.0f) ++outOfRange;
        }
        checkTrue(nonFinite == 0, "6a: every jitter phase is finite");
        checkTrue(outOfRange == 0, "6b: and stays inside [0, 1]");
        // The deep contact is dark under every phase: jitter is noise control, not a coin flip.
        int dark = 0;
        for (int i = 0; i < 32; ++i) {
            const glm::vec2 pixel(float(i) * 9.0f, float(i) * 5.0f);
            if (visibility(scene, glm::vec3(0.54f, 0.0f, 0.0f), up, p, pixel) < 0.9f) ++dark;
        }
        checkTrue(dark == 32, "6c: every phase darkens the deep contact");
    }

    // --- 7. Degenerate settings --------------------------------------------------------------------
    {
        ContactShadowParams p;
        p.steps = 0;
        checkNear(visibility(scene, glm::vec3(0.6f, 0.0f, 0.0f), up, p), 1.0, 1e-6, "7a: zero steps is off, not a crash");
        ContactShadowParams zeroLength;
        zeroLength.length = 0.0f;
        const float v = visibility(scene, glm::vec3(0.6f, 0.0f, 0.0f), up, zeroLength);
        checkTrue(std::isfinite(v) && v >= 0.0f && v <= 1.0f, "7b: a zero-length ray is finite and in range");
        ContactShadowParams noFade;
        noFade.edgeFade = 0.0f;
        checkTrue(std::isfinite(visibility(scene, glm::vec3(0.6f, 0.0f, 0.0f), up, noFade)), "7c: no edge fade is finite");
        // A point behind the camera cannot be projected: the march must give up rather than sample nonsense.
        ContactShadowParams behind;
        behind.strength = 1.0f;
        const float b = visibility(scene, glm::vec3(-6.0f, 0.0f, -6.0f), up, behind);
        checkTrue(std::isfinite(b) && b >= 0.0f && b <= 1.0f, "7d: a point behind the camera stays in range");
    }

    std::printf("contact shadows: %d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
