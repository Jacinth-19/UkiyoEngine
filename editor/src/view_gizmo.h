// The view gizmo: the axis widget in the corner of the Scene view.
//
// It is Blender's navigation gizmo (source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc), and the parts
// that make it work are taken from there:
//
//   * six handles, one per signed axis, laid out on a unit sphere and projected with the camera's rotation only — the
//     widget shows which way the view faces, not where the camera is;
//   * they are drawn **back to front**, and a handle on the far side is mixed toward the view's background colour by its
//     depth, so a ball behind the sphere reads as behind it;
//   * the axis pointing straight at the viewer cannot be clicked (it is under the one at the centre), so it is the
//     reverse one that takes the click — which is why Blender tests the opposite part when an axis is aligned;
//   * clicking a ball looks along that axis and switches the view to orthographic, which is what makes a view gizmo
//     useful for checking a silhouette.
//
// The layout and hit test are plain geometry over glm::vec3 plus QPointF, so they live here rather than in the widget.
#pragma once

#include <glm/glm.hpp>

#include <QColor>
#include <QPointF>
#include <QRectF>

#include "engine/camera.h"

class QPainter;

namespace editor::viewgizmo {

// One of the six signed axes: index 0..5 = +X, -X, +Y, -Y, +Z, -Z.
constexpr int kHandleCount = 6;

glm::vec3 handleDirection(int index);
const char* handleLabel(int index);      // "X", "Y", "Z" for the positive handles, empty for the negative ones
QColor handleColour(int index);          // axis colour: red X, green Y, blue Z

// Where the widget sits: a square in the top-right corner of the view, with a margin.
QRectF bounds(int viewWidth, int viewHeight);

// The screen position of a handle inside `box`, from the camera's rotation, and its depth (larger is nearer the viewer).
QPointF handlePoint(const ukiyo::OrbitCamera& cam, const QRectF& box, int index, float* depth);

// The handle under the pointer, or -1. Handles are tested nearest-first, and a near handle of the same axis wins over
// the far one — the far one is drawn dimmer and is behind it.
int hitTest(const ukiyo::OrbitCamera& cam, const QRectF& box, QPointF mouse);

// The yaw and pitch that look along `index`'s axis, and whether the view should be orthographic for it.
void viewFor(int index, float* yaw, float* pitch);

// Draws the widget. `hover` is the handle under the pointer, or -1; it is drawn in the highlight colour.
void paint(QPainter& p, const ukiyo::OrbitCamera& cam, const QRectF& box, int hover);

} // namespace editor::viewgizmo
