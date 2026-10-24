#include "engine/lights.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ukiyo {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

float cosHalfAngle(float deg) {
    const float d = std::clamp(deg, 0.5f, 179.0f);
    return std::cos(0.5f * d * kPi / 180.0f);
}

GpuLight packLight(const SceneLight& l) {
    GpuLight g{};
    const glm::vec3 dir = glm::length(l.direction) > 1e-6f ? glm::normalize(l.direction) : glm::vec3(0, -1, 0);
    const float radius = l.type == LightType::Directional ? 0.0f : std::max(l.radius, 0.01f);
    const float softness = std::clamp(l.spotSoftness, 0.0f, 0.95f);
    const float outer = cosHalfAngle(l.spotAngleDeg);
    const float inner = cosHalfAngle(l.spotAngleDeg * (1.0f - softness));
    g.posRadius = glm::vec4(l.position, radius);
    g.dirCosOuter = glm::vec4(dir, outer);
    g.colorIntensity = glm::vec4(l.color, l.intensity);
    g.params = glm::vec4(static_cast<float>(l.type), inner, 0.0f, 0.0f);
    return g;
}

float lightScore(const SceneLight& l, const glm::vec3& focus, const glm::vec3& forward) {
    if (!l.enabled) return 0.0f;
    if (l.type == LightType::Directional) return 1e9f;   // always worth a slot
    const glm::vec3 d = l.position - focus;
    const float dist2 = glm::dot(d, d);
    const float reach = std::max(l.radius, 0.01f);
    // Outside the reach the light contributes nothing (the shader's attenuation is zero there too).
    if (dist2 > reach * reach) return 0.0f;
    // A light behind the camera still lights what is in front of it, but less of the frame; damp instead of rejecting.
    bool behind = false;
    if (glm::length(forward) > 1e-6f) behind = glm::dot(glm::normalize(d), glm::normalize(forward)) < -0.25f;
    const float falloff = 1.0f / (dist2 + 1.0f);
    return l.intensity * reach * reach * falloff * (behind ? 0.35f : 1.0f);
}

int packSceneLights(const std::vector<SceneLight>& lights, const glm::vec3& focus, const glm::vec3& forward, int max,
                    GpuLight* out) {
    if (!out || max <= 0) return 0;
    max = std::min(max, kMaxSceneLights);
    std::vector<int> order;
    order.reserve(lights.size());
    for (int i = 0; i < static_cast<int>(lights.size()); ++i) {
        if (lights[static_cast<size_t>(i)].enabled) order.push_back(i);
    }
    std::vector<float> score(order.size());
    for (size_t k = 0; k < order.size(); ++k) score[k] = lightScore(lights[static_cast<size_t>(order[k])], focus, forward);
    std::vector<int> idx(order.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (score[static_cast<size_t>(a)] != score[static_cast<size_t>(b)])
            return score[static_cast<size_t>(a)] > score[static_cast<size_t>(b)];
        return order[static_cast<size_t>(a)] < order[static_cast<size_t>(b)];   // author order breaks ties
    });
    int n = 0;
    for (int k = 0; k < static_cast<int>(idx.size()) && n < max; ++k) {
        const int i = order[static_cast<size_t>(idx[static_cast<size_t>(k)])];
        const SceneLight& l = lights[static_cast<size_t>(i)];
        if (l.type != LightType::Directional && score[static_cast<size_t>(idx[static_cast<size_t>(k)])] <= 0.0f) continue;
        out[n++] = packLight(l);
    }
    return n;
}

} // namespace ukiyo
