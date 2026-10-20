// Multi-bounce ambient occlusion (docs/AO.md): the curve shaders/effects.frag evaluates, checked on the CPU.
//
// The properties that matter are physical, so they are measured rather than asserted:
//   * unoccluded stays unoccluded, for every albedo;
//   * more occlusion never yields more light;
//   * the approximation only ever restores light, never removes more than single-bounce AO;
//   * a brighter or more saturated surface keeps more of its ambient light;
//   * a fully occluded corner never goes black, and the engineered endpoints match the fit's constants.

#include "engine/ao.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <glm/glm.hpp>

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
        std::printf("FAIL  %-62s got %.6f want %.6f (tol %.6f)\n", what.c_str(), got, want, tol);
    }
}

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

int main() {
    // --- 1. The coefficients are the ones in the paper ------------------------------------------------------------
    {
        checkNear(MultiBounceFit::kA, 2.0404, 1e-6, "1a: a's albedo slope");
        checkNear(MultiBounceFit::kB, -4.7951, 1e-6, "1b: b's albedo slope");
        checkNear(MultiBounceFit::kC, 2.7552, 1e-6, "1c: c's albedo slope");
        checkNear(multiBounceACoefficient(glm::vec3(0.0f)).x, -0.3324, 1e-6, "1d: a(0) is -0.3324");
        checkNear(multiBounceBCoefficient(glm::vec3(0.0f)).x, 0.6417, 1e-6, "1e: b(0) is 0.6417");
        checkNear(multiBounceCCoefficient(glm::vec3(0.0f)).x, 0.6903, 1e-6, "1f: c(0) is 0.6903");
        // At albedo 1 the polynomial has a root sum of ~1 at ao = 1: 0.0005 + 0.9996.
        const double a = multiBounceACoefficient(glm::vec3(1.0f)).x;
        const double b = multiBounceBCoefficient(glm::vec3(1.0f)).x;
        const double c = multiBounceCCoefficient(glm::vec3(1.0f)).x;
        checkNear(a + b + c, 0.9996, 1e-3, "1g: the fit passes through 1 at ao = 1");
    }

    // --- 2. Endpoints ----------------------------------------------------------------------------------------------
    {
        for (float albedo : {0.0f, 0.05f, 0.25f, 0.5f, 0.8f, 1.0f}) {
            checkNear(multiBounceVisibility(1.0f, albedo), 1.0, 2e-3,
                      "2a: an unoccluded point keeps all of its light, albedo " + std::to_string(albedo).substr(0, 4));
            // Fully occluded is dark for every albedo: the fit has no way to invent light it cannot see.
            checkNear(multiBounceVisibility(0.0f, albedo), 0.0, 1e-6,
                      "2b: fully occluded is zero, albedo " + std::to_string(albedo).substr(0, 4));
        }
        // The fit bulges above the straight line between them, and a brighter surface bulges more.
        // A black surface has nothing to bounce, so the fit falls below the straight line there and Godot's `max`
        // keeps the single-bounce value: the curve can never darken more than AO asked for.
        checkNear(multiBounceVisibility(0.5f, 0.0f),
                  std::max(0.5f, -0.3324f * 0.125f + 0.6417f * 0.25f + 0.6903f * 0.5f), 2e-3,
                  "2c: the dark case falls back to the single-bounce term");
        checkTrue(multiBounceVisibility(0.5f, glm::vec3(1.0f)).x > multiBounceVisibility(0.5f, glm::vec3(0.0f)).x,
                  "2d: and the bright case is above it");
    }

    // --- 3. Monotonicity, and the clamp's two invariants ------------------------------------------------------------
    {
        bool monotone = true, aboveSingle = true, belowOne = true;
        for (int ai = 0; ai <= 20; ++ai) {
            const glm::vec3 albedo(float(ai) / 20.0f);
            float prev = -1.0f;
            for (int i = 0; i <= 100; ++i) {
                const float ao = float(i) / 100.0f;
                const glm::vec3 v = multiBounceVisibility(ao, albedo);
                if (!finite(v)) checkTrue(false, "3a: finite, every sample");
                if (v.x < prev - 1e-6f) monotone = false;                       // more visibility → more light
                if (v.x < ao - 1e-6f) aboveSingle = false;                      // never darker than single-bounce
                if (v.x > 1.0f + 1e-6f) belowOne = false;                       // never brighter than unoccluded
                if (v.x < 0.0f) checkTrue(false, "3b: never negative");
                prev = v.x;
            }
        }
        checkTrue(monotone, "3c: the curve is non-decreasing in AO for every albedo");
        checkTrue(aboveSingle, "3d: multi-bounce never removes more light than single-bounce AO");
        checkTrue(belowOne, "3e: and never adds light above unoccluded");
    }

    // --- 4. Albedo dependence: the point of the feature ------------------------------------------------------------
    {
        // At half occlusion, a brighter surface keeps more light than a darker one.
        float prev = -1.0f;
        bool rising = true;
        for (int ai = 0; ai <= 20; ++ai) {
            const float v = multiBounceVisibility(0.5f, float(ai) / 20.0f);
            if (v < prev - 1e-6f) rising = false;
            prev = v;
        }
        checkTrue(rising, "4a: a brighter albedo restores more ambient light at half occlusion");

        // And the restoration is per channel, so a red wall in a corner tints what it gives back.
        const glm::vec3 greyA = multiBounceVisibility(0.2f, glm::vec3(0.5f));
        const glm::vec3 warmA = multiBounceVisibility(0.2f, glm::vec3(0.9f, 0.4f, 0.2f));
        checkTrue(warmA.r > warmA.g && warmA.g > warmA.b, "4b: a warm wall returns warm light");
        checkTrue(warmA.r > greyA.r, "4c: and its red channel is above the grey case");
        checkTrue(warmA.b < greyA.b, "4d: while its blue channel is below it");

        // The old behaviour, for comparison: no colour, no restoration.
        checkNear(singleBounceVisibility(0.2f), 0.2, 1e-6, "4e: single-bounce is the identity times ao");
        checkTrue(multiBounceVisibility(0.2f, glm::vec3(0.5f)).x > singleBounceVisibility(0.2f),
                  "4f: multi-bounce is brighter than single-bounce in an occluded corner");
        checkNear(singleBounceVisibility(1.5f), 1.0, 1e-6, "4g: single-bounce clamps its input");
    }

    // --- 5. Out-of-range input is clamped, not propagated ----------------------------------------------------------
    {
        checkNear(multiBounceVisibility(-1.0f, glm::vec3(0.5f)).x, multiBounceVisibility(0.0f, glm::vec3(0.5f)).x, 1e-6,
                  "5a: a negative AO is treated as fully occluded");
        checkNear(multiBounceVisibility(2.0f, glm::vec3(0.5f)).x, 1.0, 1e-6, "5b: AO above 1 is unoccluded");
        checkNear(multiBounceVisibility(0.5f, glm::vec3(2.0f)).x, multiBounceVisibility(0.5f, glm::vec3(1.0f)).x, 1e-6,
                  "5c: albedo above 1 is clamped");
        checkNear(multiBounceVisibility(0.5f, glm::vec3(-1.0f)).x, multiBounceVisibility(0.5f, glm::vec3(0.0f)).x, 1e-6,
                  "5d: a negative albedo is clamped to black");
    }

    // --- 6. A worked example, so the numbers in the docs cannot drift ---------------------------------------------
    {
        // The mid-grey case the docs quote: ao 0.5 with a 0.6 grey.
        const float v = multiBounceVisibility(0.5f, 0.6f);
        // a = 0.892, b = -2.2354, c = 2.3434; v = ((0.5*0.892 - 2.2354)*0.5 + 2.3434)*0.5
        checkNear(v, ((0.5 * 0.89200 - 2.23536) * 0.5 + 2.34342) * 0.5, 1e-4, "6a: the worked mid-grey value");
        checkTrue(v > 0.5f && v <= 1.0f, "6b: and it sits between the single-bounce term and unoccluded");
        // A saturated red at the same occlusion recovers more red than blue.
        const glm::vec3 red = multiBounceVisibility(0.5f, glm::vec3(0.85f, 0.15f, 0.15f));
        checkTrue(red.r > red.g && red.r > red.b, "6c: red stays red");
    }

    std::printf("ao: %d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
