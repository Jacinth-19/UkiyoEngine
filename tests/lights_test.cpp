// Scene lights: packing, scoring, selection order and the std140 layout shared with shaders/frame.glsl.
#include "engine/lights.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ukiyo;

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
bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    // Type and cone packing.
    SceneLight spot;
    spot.type = LightType::Spot;
    spot.position = {0, 4, 0};
    spot.direction = {0, -1, 0};
    spot.color = {1.0f, 0.5f, 0.25f};
    spot.intensity = 3.0f;
    spot.radius = 12.0f;
    spot.spotAngleDeg = 60.0f;
    spot.spotSoftness = 0.5f;
    const GpuLight g = packLight(spot);
    check(near(g.posRadius.x, 0.0f) && near(g.posRadius.y, 4.0f) && near(g.posRadius.w, 12.0f), "position and radius pack");
    check(near(g.dirCosOuter.y, -1.0f), "direction is normalised");
    check(near(g.dirCosOuter.w, std::cos(30.0f * 3.14159265f / 180.0f), 1e-5f), "outer cone is cos(half angle)");
    check(near(g.params.x, 2.0f), "spot type reaches the shader");
    check(near(g.params.y, std::cos(15.0f * 3.14159265f / 180.0f), 1e-5f), "inner cone uses the softness fraction");
    check(near(g.colorIntensity.w, 3.0f), "intensity is in colour.w");
    check(sizeof(GpuLight) == 64, "GpuLight is four vec4s");

    // A zero direction must not produce NaN.
    SceneLight degenerate;
    degenerate.direction = {0, 0, 0};
    const GpuLight dg = packLight(degenerate);
    check(near(dg.dirCosOuter.y, -1.0f), "zero direction falls back to straight down");
    check(!std::isnan(dg.dirCosOuter.x), "zero direction has no NaN");

    // A zero radius is clamped so the shader's division stays finite.
    SceneLight zeroR;
    zeroR.radius = 0.0f;
    check(packLight(zeroR).posRadius.w >= 0.01f, "zero radius is clamped");

    // Scoring: same light, near beats far; behind the camera is damped but not dropped.
    SceneLight l;
    l.position = {0, 0, 5};
    const float nearScore = lightScore(l, {0, 0, 0}, {0, 0, 1});
    l.position = {0, 0, 40};
    const float farScore = lightScore(l, {0, 0, 0}, {0, 0, 1});
    check(nearScore > farScore, "a closer light scores higher");
    check(farScore == 0.0f, "a light past its radius scores zero");
    l.position = {0, 0, -5};
    const float behind = lightScore(l, {0, 0, 0}, {0, 0, 1});
    check(behind > 0.0f && behind < nearScore, "a light behind the camera is damped, not dropped");
    l.enabled = false;
    check(lightScore(l, {0, 0, 0}, {0, 0, 1}) == 0.0f, "a disabled light scores zero");

    // Selection: max is respected, directional lights win, and the closest of many points is kept.
    std::vector<SceneLight> lights;
    SceneLight sun;
    sun.type = LightType::Directional;
    sun.intensity = 1.0f;
    lights.push_back(sun);
    for (int i = 0; i < 20; ++i) {
        SceneLight p;
        p.type = LightType::Point;
        p.position = {float(i), 0.0f, 0.0f};
        p.radius = 30.0f;   // in reach, so the budget and not the radius decides
        lights.push_back(p);
    }
    GpuLight packed[kMaxSceneLights];
    const int n = packSceneLights(lights, {0, 0, 0}, {0, 0, 1}, kMaxSceneLights, packed);
    check(n == kMaxSceneLights, "selection fills the budget");
    check(near(packed[0].params.x, 0.0f), "the directional light is first");
    // The nearest point light is at x=0 (distance 0), so it must be second.
    check(near(packed[1].posRadius.x, 0.0f) && near(packed[1].params.x, 1.0f), "the nearest point light is second");

    // Beyond the radius nothing is selected.
    std::vector<SceneLight> far;
    SceneLight f;
    f.position = {100, 0, 0};
    f.radius = 5.0f;
    far.push_back(f);
    check(packSceneLights(far, {0, 0, 0}, {0, 0, 1}, kMaxSceneLights, packed) == 0, "lights out of reach are dropped");

    // max=0 and a null output are safe.
    check(packSceneLights(lights, {0, 0, 0}, {0, 0, 1}, 0, packed) == 0, "max 0 selects nothing");
    check(packSceneLights(lights, {0, 0, 0}, {0, 0, 1}, 4, nullptr) == 0, "a null output is safe");
    check(packSceneLights(lights, {0, 0, 0}, {0, 0, 1}, 4, packed) == 4, "max 4 selects four");

    // Determinism: the same input twice gives the same order.
    GpuLight again[kMaxSceneLights];
    packSceneLights(lights, {0, 0, 0}, {0, 0, 1}, kMaxSceneLights, again);
    bool same = true;
    for (int i = 0; i < kMaxSceneLights; ++i)
        same = same && again[i].posRadius == packed[i].posRadius && again[i].params.x == packed[i].params.x;
    check(same, "selection is deterministic");

    std::printf("lights_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
