#include "scene_gizmo.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace editor::gizmo {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr double kHitPixels = 8.0;          // how close the pointer must be to a handle
constexpr int kRingSegments = 96;
constexpr double kHandleFraction = 0.18;    // handle length as a fraction of the view height

glm::vec3 axisDir(Part p) {
    switch (p) {
    case Part::MoveX: return glm::vec3(1.0f, 0.0f, 0.0f);
    case Part::MoveY: return glm::vec3(0.0f, 1.0f, 0.0f);
    case Part::MoveZ: return glm::vec3(0.0f, 0.0f, 1.0f);
    default: return glm::vec3(0.0f);
    }
}

float wrapDeg(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

double distanceToSegment(QPointF p, QPointF a, QPointF b) {
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = 0.0;
    if (len2 > 1e-9) t = std::clamp(((p - a).x() * ab.x() + (p - a).y() * ab.y()) / len2, 0.0, 1.0);
    const QPointF q = a + ab * t;
    const double dx = p.x() - q.x(), dy = p.y() - q.y();
    return std::sqrt(dx * dx + dy * dy);
}

// The ring in the horizontal plane through `origin`, as screen points. Points behind the camera are dropped.
std::vector<QPointF> ringPoints(const ukiyo::OrbitCamera& cam, int w, int h, glm::vec3 origin, float radius) {
    std::vector<QPointF> pts;
    for (int i = 0; i <= kRingSegments; ++i) {
        const float a = 2.0f * kPi * float(i) / float(kRingSegments);
        const glm::vec3 p = origin + radius * glm::vec3(std::sin(a), 0.0f, std::cos(a));
        QPointF s;
        if (project(cam, w, h, p, &s)) pts.push_back(s);
    }
    return pts;
}

} // namespace

bool project(const ukiyo::OrbitCamera& cam, int w, int h, glm::vec3 p, QPointF* out) {
    const float aspect = float(w) / float(std::max(h, 1));
    const glm::vec4 c = cam.proj(aspect) * cam.view() * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const double nx = c.x / c.w, ny = c.y / c.w;
    *out = QPointF((nx * 0.5 + 0.5) * w, (1.0 - (ny * 0.5 + 0.5)) * h);
    return true;
}

Ray screenRay(const ukiyo::OrbitCamera& cam, int w, int h, QPointF s) {
    const float aspect = float(w) / float(std::max(h, 1));
    const glm::mat4 inv = glm::inverse(cam.proj(aspect) * cam.view());
    const float nx = float(2.0 * s.x() / std::max(w, 1) - 1.0);
    const float ny = float(1.0 - 2.0 * s.y() / std::max(h, 1));
    glm::vec4 far4 = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
    const glm::vec3 farPoint = glm::vec3(far4) / far4.w;
    Ray r;
    r.origin = cam.eye();
    r.dir = glm::normalize(farPoint - r.origin);
    return r;
}

float handleLength(const ukiyo::OrbitCamera& cam, glm::vec3 origin) {
    const float d = std::max(glm::length(origin - cam.eye()), 1e-3f);
    // The view's height in world units at distance d, times the chosen fraction.
    return float(2.0 * d * std::tan(cam.fovY * 0.5) * kHandleFraction);
}

Part hitTest(const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin, QPointF mouse) {
    const float len = handleLength(cam, origin);
    if (tool == Tool::Scale) {
        QPointF c;
        if (!project(cam, w, h, origin, &c)) return Part::None;
        const double dx = mouse.x() - c.x(), dy = mouse.y() - c.y();
        return (dx * dx + dy * dy) <= (kHitPixels + 6.0) * (kHitPixels + 6.0) ? Part::Scale : Part::None;
    }
    if (tool == Tool::Move) {
        Part best = Part::None;
        double bestDist = kHitPixels;
        for (Part p : {Part::MoveX, Part::MoveY, Part::MoveZ}) {
            QPointF a, b;
            if (!project(cam, w, h, origin, &a) || !project(cam, w, h, origin + axisDir(p) * len, &b)) continue;
            const double d = distanceToSegment(mouse, a, b);
            if (d <= bestDist) {
                bestDist = d;
                best = p;
            }
        }
        return best;
    }
    const std::vector<QPointF> ring = ringPoints(cam, w, h, origin, len);
    for (size_t i = 1; i < ring.size(); ++i)
        if (distanceToSegment(mouse, ring[i - 1], ring[i]) <= kHitPixels) return Part::RotateYaw;
    return Part::None;
}

