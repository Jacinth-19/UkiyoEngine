// Visibility helpers for the renderer: a conservative box-against-frustum test, and the distance cull and fade
// for a model. Header-only, so the tests can use them without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

// True when the box [bmin, bmax] (model space), moved by `world` and grown by `margin`, lies wholly outside one
// plane of the clip volume of `viewProj`. The volume is x and y in [-w, w] and z in [0, w] (glm's _ZO
// projections). A false answer means "maybe visible". A box with a corner at or behind the eye is never culled.
inline bool boxOutsideFrustum(const glm::mat4& viewProj, const glm::mat4& world, glm::vec3 bmin, glm::vec3 bmax,
                              float margin) {
    bmin -= glm::vec3(margin);
    bmax += glm::vec3(margin);
    const glm::mat4 m = viewProj * world;
    bool left = true, right = true, bottom = true, top = true, nearOut = true, farOut = true;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
        const glm::vec4 h = m * glm::vec4(c, 1.0f);
        if (h.w <= 0.0f) return false;
        left = left && h.x < -h.w;
        right = right && h.x > h.w;
        bottom = bottom && h.y < -h.w;
        top = top && h.y > h.w;
        nearOut = nearOut && h.z < 0.0f;
        farOut = farOut && h.z > h.w;
    }
    return left || right || bottom || top || nearOut || farOut;
}

struct DistanceVisibility {
    bool visible = true;
    float fade = 0.0f;   // fraction of the surface dithered out, 0 solid to 1 gone
};

// cullDistance <= 0 turns the cull off. The fade runs from fadeStart to cullDistance and needs fadeStart to be
// inside that range; otherwise the draw is solid until it is culled.
inline DistanceVisibility distanceVisibility(float distance, float fadeStart, float cullDistance) {
    DistanceVisibility v;
    if (cullDistance <= 0.0f) return v;
    if (distance >= cullDistance) {
        v.visible = false;
        v.fade = 1.0f;
        return v;
    }
    if (fadeStart > 0.0f && fadeStart < cullDistance && distance > fadeStart)
        v.fade = (distance - fadeStart) / (cullDistance - fadeStart);
    return v;
}

// Mesh LOD level (item 15). `switches` holds the distance at which each level starts: level 0 (full detail)
// below switches[0], level 1 from switches[0] up to switches[1], and so on. Empty switches mean level 0.
inline int lodForDistance(float distance, const std::vector<float>& switches) {
    int level = 0;
    for (float s : switches)
        if (distance >= s) ++level;
    return level;
}

// Most switch distances one model may have. Three levels keep the last one at 1/8 of the indices.
constexpr std::size_t kMaxLodSwitches = 3;

// Cleans LOD switch distances from a file or a text field: keeps finite positive values, sorts them ascending,
// drops repeats, and keeps at most kMaxLodSwitches. Empty means LOD off.
inline std::vector<float> normalizeLodSwitches(std::vector<float> switches) {
    std::vector<float> out;
    for (float s : switches)
        if (std::isfinite(s) && s > 0.0f) out.push_back(s);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > kMaxLodSwitches) out.resize(kMaxLodSwitches);
    return out;
}

} // namespace ukiyo
