// The orbit camera: projection, the orthographic view, and flying.
//
// The editor needs two views of one scene — a Scene view for building and a Game view for playing — and a Scene view
// that can be orthographic and flown through. Those are properties of the camera, so they are checked here rather than
// by looking at a screenshot.
#include "engine/camera.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace ukiyo;

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
bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    OrbitCamera cam;
    cam.target = glm::vec3(0.0f, 1.0f, 0.0f);
    cam.distance = 6.0f;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;

    // --- Perspective ------------------------------------------------------------------------------
    {
        const glm::mat4 p = cam.proj(16.0f / 9.0f);
        check(near(p[0][0], 1.0f / (std::tan(cam.fovY * 0.5f) * (16.0f / 9.0f)), 1e-4f),
              "the perspective x scale follows the fov and aspect");
        check(near(p[1][1], 1.0f / std::tan(cam.fovY * 0.5f), 1e-4f), "the y scale follows the fov");
    }

    // --- Orthographic: the same view, no perspective -----------------------------------------------
    {
        cam.orthographic = true;
        cam.orthoHeight = 8.0f;
        const float aspect = 16.0f / 9.0f;
        const glm::mat4 o = cam.proj(aspect);
        const float halfW = 4.0f * aspect;
        check(near(o[0][0], 1.0f / halfW, 1e-5f), "ortho x scale is the inverse of the half width");
        check(near(o[1][1], 1.0f / 4.0f, 1e-5f), "ortho y scale is the inverse of the half height");
        check(near(o[3][3], 1.0f, 1e-6f), "ortho has no perspective divide");
        // A nearer object is not bigger: two points with the same x at different depths project to the same clip x.
        const glm::mat4 view = cam.view();
        const glm::vec4 a = o * view * glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
        const glm::vec4 b = o * view * glm::vec4(1.0f, 1.0f, -3.0f, 1.0f);
        check(near(a.x / a.w, b.x / b.w, 1e-4f), "depth does not change where a point lands in an ortho view");
        // Distance from the target does not change the projected scale, which is the point of ortho.
        const float s1 = o[1][1];
        cam.distance = 12.0f;
        const float s2 = cam.proj(aspect)[1][1];
        check(near(s1, s2), "moving the eye back does not zoom an orthographic view");
    }

    // --- Zoom in ortho changes the visible height, not the distance --------------------------------
    {
        cam.orthographic = true;
        cam.orthoHeight = 8.0f;
        cam.distance = 6.0f;
        cam.zoom(2.0f);
        check(cam.orthoHeight < 8.0f, "zooming in shrinks the visible height");
        check(near(cam.distance, 6.0f), "and leaves the eye where it was");
        cam.zoom(-100.0f);
        check(cam.orthoHeight <= 400.0f, "a zoom out is bounded");
    }

    // --- Pan follows the mouse in both projections -------------------------------------------------
    {
        OrbitCamera ortho;
        ortho.orthographic = true;
        ortho.orthoHeight = 10.0f;
        ortho.yaw = 0.0f;
        ortho.pitch = 0.0f;
        const glm::vec3 before = ortho.target;
        ortho.pan(0.0f, 20.0f, 100.0f);   // 20 of 100 pixels = a fifth of the visible height
        check(near(std::fabs(ortho.target.y - before.y), 2.0f, 1e-3f),
              "an ortho pan moves by the visible height, not by the perspective distance",
              std::to_string(std::fabs(ortho.target.y - before.y)));
    }

    // --- Fly: travel along the view, and keep the orbit parameters ---------------------------------
    {
        OrbitCamera f;
        f.target = glm::vec3(0.0f);
        f.yaw = 0.0f;
        f.pitch = 0.0f;
        f.distance = 5.0f;
        const glm::vec3 e0 = f.eye();
        f.fly(2.0f, 0.0f, 0.0f);
        check(near(glm::length(f.eye() - e0), 2.0f, 1e-4f), "flying forward travels the distance asked for");
        check(f.yaw == 0.0f && f.distance == 5.0f, "and the view does not change");
        const glm::vec3 e1 = f.eye();
        f.fly(0.0f, 0.0f, 3.0f);
        check(near(f.eye().y - e1.y, 3.0f, 1e-4f), "flying up rises");
        // Forward is the camera's own direction, so a turned camera flies where it looks.
        f.yaw = 1.5707963f;   // 90 degrees: forward is -X
        const glm::vec3 e2 = f.eye();
        f.fly(1.0f, 0.0f, 0.0f);
        check(f.target.x < e2.x + 1e-3f, "a turned camera flies where it looks");
    }

    std::printf("camera_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
