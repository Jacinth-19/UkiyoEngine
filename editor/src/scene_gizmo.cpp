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

using ukiyo::gizmo::deltaDeg;

constexpr double kHitPixels = 8.0;          // how close the pointer must be to a handle
constexpr int kRingSegments = 96;
constexpr double kHandleFraction = 0.18;    // handle length as a fraction of the view height
constexpr double kPlaneOffset = 0.34;       // a plane handle's square, as a fraction of the handle length

glm::vec3 axisDir(Part p) {
    switch (p) {
    case Part::MoveX:
    case Part::RotateX:
    case Part::ScaleX: return glm::vec3(1.0f, 0.0f, 0.0f);
    case Part::MoveY:
    case Part::RotateY:
    case Part::ScaleY: return glm::vec3(0.0f, 1.0f, 0.0f);
    case Part::MoveZ:
    case Part::RotateZ:
    case Part::ScaleZ: return glm::vec3(0.0f, 0.0f, 1.0f);
    default: return glm::vec3(0.0f);
    }
}

// The plane handles keep Blender's ordering: the handle between two axes is named by the plane, and its normal is the
// third axis.
glm::vec3 planeNormal(Part p) {
    switch (p) {
    case Part::MoveYZ: return glm::vec3(1.0f, 0.0f, 0.0f);
    case Part::MoveZX: return glm::vec3(0.0f, 1.0f, 0.0f);
    case Part::MoveXY: return glm::vec3(0.0f, 0.0f, 1.0f);
    default: return glm::vec3(0.0f);
    }
}

QColor baseColour(Part p) {
    // A plane handle takes its colour from the axis it *faces*, not the axes it spans: the square between +Y and +Z is
    // the X plane, and says X.
    const glm::vec3 a = isPlanePart(p) ? planeNormal(p) : partAxis(p);
    if (a.x > 0.5f) return QColor(229, 72, 77);     // X
    if (a.y > 0.5f) return QColor(70, 167, 88);     // Y
    if (a.z > 0.5f) return QColor(62, 99, 221);     // Z
    return QColor(200, 200, 200);                    // uniform scale
}

double distanceToSegment(QPointF p, QPointF a, QPointF b) {
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1e-9) return std::hypot(p.x() - a.x(), p.y() - a.y());
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q(a.x() + ab.x() * t, a.y() + ab.y() * t);
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

// The view direction, for the visibility rule.
glm::vec3 viewDir(const ukiyo::OrbitCamera& cam) { return cam.forward(); }

bool visible(const ukiyo::OrbitCamera& cam, Part p) {
    if (isPlanePart(p)) return ukiyo::gizmo::axisVisible(viewDir(cam), planeNormal(p), true);
    return ukiyo::gizmo::axisVisible(viewDir(cam), partAxis(p), false);
}

// A ring in the plane perpendicular to `axis`, drawn from `radius` in world units.
std::vector<QPointF> ringPoints(const ukiyo::OrbitCamera& cam, int w, int h, glm::vec3 origin, glm::vec3 axis,
                                float radius) {
    std::vector<QPointF> out;
    const glm::vec3 a = glm::normalize(axis);
    const glm::vec3 u = ukiyo::gizmo::perpendicular(a);
    const glm::vec3 v = glm::cross(a, u);
    out.reserve(kRingSegments + 1);
    for (int i = 0; i <= kRingSegments; ++i) {
        const float t = 6.2831853f * float(i) / float(kRingSegments);
        const glm::vec3 p = origin + radius * (std::cos(t) * u + std::sin(t) * v);
        QPointF s;
        if (!project(cam, w, h, p, &s)) return std::vector<QPointF>();
        out.push_back(s);
    }
    return out;
}

// The four corners of a plane handle's square, in world space: centred at `origin + offset * (a + b)` and spanning a
// small square of the plane spanned by axes a and b.
std::vector<glm::vec3> planeQuad(glm::vec3 origin, Part plane, float len) {
    const glm::vec3 n = planeNormal(plane);
    // The two axes inside the plane, in a fixed order so the square does not flip between frames.
    const glm::vec3 a = std::abs(n.x) > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 b = glm::cross(n, a);
    const float off = float(kPlaneOffset) * len;
    const float half = len * 0.09f;
    const glm::vec3 c = origin + (a + b) * off;
    return {c - a * half - b * half, c + a * half - b * half, c + a * half + b * half, c - a * half + b * half};
}

bool insideQuad(const std::vector<QPointF>& q, QPointF p) {
    if (q.size() != 4) return false;
    bool in = false;
    for (size_t i = 0, j = 3; i < 4; j = i++) {
        const bool up = (q[i].y() > p.y()) != (q[j].y() > p.y());
        if (up && p.x() < (q[j].x() - q[i].x()) * (p.y() - q[i].y()) / (q[j].y() - q[i].y()) + q[i].x()) in = !in;
    }
    return in;
}

} // namespace

glm::vec3 partAxis(Part part) {
    if (isPlanePart(part)) return glm::vec3(0.0f);   // a plane has no single axis; planeNormal is the one it needs
    return axisDir(part);
}

