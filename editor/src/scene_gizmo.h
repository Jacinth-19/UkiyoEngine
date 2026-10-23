// Transform gizmos for the Scene view: a move handle (three axis arrows), a yaw ring, and a scale handle.
// The math is kept free of widgets so it can be tested directly. Screen points are widget pixels,
// with y pointing down, the same as the Scene view.
#pragma once

#include <glm/glm.hpp>

#include <QPointF>

#include "engine/camera.h"

class QPainter;

namespace editor::gizmo {

enum class Tool { Move, Rotate, Scale };
// The handle under the pointer or being dragged. Scale is one handle on the gizmo's centre: instances scale evenly, so
// there is no per-axis stretching to drag.
enum class Part { None, MoveX, MoveY, MoveZ, RotateYaw, Scale };

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 dir{0.0f, 0.0f, 1.0f};   // unit length
};

// Projects a world point to the view. Returns false when the point is behind the camera.
bool project(const ukiyo::OrbitCamera& cam, int w, int h, glm::vec3 p, QPointF* out);
// The ray through a screen point, starting at the eye.
Ray screenRay(const ukiyo::OrbitCamera& cam, int w, int h, QPointF s);
// Length of an arrow or ring radius. It is a fixed fraction of the view height at any distance.
float handleLength(const ukiyo::OrbitCamera& cam, glm::vec3 origin);
// The handle nearest the pointer within a few pixels, or Part::None.
Part hitTest(const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin, QPointF mouse);

// Parameter t of the point on the axis line (origin + axis * t) that is closest to the ray.
float axisParam(const Ray& r, glm::vec3 origin, Part axis);
// Angle in degrees of the ray's hit on the horizontal plane through `origin`, measured like yaw
// (atan2(x, z)). Returns false when the ray does not reach the plane.
bool yawAngle(const Ray& r, glm::vec3 origin, float* thetaDeg);

// New position while moving along one axis from `origin0`. `step` > 0 snaps that axis to the grid.
glm::vec3 movedPosition(glm::vec3 origin0, Part axis, float t0, float t, float step);
// New yaw in degrees. `stepDeg` > 0 snaps the result.
float rotatedYaw(float yaw0, float theta0, float theta, float stepDeg);
// The scale a drag of the scale handle asks for: how much further from the gizmo the pointer is now than where the
// drag started, times the scale it started at. `from` and `to` are widget pixels. A drag that would shrink an object
// below 1% stops there. `step` > 0 snaps the result (0.1 is a useful step).
float scaledBy(float scale0, QPointF center, QPointF from, QPointF to, float step);

// Draws the gizmo for the selected instance. `hover` is outlined in a highlight colour, and so is
// `active` while it is being dragged.
void paint(QPainter& p, const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin,
           Part hover, Part active);

} // namespace editor::gizmo
