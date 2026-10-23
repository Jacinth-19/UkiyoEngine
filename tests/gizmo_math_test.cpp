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

    std::printf("gizmo_math_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