bool isPlanePart(Part part) { return part == Part::MoveYZ || part == Part::MoveZX || part == Part::MoveXY; }

bool project(const ukiyo::OrbitCamera& cam, int w, int h, glm::vec3 p, QPointF* out) {
    const glm::mat4 vp = cam.proj(float(w) / float(std::max(h, 1))) * cam.view();
    const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
    if (clip.w <= 1e-6f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (out) *out = QPointF((ndc.x * 0.5 + 0.5) * w, (0.5 - ndc.y * 0.5) * h);
    return true;
}

Ray screenRay(const ukiyo::OrbitCamera& cam, int w, int h, QPointF s) {
    // The inverse of the projection at that pixel, in world space.
    const float aspect = float(w) / float(std::max(h, 1));
    const glm::mat4 inv = glm::inverse(cam.proj(aspect) * cam.view());
    const float x = float((s.x() / std::max(w, 1)) * 2.0 - 1.0);
    const float y = float(1.0 - (s.y() / std::max(h, 1)) * 2.0);
    const glm::vec4 farPoint = inv * glm::vec4(x, y, 1.0f, 1.0f);
    const glm::vec4 nearPoint = inv * glm::vec4(x, y, -1.0f, 1.0f);
    Ray r;
    r.origin = cam.eye();
    r.dir = glm::normalize(glm::vec3(farPoint) / farPoint.w - glm::vec3(nearPoint) / nearPoint.w);
    return r;
}

float handleLength(const ukiyo::OrbitCamera& cam, glm::vec3 origin) {
    // A fixed fraction of the height the view covers at the gizmo's distance: the handles stay the same size on screen
    // however far the object is, which is what makes them targets.
    const float dist = std::max(glm::length(origin - cam.eye()), 0.05f);
    const float worldPerPixel = cam.orthographic ? cam.orthoHeight / 600.0f
                                                 : 2.0f * dist * std::tan(cam.fovY * 0.5f) / 600.0f;
    return float(kHandleFraction * 600.0 * worldPerPixel);
}

Part hitTest(const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin, QPointF mouse) {
    const float len = handleLength(cam, origin);
    QPointF centre;
    const bool centreOk = project(cam, w, h, origin, &centre);
    if (!centreOk) return Part::None;

    // The nearest handle wins, as it does in Blender: a hit is a distance in pixels, and the smallest one is the one the
    // hand meant. Highlights widen the line but not the test.
    Part best = Part::None;
    double bestDist = kHitPixels;

    if (tool == Tool::Move) {
        for (Part p : {Part::MoveX, Part::MoveY, Part::MoveZ}) {
            if (!visible(cam, p)) continue;
            QPointF a, b;
            if (!project(cam, w, h, origin, &a) || !project(cam, w, h, origin + axisDir(p) * len, &b)) continue;
            const double d = distanceToSegment(mouse, a, b);
            if (d <= bestDist) {
                bestDist = d;
                best = p;
            }
        }
        for (Part p : {Part::MoveYZ, Part::MoveZX, Part::MoveXY}) {
            if (!visible(cam, p)) continue;
            const std::vector<glm::vec3> quad = planeQuad(origin, p, len);
            std::vector<QPointF> screen(4);
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) ok = project(cam, w, h, quad[size_t(i)], &screen[size_t(i)]);
            if (ok && insideQuad(screen, mouse)) return p;   // a square under the pointer is an unambiguous hit
        }
        return best;
    }

    if (tool == Tool::Rotate) {
        for (Part p : {Part::RotateX, Part::RotateY, Part::RotateZ}) {
            const std::vector<QPointF> ring = ringPoints(cam, w, h, origin, partAxis(p), len);
            for (size_t i = 1; i < ring.size(); ++i) {
                const double d = distanceToSegment(mouse, ring[i - 1], ring[i]);
                if (d <= bestDist) {
                    bestDist = d;
                    best = p;
                }
            }
        }
        return best;
    }

    // Scale: the centre cube first, because it sits on top of the axis lines, then the axes.
    const double half = kHitPixels + 6.0;
    if (std::hypot(mouse.x() - centre.x(), mouse.y() - centre.y()) <= half) return Part::ScaleUniform;
    for (Part p : {Part::ScaleX, Part::ScaleY, Part::ScaleZ}) {
        if (!visible(cam, p)) continue;
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

bool ringAngle(const Ray& r, glm::vec3 origin, glm::vec3 axis, float* deg) {
    const glm::vec3 reference = ukiyo::gizmo::perpendicular(axis);
    return ukiyo::gizmo::angleAboutAxis(r, origin, axis, reference, deg);
}

bool planePoint(const Ray& r, glm::vec3 origin, Part plane, glm::vec3* hit) {
    return ukiyo::gizmo::planePoint(r, origin, planeNormal(plane), hit);
}

float axisParam(const Ray& r, glm::vec3 origin, Part axis) {
    return ukiyo::gizmo::axisParam(r, origin, axisDir(axis));
}

glm::vec3 movedPosition(glm::vec3 origin0, Part axis, float t0, float t, float step) {
    glm::vec3 p = origin0 + axisDir(axis) * (t - t0);
    if (step > 0.0f) {
        const glm::vec3 a = axisDir(axis);
        const int k = a.x > 0.5f ? 0 : a.y > 0.5f ? 1 : 2;
        p[k] = std::round(p[k] / step) * step;
    }
    return p;
}

glm::vec3 movedInPlane(glm::vec3 from, glm::vec3 hit0, glm::vec3 hit, float step) {
    glm::vec3 p = from + (hit - hit0);
    if (step > 0.0f) {
        for (int k = 0; k < 3; ++k) p[k] = std::round(p[k] / step) * step;
    }
    return p;
}

float rotatedAngle(float startDeg, float theta0, float theta, float stepDeg) {
    return ukiyo::gizmo::snapAngle(startDeg + deltaDeg(theta, theta0), stepDeg);
}

void paint(QPainter& p, const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin, Part hover,
           Part active) {
    const float len = handleLength(cam, origin);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    // A hovered or dragged handle is drawn wider: Blender's select bias, which is how a gizmo says what it is about to
    // move.
    auto widthFor = [&](Part part, double base) {
        const bool lit = part == active || (active == Part::None && part == hover);
        return lit ? base + 1.75 : base;
    };
    auto colourFor = [&](Part part) {
        return (part == active || (active == Part::None && part == hover)) ? QColor(255, 224, 102) : baseColour(part);
    };

    if (tool == Tool::Move) {
        // Plane handles are drawn first, so the arrows are on top of them at the centre.
        for (Part plane : {Part::MoveYZ, Part::MoveZX, Part::MoveXY}) {
            if (!visible(cam, plane)) continue;
            const std::vector<glm::vec3> quad = planeQuad(origin, plane, len);
            std::vector<QPointF> screen(4);
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) ok = project(cam, w, h, quad[size_t(i)], &screen[size_t(i)]);
            if (!ok) continue;
            const QColor c = colourFor(plane);
            QColor fill = c;
            fill.setAlpha(55);
            p.setPen(QPen(c, widthFor(plane, 2.0)));
            p.setBrush(fill);
            p.drawPolygon(screen.data(), 4);
        }
        p.setBrush(Qt::NoBrush);
        for (Part axis : {Part::MoveX, Part::MoveY, Part::MoveZ}) {
            if (!visible(cam, axis)) continue;
            QPointF a, b;
            if (!project(cam, w, h, origin, &a) || !project(cam, w, h, origin + axisDir(axis) * len, &b)) continue;
            const QColor c = colourFor(axis);
            p.setPen(QPen(c, widthFor(axis, 2.5)));
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
            p.setBrush(Qt::NoBrush);
        }
    } else if (tool == Tool::Rotate) {
        // Three rings, one per axis. The half of each ring that passes behind the gizmo's origin is drawn faint: that is
        // what makes a flat ring read as a circle seen at an angle, and it is Blender's treatment of a dial.
        const glm::vec3 toward = cam.forward();
        for (Part axis : {Part::RotateX, Part::RotateY, Part::RotateZ}) {
            const glm::vec3 a = partAxis(axis);
            const glm::vec3 u = ukiyo::gizmo::perpendicular(a);
            const glm::vec3 v = glm::cross(a, u);
            QPointF prev;
            bool havePrev = false;
            bool prevFront = true;
            for (int i = 0; i <= kRingSegments; ++i) {
                const float t = 6.2831853f * float(i) / float(kRingSegments);
                const glm::vec3 world = origin + len * (std::cos(t) * u + std::sin(t) * v);
                QPointF s;
                if (!project(cam, w, h, world, &s)) {
                    havePrev = false;
                    continue;
                }
                const bool front = glm::dot(world - origin, toward) <= 0.0f;   // nearer the camera than the origin
                if (havePrev && front == prevFront) {
                    QColor c = colourFor(axis);
                    if (!front) c.setAlpha(70);
                    p.setPen(QPen(c, widthFor(axis, 2.5)));
                    p.drawLine(prev, s);
                }
                prev = s;
                prevFront = front;
                havePrev = true;
            }
        }
    } else {
        // Scale: axis lines with a cube at the end, and a cube at the centre for uniform scale.
        for (Part axis : {Part::ScaleX, Part::ScaleY, Part::ScaleZ}) {
            if (!visible(cam, axis)) continue;
            QPointF a, b;
            if (!project(cam, w, h, origin, &a) || !project(cam, w, h, origin + axisDir(axis) * len, &b)) continue;
            const QColor c = colourFor(axis);
            p.setPen(QPen(c, widthFor(axis, 2.5)));
            p.drawLine(a, b);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(QRectF(b.x() - 4.5, b.y() - 4.5, 9.0, 9.0));
        }
        QPointF centre;
        if (project(cam, w, h, origin, &centre)) {
            const QColor c = colourFor(Part::ScaleUniform);
            const double half = kHitPixels + 2.0;
            p.setPen(QPen(c, widthFor(Part::ScaleUniform, 2.0)));
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 70));
            p.drawRect(QRectF(centre.x() - half, centre.y() - half, half * 2.0, half * 2.0));
        }
    }
    p.restore();
}

} // namespace editor::gizmo
