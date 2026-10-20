#include "engine/trail.h"

#include <algorithm>
#include <cmath>

namespace ukiyo {

namespace {

constexpr float kEpsilon = 1e-6f;

TrailColorStop gradientDefault(int i) {
    // Tail transparent, head solid: the shape most trails want, and the one docs/TRAIL.md draws.
    static const TrailColorStop stops[2] = {{0.0f, {1.0f, 1.0f, 1.0f, 0.0f}}, {1.0f, {1.0f, 1.0f, 1.0f, 1.0f}}};
    return stops[i];
}

TrailColorStop newestStop() { return gradientDefault(1); }

glm::vec3 anyPerpendicular(const glm::vec3& v) {
    // A stable second axis for the degenerate cross products below. Picking the smallest component keeps the result
    // well conditioned whichever way the trail happens to point.
    const glm::vec3 a = std::fabs(v.x) <= std::fabs(v.y) && std::fabs(v.x) <= std::fabs(v.z) ? glm::vec3(1, 0, 0)
                       : std::fabs(v.y) <= std::fabs(v.z)                                        ? glm::vec3(0, 1, 0)
                                                                                                 : glm::vec3(0, 0, 1);
    return a;
}

} // namespace

// ---------------------------------------------------------------- gradient

TrailGradient::TrailGradient() : stops_{gradientDefault(0), newestStop()} {}

TrailGradient::TrailGradient(std::vector<TrailColorStop> stops) { setStops(std::move(stops)); }

void TrailGradient::setStops(std::vector<TrailColorStop> stops) {
    // Sorted by t so sample() is a single scan; a stop outside [0, 1] is clamped rather than dropped, because a curve
    // authored in a panel can land there and silently losing the user's colour is worse than compressing it.
    for (TrailColorStop& s : stops) s.t = std::clamp(s.t, 0.0f, 1.0f);
    std::stable_sort(stops.begin(), stops.end(),
                     [](const TrailColorStop& a, const TrailColorStop& b) { return a.t < b.t; });
    stops_ = std::move(stops);
}

glm::vec4 TrailGradient::sample(float t) const {
    if (stops_.empty()) return glm::vec4(1.0f);
    if (stops_.size() == 1) return stops_.front().color;
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= stops_.front().t) return stops_.front().color;
    if (t >= stops_.back().t) return stops_.back().color;
    for (size_t i = 1; i < stops_.size(); ++i) {
        if (t > stops_[i].t) continue;
        const float span = stops_[i].t - stops_[i - 1].t;
        const float f = span <= kEpsilon ? 0.0f : (t - stops_[i - 1].t) / span;
        return glm::mix(stops_[i - 1].color, stops_[i].color, f);
    }
    return stops_.back().color;
}

// ---------------------------------------------------------------- width curve

TrailWidthCurve::TrailWidthCurve() : points_{{0.0f, 0.15f}, {0.3f, 1.0f}, {1.0f, 0.45f}} {}

TrailWidthCurve::TrailWidthCurve(std::vector<glm::vec2> points) { setPoints(std::move(points)); }

void TrailWidthCurve::setPoints(std::vector<glm::vec2> points) {
    for (glm::vec2& p : points) {
        p.x = std::clamp(p.x, 0.0f, 1.0f);
        p.y = std::max(0.0f, p.y);
    }
    std::stable_sort(points.begin(), points.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
    points_ = std::move(points);
}

float TrailWidthCurve::sample(float t) const {
    if (points_.empty()) return 1.0f;
    if (points_.size() == 1) return points_.front().y;
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= points_.front().x) return points_.front().y;
    if (t >= points_.back().x) return points_.back().y;
    for (size_t i = 1; i < points_.size(); ++i) {
        if (t > points_[i].x) continue;
        const float span = points_[i].x - points_[i - 1].x;
        const float f = span <= kEpsilon ? 0.0f : (t - points_[i - 1].x) / span;
        return glm::mix(points_[i - 1].y, points_[i].y, f);
    }
    return points_.back().y;
}

// ---------------------------------------------------------------- trail

void Trail::clear() {
    points_.clear();
    totalArc_ = 0.0f;
}

void Trail::reset(const glm::vec3& head) {
    clear();
    Section s;
    s.position = head;
    s.previous = head;
    points_.push_back(s);
}

bool Trail::advance(const glm::vec3& head) {
    if (points_.empty()) {
        reset(head);
        return true;
    }
    Section& newest = points_.back();
    const float dist = glm::length(head - newest.position);
    if (dist < settings_.minSectionLength) {
        // The emitter has moved, but not far enough for a section. Move the head anyway: the ribbon has to reach the
        // object, and a head that only jumped every section would visibly lag it.
        if (dist > kEpsilon) {
            newest.previous = newest.position;
            newest.position = head;
            if (points_.size() >= 2) newest.arc = points_[points_.size() - 2].arc + dist;
            totalArc_ = newest.arc;
        }
        return false;
    }
    Section s;
    s.position = head;
    s.previous = newest.position;
    s.arc = newest.arc + dist;
    points_.push_back(s);
    totalArc_ = s.arc;
    // The cap is a memory bound, not a look: a long lifetime or a fast object would otherwise grow the list every
    // frame. The tail goes first, which is what the lifetime would have removed a moment later anyway.
    while (settings_.maxSections > 1 && static_cast<int>(points_.size()) > settings_.maxSections) points_.erase(points_.begin());
    return true;
}

