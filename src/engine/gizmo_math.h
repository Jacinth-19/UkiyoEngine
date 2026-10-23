// The arithmetic behind the Scene view's gizmos, with no Qt in it.
//
// The editor draws its handles with QPainter, but the decisions a drag makes are geometry: where a ray meets a plane,
// how far along an axis the pointer has travelled, what angle a ring has been swept through. Those are here so they can
// be tested without a window, and so the window's code is drawing and nothing else.
//
// The structure follows Blender's gizmos (source/blender/editors/gizmo_library), which is the reference this editor's
// handles were built against:
//
//   * an axis is one of X, Y, Z or a *plane* between two of them, and a plane is culled from view by a different rule
//     than a line (transform_gizmo_3d.cc's g_tw_axis_range);
//   * a drag is measured from where it started, never accumulated frame by frame, so a drag that wanders and comes back
//     ends where it began;
//   * a ring drag can cross the ±180° seam of atan2, so the delta is unwrapped rather than trusted.
#pragma once

#include <vector>

#include <glm/glm.hpp>

namespace ukiyo::gizmo {

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 dir{0.0f, 0.0f, 1.0f};   // unit length
};

enum class Axis { X, Y, Z };

glm::vec3 axisDir(Axis a);
// The unit vector most nearly perpendicular to `axis`, chosen deterministically, so a ring drag's reference direction
// is the same on every frame and on every machine.
glm::vec3 perpendicular(const glm::vec3& axis);

// Where the ray meets the plane through `point` with normal `normal`, as a distance along the ray. False when the ray
// runs parallel to the plane, or meets it behind the eye.
bool rayPlane(const Ray& r, const glm::vec3& point, const glm::vec3& normal, float* t);

// The point on the plane under the cursor. Used to drag a plane handle: the pointer is free in the plane, and the
// movement is the difference between where it started and where it is.
bool planePoint(const Ray& r, const glm::vec3& point, const glm::vec3& normal, glm::vec3* hit);

// Parameter along the line (point + axis * t) of the point closest to the ray: the axis drag's measurement.
float axisParam(const Ray& r, const glm::vec3& point, const glm::vec3& axis);

// The angle in degrees of the ray's hit on the plane through `point` with normal `axis`, measured from `reference`.
// False when the ray does not reach the plane.
bool angleAboutAxis(const Ray& r, const glm::vec3& point, const glm::vec3& axis, const glm::vec3& reference,
                    float* deg);

// `a - b` wrapped into (-180, 180]. A drag that crosses the seam gets the short way round, which is what the hand did.
float deltaDeg(float a, float b);

// Blender's visibility rule (gizmo_get_idot in transform_gizmo_3d.cc), for a handle along `axis` seen from a camera
// looking along `viewDir`. A line nearly parallel to the view is hidden because it is drawn as a dot; a plane nearly
// edge-on is hidden earlier, because a plane seen edge-on is a line that cannot be grabbed. Returns 0..1: 1 is fully
// facing the viewer.
float axisVisibility(const glm::vec3& viewDir, const glm::vec3& axis, bool plane);
bool axisVisible(const glm::vec3& viewDir, const glm::vec3& axis, bool plane);

// A scale handle's factor change for a drag. `axisPixels` is the handle's screen direction and length (the drawn gizmo's
// axis, in pixels); dragging `from` -> `to` along it changes the scale by the fraction of that length travelled, which
// is how Blender's scale handles behave: drag the cube a handle's length out and the object doubles.
float scaleDelta(const glm::vec2& from, const glm::vec2& to, const glm::vec2& axisPixels, float* alongPixels);

// The uniform handle: the ratio of the pointer's distance from the gizmo's centre now to what it was at the start.
float scaleRadial(const glm::vec2& centre, const glm::vec2& from, const glm::vec2& to);

// Snaps a scale to `step` when step > 0. Keeps an object above a hundredth of its size.
float snapScale(float scale, float step);

// Snaps an angle in degrees to `stepDeg` when stepDeg > 0.
float snapAngle(float deg, float stepDeg);

// ---- A whole selection at once -----------------------------------------------------------------------------------
// The gizmo edits every object in the selection, not only the active one. The rules are here rather than in the window
// because they are the part that is easy to get subtly wrong: a turn has to swing each position around the shared pivot
// *and* compose with the rotation each object already had, and a scale has to carry the positions out from the pivot as
// well as multiply the scale. With one object selected the pivot is that object's own position, so all three of these
// do exactly what the single-object drag did.

struct Placement {
    glm::vec3 position{0.0f};
    glm::vec3 rotationDeg{0.0f};   // x = pitch, y = yaw, z = roll, the engine's ZXY order
    glm::vec3 scale{1.0f};
};

// The point a multi-object drag turns and scales about: the middle of the selection's positions.
glm::vec3 selectionPivot(const std::vector<glm::vec3>& positions);

// A move drag: every object moves by the same delta, so the selection keeps its shape.
glm::vec3 movedBy(const glm::vec3& position, const glm::vec3& delta);

// A turn about the pivot by `deltaDeg` (one angle at a time, as the rings give it): the position swings around the
// pivot, and the turned angle composes with the rotation the object already had rather than being added to one of its
// Euler angles, which is what makes a tilted object stay tilted the way the hand expects.
Placement rotatedAbout(const Placement& p, const glm::vec3& pivot, const glm::vec3& deltaDeg);

// A scale about the pivot. The factor is per axis - an X handle passes (f, 1, 1) - the position moves out from the
// pivot by it, and the scale multiplies by it. Neither the factor nor the scale it lands on goes below a hundredth, so
// a drag cannot flatten a selection into a plane it can never be unflattened from.
Placement scaledAbout(const Placement& p, const glm::vec3& pivot, const glm::vec3& factor);

} // namespace ukiyo::gizmo
