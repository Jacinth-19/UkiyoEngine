// The gizmo arithmetic: rays, planes, axis drags, ring angles, Blender's visibility rule, and scale handles.
#include "engine/gizmo_math.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ukiyo::gizmo;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
std::string fmt(const glm::vec3& v) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return b;
}
} // namespace

int main() {
    // --- Ray meets plane ---------------------------------------------------------------------------
    {
        Ray r;
        r.origin = glm::vec3(0.0f, 5.0f, 0.0f);
        r.dir = glm::vec3(0.0f, -1.0f, 0.0f);
        float t = 0.0f;
        check(rayPlane(r, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), &t), "a ray straight down meets the floor");
        check(near(t, 5.0f), "at five metres", std::to_string(t));
        glm::vec3 hit(0.0f);
        check(planePoint(r, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f, 1.0f, 0.0f), &hit) && near(hit.y, 2.0f),
              "the plane's own height is where it is hit");
        // A plane seen edge-on cannot be hit.
        r.dir = glm::vec3(1.0f, 0.0f, 0.0f);
        check(!rayPlane(r, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), &t), "a ray along the plane misses it");
        // A plane behind the eye is not a hit either.
        r.dir = glm::vec3(0.0f, 1.0f, 0.0f);
        check(!rayPlane(r, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), &t), "a plane behind the eye is behind");
    }

    // --- Axis drag parameter -----------------------------------------------------------------------
    {
        Ray r;                                  // from (2, 3, 10) looking down -Z: passes over x=2, y=3
        r.origin = glm::vec3(2.0f, 3.0f, 10.0f);
        r.dir = glm::vec3(0.0f, 0.0f, -1.0f);
        check(near(axisParam(r, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)), 2.0f), "the X parameter is the ray's x");
        check(near(axisParam(r, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), 3.0f), "the Y parameter is the ray's y");
        // A ray along the axis has no unique closest point; the answer is zero rather than a division by zero.
        r.dir = glm::vec3(1.0f, 0.0f, 0.0f);
        check(near(axisParam(r, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)), 0.0f), "a parallel ray gives zero");
    }

    // --- Ring angle ---------------------------------------------------------------------------------
    {
        // Looking down onto the XZ plane from +Y, at the origin.
        Ray r;
        r.origin = glm::vec3(0.0f, 10.0f, 0.0f);
        r.dir = glm::vec3(0.0f, -1.0f, 0.0f);
        float deg = 0.0f;
        const glm::vec3 yawAxis(0.0f, 1.0f, 0.0f);
        const glm::vec3 reference(0.0f, 0.0f, 1.0f);   // +Z is angle zero
        check(angleAboutAxis(r, glm::vec3(0.0f), yawAxis, reference, &deg), "a ray onto the ring meets it");
        check(near(deg, 0.0f, 0.01f), "below the eye, on +Z, is zero degrees", std::to_string(deg));
        r.origin = glm::vec3(10.0f, 10.0f, 0.0f);       // off to the side: the ray still hits the plane at +X
        check(angleAboutAxis(r, glm::vec3(0.0f), yawAxis, reference, &deg) && near(deg, 90.0f, 0.01f),
              "on +X, the ring reads ninety degrees", std::to_string(deg));
        // An edge-on ray cannot be read: that is the ring you cannot grab, which is why the ring is culled.
        Ray edge;
        edge.origin = glm::vec3(10.0f, 0.0f, 0.0f);
        edge.dir = glm::vec3(-1.0f, 0.0f, 0.0f);
        check(!angleAboutAxis(edge, glm::vec3(0.0f), yawAxis, reference, &deg), "a ray along the ring's plane misses it");
    }

    // --- Wrapping ------------------------------------------------------------------------------------
    {
        check(near(deltaDeg(10.0f, 350.0f), 20.0f), "a drag across the seam is the short way round");
        check(near(deltaDeg(350.0f, 10.0f), -20.0f), "and it is signed");
        check(near(deltaDeg(-179.0f, 179.0f), 2.0f), "-179 from 179 is two degrees, not 358");
        check(deltaDeg(180.0f, 0.0f) > 0.0f, "180 stays inside the range");
    }

    // --- Blender's visibility rule -------------------------------------------------------------------
    {
        const glm::vec3 axis(1.0f, 0.0f, 0.0f);
        check(axisVisible(glm::vec3(0, 0, -1), axis, false), "an axis across the view is drawn");
        check(!axisVisible(glm::vec3(-1, 0, 0), axis, false), "an axis pointing straight at the viewer is hidden");
        // A plane whose normal points at the viewer is the face-on one: visible, and easiest to grab.
        check(axisVisible(glm::vec3(-1, 0, 0), axis, true), "the plane facing the viewer is drawn");
        check(!axisVisible(glm::vec3(0, 0, -1), axis, true), "the plane seen edge-on is hidden");
        // The plane threshold is earlier than the line's, as in transform_gizmo_3d.cc.
        const glm::vec3 tilted = glm::normalize(glm::vec3(0.05f, 0.0f, -1.0f));   // |dot| ~ 0.05 with X
        check(axisVisible(tilted, axis, false), "a line at five percent is still drawn");
        check(!axisVisible(tilted, axis, true), "a plane at five percent is hidden");
    }

    // --- Scale handles -------------------------------------------------------------------------------
    {
        float along = 0.0f;
        const glm::vec2 axisPixels(100.0f, 0.0f);       // a handle 100 px long, pointing +x
        check(near(scaleDelta(glm::vec2(10, 10), glm::vec2(110, 10), axisPixels, &along), 1.0f),
              "dragging a handle's own length doubles the scale");
        check(near(along, 100.0f), "and the travel is the distance along the handle", std::to_string(along));
        check(near(scaleDelta(glm::vec2(10, 10), glm::vec2(10, 90), axisPixels, &along), 0.0f),
              "dragging across the handle does not scale");
        check(near(scaleDelta(glm::vec2(10, 10), glm::vec2(85, 10), axisPixels, &along), 0.75f),
              "three quarters of the way is three quarters bigger");
        // The uniform handle measures distance from the centre.
        check(near(scaleRadial(glm::vec2(0, 0), glm::vec2(10, 0), glm::vec2(20, 0)), 2.0f), "twice as far, twice as big");
        check(near(scaleRadial(glm::vec2(0, 0), glm::vec2(10, 0), glm::vec2(0, 5)), 0.5f), "half as far, half as big");
        // A pointer exactly on the centre would divide by nothing.
        check(scaleRadial(glm::vec2(0, 0), glm::vec2(0, 0), glm::vec2(5, 0)) > 0.0f, "the centre does not divide by zero");
        // Snapping and the floor.
        check(near(snapScale(1.03f, 0.1f), 1.0f), "a scale snaps to its step");
        check(snapScale(0.0001f, 0.0f) >= 0.01f, "and never reaches zero");
        check(near(snapAngle(7.0f, 5.0f), 5.0f), "an angle snaps to its step");
        check(near(snapAngle(7.0f, 0.0f), 7.0f), "and is left alone without one");
    }

    // --- Perpendicular reference is stable ------------------------------------------------------------
    {
        const glm::vec3 a = perpendicular(glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 b = perpendicular(glm::vec3(0.0f, 1.0f, 0.0f));
        check(a == b, "the same axis gives the same reference every time");
        check(near(glm::dot(a, glm::vec3(0.0f, 1.0f, 0.0f)), 0.0f), "and it is perpendicular");
        check(near(glm::length(a), 1.0f), "and unit length");
    }

    // --- A whole selection at once ---------------------------------------------------------------------
    {
        // The pivot is the middle of the selection; with one object, it is that object.
        check(near3(selectionPivot({}), glm::vec3(0.0f)), "an empty selection pivots at the origin");
        check(near3(selectionPivot({glm::vec3(1, 0, 0), glm::vec3(3, 0, 0)}), glm::vec3(2, 0, 0)),
              "two objects pivot midway between them");
        check(near3(selectionPivot({glm::vec3(-2, 4, 0), glm::vec3(0, -2, 6), glm::vec3(2, 1, 0)}), glm::vec3(0, 1, 2)),
              "three objects pivot at the mean of their positions");

        // A move carries every object by the same delta, so the selection keeps its shape.
        const glm::vec3 a = movedBy(glm::vec3(1, 2, 3), glm::vec3(0.5f, -1.0f, 0.0f));
        const glm::vec3 b = movedBy(glm::vec3(4, 2, 3), glm::vec3(0.5f, -1.0f, 0.0f));
        check(near3(a, glm::vec3(1.5f, 1, 3)) && near3(b - a, glm::vec3(3, 0, 0)),
              "a move drag keeps the gap between two objects");

        // A turn swings the positions around the pivot . . .
        Placement left;
        left.position = glm::vec3(2, 0, 0);
        Placement right;
        right.position = glm::vec3(4, 0, 0);
        const glm::vec3 pivot = selectionPivot({left.position, right.position});   // (3, 0, 0)
        const Placement l2 = rotatedAbout(left, pivot, glm::vec3(0, 90, 0));
        const Placement r2 = rotatedAbout(right, pivot, glm::vec3(0, 90, 0));
        check(near3(l2.position, glm::vec3(3, 0, 1)) && near3(r2.position, glm::vec3(3, 0, -1)),
              "a quarter turn about the pivot swings two objects round it",
              fmt(l2.position) + " " + fmt(r2.position));
        check(near(glm::length(r2.position - l2.position), 2.0f),
              "and they are still as far apart as they were");
        // . . . and composes with the rotation each one already had.
        Placement tilted;
        tilted.rotationDeg = glm::vec3(30, 0, 0);
        const Placement twice = rotatedAbout(tilted, glm::vec3(0.0f), glm::vec3(0, 90, 0));
        check(near(twice.rotationDeg.x, 30.0f, 0.01f) && near(twice.rotationDeg.y, 90.0f, 0.01f),
              "a yaw on top of a pitch keeps the pitch", fmt(twice.rotationDeg));
        // One object turning about its own position does not move.
        Placement solo;
        solo.position = glm::vec3(5, 1, 0);
        const Placement spun = rotatedAbout(solo, solo.position, glm::vec3(0, 45, 0));
        check(near3(spun.position, solo.position), "one object selected turns where it stands");
        check(near(spun.rotationDeg.y, 45.0f), "and takes the turned angle", fmt(spun.rotationDeg));

        // A scale carries the positions out from the pivot and multiplies the scale.
        const Placement ls = scaledAbout(left, pivot, glm::vec3(2, 2, 2));
        const Placement rs = scaledAbout(right, pivot, glm::vec3(2, 2, 2));
        check(near3(ls.position, glm::vec3(1, 0, 0)) && near3(rs.position, glm::vec3(5, 0, 0)),
              "doubling a selection pushes the objects twice as far from the pivot",
              fmt(ls.position) + " " + fmt(rs.position));
        Placement big;
        big.scale = glm::vec3(0.5f, 1.0f, 1.0f);
        const Placement bigger = scaledAbout(big, glm::vec3(0.0f), glm::vec3(2, 1, 1));
        check(near(bigger.scale.x, 1.0f) && near(bigger.scale.y, 1.0f), "one axis handle scales one axis",
              fmt(bigger.scale));
        const Placement flat = scaledAbout(big, glm::vec3(0.0f), glm::vec3(0, 0, 0));
        check(flat.scale.x >= 0.01f, "a snap to nothing cannot flatten a selection away", fmt(flat.scale));
        Placement single;
        single.position = glm::vec3(3, 0, 0);
        check(near3(scaledAbout(single, single.position, glm::vec3(3, 3, 3)).position, single.position),
              "one object selected scales where it stands");
    }

    std::printf("gizmo_math_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
