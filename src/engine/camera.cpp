#include "engine/camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace ukiyo {

glm::vec3 OrbitCamera::eye() const {
    float cp = std::cos(pitch);
    return target + distance * glm::vec3(std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp);
}

glm::vec3 OrbitCamera::forward() const { return glm::normalize(target - eye()); }

glm::vec3 OrbitCamera::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 OrbitCamera::up() const { return glm::normalize(glm::cross(right(), forward())); }

glm::mat4 OrbitCamera::view() const {
    return glm::lookAtRH(eye(), target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::proj(float aspect) const {
    if (!orthographic) return glm::perspectiveRH_ZO(fovY, aspect, nearZ, farZ);
    // The eye stays where the orbit maths put it, so depth is linear between the same near and far planes; a designer
    // switching views sees the same objects in the same place, drawn without perspective.
    const float halfH = std::max(orthoHeight, 0.01f) * 0.5f;
    const float halfW = halfH * std::max(aspect, 0.01f);
    return glm::orthoRH_ZO(-halfW, halfW, -halfH, halfH, nearZ, farZ);
}

void OrbitCamera::fly(float forwardAmt, float rightAmt, float upAmt) {
    // A fly camera moves the eye and carries the target with it, so the orbit parameters stay valid and the next drag
    // continues from where the flight ended.
    const glm::vec3 move = forward() * forwardAmt + right() * rightAmt + glm::vec3(0.0f, upAmt, 0.0f);
    target += move;
}

void OrbitCamera::orbit(float dYaw, float dPitch) {
    yaw += dYaw;
    pitch = std::clamp(pitch + dPitch, -1.2f, 1.2f);
}

void OrbitCamera::zoom(float steps) {
    if (orthographic) {
        orthoHeight = std::clamp(orthoHeight * std::pow(0.9f, steps), 0.25f, 400.0f);
        return;
    }
    distance = std::clamp(distance * std::pow(0.9f, steps), 0.4f, 60.0f);
}

void OrbitCamera::pan(float dxPixels, float dyPixels, float viewportHeight) {
    // Scale so the content under the cursor follows the mouse. In ortho the visible height is the whole story.
    const float worldPerPixel = orthographic ? orthoHeight / std::max(viewportHeight, 1.0f)
                                             : 2.0f * distance * std::tan(fovY * 0.5f) / viewportHeight;
    target -= right() * (dxPixels * worldPerPixel);
    target += up() * (dyPixels * worldPerPixel);
}

void OrbitCamera::moveTargetPlanar(float forwardAmt, float rightAmt) {
    glm::vec3 f = forward();
    f.y = 0.0f;
    if (glm::length(f) > 1e-5f) f = glm::normalize(f);
    glm::vec3 r = right();
    r.y = 0.0f;
    if (glm::length(r) > 1e-5f) r = glm::normalize(r);
    target += f * forwardAmt + r * rightAmt;
}

void OrbitCamera::moveTargetVertical(float amt) { target.y += amt; }

} // namespace ukiyo