void Trail::update(float dt) {
    if (points_.empty()) return;
    for (Section& s : points_) s.age += dt;

    if (settings_.limit == TrailLimit::Lifetime) {
        // Two sections stay: a trail that lost its last pair would pop out of existence, and Godot keeps the ribbon
        // until the emitter stops feeding it too.
        while (points_.size() > 2 && points_.front().age > std::max(0.0f, settings_.lifetime)) points_.erase(points_.begin());
    } else {
        while (points_.size() > 2 && span() > std::max(0.0f, settings_.maxLength)) points_.erase(points_.begin());
    }
    while (settings_.maxSections > 1 && static_cast<int>(points_.size()) > settings_.maxSections) points_.erase(points_.begin());
}

float Trail::span() const {
    float total = 0.0f;
    for (size_t i = 1; i < points_.size(); ++i) total += glm::length(points_[i].position - points_[i - 1].position);
    return total;
}

float Trail::length() const { return span(); }

uint32_t Trail::buildRibbon(const glm::vec3& eye, std::vector<TrailVertex>* vertices,
                            std::vector<uint32_t>* indices) const {
    const size_t n = points_.size();
    if (vertices) vertices->clear();
    if (indices) indices->clear();
    if (!hasGeometry() || n < 2) return 0;
    if (!vertices || !indices) return 0;

    vertices->resize(n * 2);
    indices->resize((n - 1) * 6);
    uint32_t indexCount = 0;
    const uint32_t written = buildRibbon(eye, vertices->data(), static_cast<uint32_t>(vertices->size()), indices->data(),
                                         static_cast<uint32_t>(indices->size()), &indexCount);
    vertices->resize(written);
    indices->resize(indexCount);
    return indexCount / 3;
}

uint32_t Trail::buildRibbon(const glm::vec3& eye, TrailVertex* vertices, uint32_t vertexCapacity, uint32_t* indices,
                            uint32_t indexCapacity, uint32_t* indexCount) const {
    if (indexCount) *indexCount = 0;
    const size_t n = points_.size();
    if (!hasGeometry() || n < 2 || !vertices || !indices) return 0;
    if (vertexCapacity < n * 2 || indexCapacity < (n - 1) * 6) return 0;

    // t along the trail is measured in arc length, not in sections: a section added after a long frame should not
    // own a third of the gradient just because it is one third of the points.
    const float arcSpan = points_.back().arc - points_.front().arc;
    const auto tAt = [&](size_t i) {
        if (arcSpan > kEpsilon) return (points_[i].arc - points_.front().arc) / arcSpan;
        return n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
    };

    glm::vec3 previousTangent(0.0f);
    for (size_t i = 0; i < n; ++i) {
        const glm::vec3& p = points_[i].position;
        // Central difference inside, one-sided at the ends. The head's one-sided tangent uses where it was, so a head
        // that has not moved since the last section still has a direction instead of a zero vector.
        glm::vec3 tangent;
        if (i == 0) tangent = points_[1].position - points_[0].position;
        else if (i + 1 < n) tangent = points_[i + 1].position - points_[i - 1].position;
        else tangent = p - points_[i].previous;
        if (glm::length(tangent) < kEpsilon && i > 0) tangent = p - points_[i - 1].position;
        if (glm::length(tangent) < kEpsilon) tangent = previousTangent;
        if (glm::length(tangent) < kEpsilon) tangent = glm::vec3(0.0f, 0.0f, 1.0f);
        tangent = glm::normalize(tangent);
        previousTangent = tangent;

        glm::vec3 side;
        if (settings_.align == TrailAlign::Billboard) {
            const glm::vec3 toEye = eye - p;
            side = glm::cross(tangent, toEye);
            // Looking straight down the trail: any perpendicular will do, and the fallback keeps the strip from
            // collapsing to a line for the frames the camera is behind the object.
            if (glm::length(side) < kEpsilon) side = glm::cross(tangent, anyPerpendicular(tangent));
        } else {
            side = glm::cross(tangent, settings_.localUp);
            if (glm::length(side) < kEpsilon) side = glm::cross(tangent, anyPerpendicular(settings_.localUp));
        }
        if (glm::length(side) < kEpsilon) side = anyPerpendicular(tangent);
        side = glm::normalize(side);

        const float t = tAt(i);
        const float halfWidth = 0.5f * settings_.width * settings_.widthCurve.sample(t);
        // The colour keeps the authored alpha; the width curve is what tapers the silhouette.
        const glm::vec4 color = settings_.gradient.sample(t);

        float u;
        if (settings_.pinUv) {
            // Pinned: u follows the distance travelled from where the trail started, so the texture stays on the
            // ground rather than sliding along the strip as it grows.
            u = points_[i].arc * settings_.tilingMultiplier;
        } else if (settings_.tiling == TrailTiling::Stretch) {
            u = t * settings_.tilingMultiplier;
        } else {
            u = (points_[i].arc - points_.front().arc) * settings_.tilingMultiplier;
        }

        TrailVertex& a = vertices[i * 2];
        TrailVertex& b = vertices[i * 2 + 1];
        a.position = p - side * halfWidth;
        b.position = p + side * halfWidth;
        a.uv = glm::vec2(u, 0.0f);
        b.uv = glm::vec2(u, 1.0f);
        a.color = color;
        b.color = color;
    }

    uint32_t k = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint32_t base = static_cast<uint32_t>(i) * 2;
        // Two triangles, wound so the face normal agrees with the side vector: (a, b, c) and (c, b, d).
        indices[k++] = base + 0;
        indices[k++] = base + 1;
        indices[k++] = base + 2;
        indices[k++] = base + 2;
        indices[k++] = base + 1;
        indices[k++] = base + 3;
    }
    if (indexCount) *indexCount = k;
    return static_cast<uint32_t>(n * 2);
}

} // namespace ukiyo
