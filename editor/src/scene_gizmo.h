// The Scene view's transform gizmo: move, rotate and scale handles, drawn with QPainter in screen space.
//
// The handle set is Blender's (source/blender/editors/transform/transform_gizmo_3d.cc), which is the reference this was
// built against:
//
//   * **move**: three axis arrows and three plane handles between them (XY, YZ, ZX);
//   * **rotate**: one ring per axis, the far half of each ring dimmed so the ring reads as a circle in space;
//   * **scale**: three axis handles with a cube at the end, plus a cube on the centre for uniform scale.
//
// Two rules are taken from the same place. A handle pointing at the viewer is not drawn (it would be a dot, and it would
// steal clicks from the ones you can see), and a plane seen edge-on is dropped earlier than a line would be. And a handle
// under the pointer or being dragged is drawn wider, the way Blender biases a hovered gizmo's line width.
//
// The arithmetic a drag needs — ray/plane, axis travel, ring angles, the visibility dot, scale factors — lives in
// `engine/gizmo_math.h`, without Qt, so it is tested on its own.
#pragma once

#include <glm/glm.hpp>

#include <QPointF>

#include <vector>

#include "engine/camera.h"
#include "engine/gizmo_math.h"

class QPainter;

namespace editor::gizmo {

enum class Tool { Move, Rotate, Scale };

// The handle under the pointer or being dragged. Plane handles are named by the plane they lie in: MoveYZ is the plane
// whose normal is X.
enum class Part {
    None,
    MoveX, MoveY, MoveZ,
    MoveYZ, MoveZX, MoveXY,
    RotateX, RotateY, RotateZ,
    ScaleX, ScaleY, ScaleZ, ScaleUniform,
};

using Ray = ukiyo::gizmo::Ray;

// Projects a world point to the view. Returns false when the point is behind the camera.
bool project(const ukiyo::OrbitCamera& cam, int w, int h, glm::vec3 p, QPointF* out);
// The ray through a screen point, starting at the eye.
Ray screenRay(const ukiyo::OrbitCamera& cam, int w, int h, QPointF s);
// Length of an arrow or ring radius, in pixels: a fixed fraction of the view height.
float handleLength(const ukiyo::OrbitCamera& cam, glm::vec3 origin);

// The handle under the pointer, or Part::None. Handles that are hidden by the view rule are not hit either.
Part hitTest(const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin, QPointF mouse);

// True when a handle belongs to the tool being used.
bool partInTool(Part part, Tool tool);
// The world axis a handle lies along (for a plane handle, its normal; for the uniform handle, zero).
glm::vec3 partAxis(Part part);
// True for the three plane handles.
bool isPlanePart(Part part);
// The plane handle whose normal is `axis`.
Part planePartFor(Axis axis);

// The angle in degrees of the ray's hit on the ring around `axis` through `origin`, from a stable reference direction.
bool ringAngle(const Ray& r, glm::vec3 origin, glm::vec3 axis, float* deg);
// The point on a plane handle's plane under the pointer.
bool planePoint(const Ray& r, glm::vec3 origin, Part plane, glm::vec3* hit);
// The parameter along an axis handle.
float axisParam(const Ray& r, glm::vec3 origin, Part axis);
// New position while moving along one axis from `origin0`. `step` > 0 snaps that axis to the grid.
glm::vec3 movedPosition(glm::vec3 origin0, Part axis, float t0, float t, float step);
// New position while moving in a plane handle's plane, from where the drag began and the pointer's hit now.
glm::vec3 movedInPlane(glm::vec3 from, glm::vec3 hit0, glm::vec3 hit, float step);
// New angle in degrees for a ring drag that swept `theta0` -> `theta` from a starting angle of `startDeg`. `stepDeg` > 0
// snaps the result.
float rotatedAngle(float startDeg, float theta0, float theta, float stepDeg);

// Draws the gizmo for the selected instance. `hover` is outlined in a highlight colour, and so is `active` while it is
// being dragged.
void paint(QPainter& p, const ukiyo::OrbitCamera& cam, int w, int h, Tool tool, glm::vec3 origin, Part hover,
           Part active);

} // namespace editor::gizmo