float axisParam(const Ray& r, glm::vec3 origin, Part axis) {
    // Closest approach of two lines: the ray (r.origin + s*r.dir) and the axis (origin + t*a).
    const glm::vec3 a = axisDir(axis);
    const glm::vec3 w = r.origin - origin;
    const float b = glm::dot(r.dir, a);
    const float denom = 1.0f - b * b;
    if (std::fabs(denom) < 1e-6f) return 0.0f;   // the ray runs parallel to the axis
    return (glm::dot(a, w) - b * glm::dot(r.dir, w)) / denom;
}

bool yawAngle(const Ray& r, glm::vec3 origin, float* thetaDeg) {
    if (std::fabs(r.dir.y) < 1e-6f) return false;
    const float t = (origin.y - r.origin.y) / r.dir.y;
    if (t < 0.0f) return false;
    const glm::vec3 hit = r.origin + t * r.dir;
    *thetaDeg = glm::degrees(std::atan2(hit.x - origin.x, hit.z - origin.z));
    return true;
}

glm::vec3 movedPosition(glm::vec3 origin0, Part axis, float t0, float t, float step) {
    glm::vec3 p = origin0 + axisDir(axis) * (t - t0);
    if (step > 0.0f) {
        const int k = axis == Part::MoveX ? 0 : axis == Part::MoveY ? 1 : 2;
        p[k] = std::round(p[k] / step) * step;
    }
    return p;
}

float rotatedYaw(float yaw0, float theta0, float theta, float stepDeg) {
    float yaw = yaw0 + wrapDeg(theta - theta0);
    if (stepDeg > 0.0f) yaw = std::round(yaw / stepDeg) * stepDeg;
    return yaw;
}

float scaledBy(float scale0, QPointF center, QPointF from, QPointF to, float step) {
    const double d0 = std::max(1.0, std::hypot(from.x() - center.x(), from.y() - center.y()));
    const double d1 = std::hypot(to.x() - center.x(), to.y() - center.y());
    float s = float(double(scale0) * (d1 / d0));
    if (step > 0.0f) s = std::round(s / step) * step;
    return std::clamp(s, 0.01f, 100.0f);
}

void paint(QPainter& p, const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin,
           Part hover, Part active) {
    const float len = handleLength(cam, origin);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    auto colourFor = [&](Part part, const QColor& base) {
        return (part == active || (active == Part::None && part == hover)) ? QColor(255, 224, 102) : base;
    };
    if (tool == Tool::Scale) {
        QPointF c;
        if (project(cam, w, h, origin, &c)) {
            const QColor base = colourFor(Part::Scale, QColor(62, 99, 221));
            p.setPen(QPen(base, (active == Part::Scale || (active == Part::None && hover == Part::Scale)) ? 3.5 : 2.0));
            p.setBrush(QColor(base.red(), base.green(), base.blue(), 70));
            // A square on the pivot with a cube at its corner, the way a scale handle reads at a glance.
            const double half = kHitPixels + 6.0;
            p.drawRect(QRectF(c.x() - half, c.y() - half, half * 2.0, half * 2.0));
            p.setBrush(base);
            p.drawRect(QRectF(c.x() + half - 4.0, c.y() + half - 4.0, 8.0, 8.0));
        }
        p.restore();
        return;
    }
    if (tool == Tool::Move) {
        const struct { Part part; QColor base; } axes[] = {
            {Part::MoveX, QColor(229, 72, 77)},
            {Part::MoveY, QColor(70, 167, 88)},
            {Part::MoveZ, QColor(62, 99, 221)},
        };
        for (const auto& ax : axes) {
            QPointF a, b;
            if (!project(cam, w, h, origin, &a) || !project(cam, w, h, origin + axisDir(ax.part) * len, &b)) continue;
            const QColor c = colourFor(ax.part, ax.base);
            const bool emphasised = ax.part == active || (active == Part::None && ax.part == hover);
            p.setPen(QPen(c, emphasised ? 4.0 : 2.5));
            p.drawLine(a, b);
            // Arrowhead: a triangle at the tip, pointing away from the origin.
            const QPointF dir = b - a;
            const double n = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
            if (n < 1e-3) continue;
            const QPointF u(dir.x() / n, dir.y() / n);
            const QPointF side(-u.y(), u.x());
            const double head = 12.0, half = 5.0;
            const QPointF base = b - u * head;
            QPolygonF tri;
            tri << b << base + side * half << base - side * half;
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPolygon(tri);
        }
    } else {
        const std::vector<QPointF> ring = ringPoints(cam, w, h, origin, len);
        if (ring.size() > 1) {
            const QColor c = colourFor(Part::RotateYaw, QColor(245, 208, 40));
            p.setPen(QPen(c, (active == Part::RotateYaw || (active == Part::None && hover == Part::RotateYaw)) ? 4.0 : 2.5));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(ring.data(), static_cast<int>(ring.size()));
        }
    }
    p.restore();
}

} // namespace editor::gizmo
