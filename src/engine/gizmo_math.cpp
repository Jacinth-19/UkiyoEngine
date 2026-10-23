#include "engine/gizmo_math.h"

#include "engine/transform.h"

#include <algorithm>
#include <cmath>

namespace ukiyo::gizmo {

namespace {
constexpr float kPi = 3.14159265358979f;

// The thresholds from Blender's transform_gizmo_3d.cc. A line needs a dot of 0.02 to be drawn; a plane needs 0.175,
// because a plane seen edge-on is a sliver that no one can hit.
constexpr float kLineMinDot = 0.02f;
constexpr float kPlaneMinDot = 0.175f;

glm::vec3 normalise(const glm::vec3& v) {
    const float len = glm::length(v);
    return len > 1e-8f ? v / len : glm::vec3(0.0f);
}
} // namespace

glm::vec3 axisDir(Axis a) {
    switch (a) {
    case Axis::X: return glm::vec3(1.0f, 0.0f, 0.0f);
    case Axis::Y: return glm::vec3(0.0f, 1.0f, 0.0f);
    case Axis::Z: return glm::vec3(0.0f, 0.0f, 1.0f);
    }
    return glm::vec3(1.0f, 0.0f, 0.0f);
}

glm::vec3 perpendicular(const glm::vec3& axis) {
    // Cross with whichever cardinal direction the axis is least aligned with: the result is never short, and it is the
    // same for the same input, which a drag's reference direction has to be.
    const glm::vec3 a = normalise(axis);
    const glm::vec3 pick = std::fabs(a.x) < std::fabs(a.y) ? (std::fabs(a.x) < std::fabs(a.z) ? glm::vec3(1, 0, 0)
                                                                                             : glm::vec3(0, 0, 1))
                                                           : (std::fabs(a.y) < std::fabs(a.z) ? glm::vec3(0, 1, 0)
                                                                                              : glm::vec3(0, 0, 1));
    return normalise(glm::cross(a, pick));
}

bool rayPlane(const Ray& r, const glm::vec3& point, const glm::vec3& normal, float* t) {
    const float denom = glm::dot(r.dir, normal);
    if (std::fabs(denom) < 1e-6f) return false;   // the ray runs along the plane
    const float hit = glm::dot(point - r.origin, normal) / denom;
    if (hit < 0.0f) return false;                 // the plane is behind the eye
    if (t) *t = hit;
    return true;
}

bool planePoint(const Ray& r, const glm::vec3& point, const glm::vec3& normal, glm::vec3* hit) {
    float t = 0.0f;
    if (!rayPlane(r, point, normal, &t)) return false;
    if (hit) *hit = r.origin + r.dir * t;
    return true;
}

float axisParam(const Ray& r, const glm::vec3& point, const glm::vec3& axis) {
    // Closest approach of two lines: the ray (origin + s*dir) and the axis (point + t*a).
    const glm::vec3 a = normalise(axis);
    const glm::vec3 w = r.origin - point;
    const float b = glm::dot(r.dir, a);
    const float denom = 1.0f - b * b;
    if (std::fabs(denom) < 1e-6f) return 0.0f;   // the ray runs parallel to the axis
    return (glm::dot(a, w) - b * glm::dot(r.dir, w)) / denom;
}

bool angleAboutAxis(const Ray& r, const glm::vec3& point, const glm::vec3& axis, const glm::vec3& reference,
                    float* deg) {
    glm::vec3 hit(0.0f);
    if (!planePoint(r, point, normalise(axis), &hit)) return false;
    const glm::vec3 a = normalise(axis);
    glm::vec3 u = normalise(reference - a * glm::dot(reference, a));
    if (glm::length(u) < 1e-6f) u = perpendicular(a);   // the reference was along the axis: any perpendicular will do
    const glm::vec3 v = glm::cross(a, u);
    const glm::vec3 d = hit - point;
    if (deg) *deg = std::atan2(glm::dot(d, v), glm::dot(d, u)) * 180.0f / kPi;
    return true;
}

float deltaDeg(float a, float b) {
    float d = a - b;
    while (d <= -180.0f) d += 360.0f;
    while (d > 180.0f) d -= 360.0f;
    return d;
}

float axisVisibility(const glm::vec3& viewDir, const glm::vec3& axis, bool plane) {
    // Blender's gizmo_get_idot: for a line, one minus the |dot| with the view is one when the axis lies across the
    // screen and zero when it points at the viewer and is only a dot. A plane is the other way round — it needs its
    // normal pointing at the viewer to be face-on — so its measure is the |dot| itself.
    const float d = std::fabs(glm::dot(normalise(viewDir), normalise(axis)));
    return plane ? d : 1.0f - d;
}

bool axisVisible(const glm::vec3& viewDir, const glm::vec3& axis, bool plane) {
    return axisVisibility(viewDir, axis, plane) >= (plane ? kPlaneMinDot : kLineMinDot);
}

float scaleDelta(const glm::vec2& from, const glm::vec2& to, const glm::vec2& axisPixels, float* alongPixels) {
    const float len = glm::length(axisPixels);
    if (len < 1e-4f) {
        if (alongPixels) *alongPixels = 0.0f;
        return 0.0f;
    }
    const glm::vec2 dir = axisPixels / len;
    const float travel = glm::dot(to - from, dir);
    if (alongPixels) *alongPixels = travel;
    return travel / len;
}

float scaleRadial(const glm::vec2& centre, const glm::vec2& from, const glm::vec2& to) {
    const float d0 = std::max(1.0f, glm::length(from - centre));
    const float d1 = glm::length(to - centre);
    return d1 / d0;
}

float snapScale(float scale, float step) {
    float s = scale;
    if (step > 0.0f) s = std::round(s / step) * step;
    return std::clamp(s, 0.01f, 100.0f);
}

float snapAngle(float deg, float stepDeg) {
    if (stepDeg <= 0.0f) return deg;
    return std::round(deg / stepDeg) * stepDeg;
}

glm::vec3 selectionPivot(const std::vector<glm::vec3>& positions) {
    if (positions.empty()) return glm::vec3(0.0f);
    glm::vec3 sum(0.0f);
    for (const glm::vec3& p : positions) sum += p;
    return sum / float(positions.size());
}

glm::vec3 movedBy(const glm::vec3& position, const glm::vec3& delta) { return position + delta; }

Placement rotatedAbout(const Placement& p, const glm::vec3& pivot, const glm::vec3& deltaDeg) {
    const glm::quat turn = eulerToQuat(deltaDeg);
    Placement out = p;
    // The position swings around the pivot on the same turn the object makes.
    out.position = pivot + turn * (p.position - pivot);
    // The rotation composes: the turn is applied on top of what the object already had, in the engine's ZXY order,
    // because adding to one Euler angle of an already tilted object turns it about the wrong axis.
    out.rotationDeg = quatToEuler(turn * eulerToQuat(p.rotationDeg));
    return out;
}

Placement scaledAbout(const Placement& p, const glm::vec3& pivot, const glm::vec3& factor) {
    const glm::vec3 f = glm::vec3(std::max(factor.x, 0.01f), std::max(factor.y, 0.01f), std::max(factor.z, 0.01f));
    Placement out = p;
    // The position moves out from the pivot by the same factor the object grows by, so two objects twice as far apart
    // stay twice as far apart after a uniform drag.
    out.position = pivot + f * (p.position - pivot);
    // The scale keeps the same hundredth-of-its-size floor the scale handles use, so a drag cannot flatten a
    // selection into a plane it can never be unflattened from.
    out.scale = glm::max(p.scale * f, glm::vec3(0.01f));
    return out;
}

} // namespace ukiyo::gizmo
