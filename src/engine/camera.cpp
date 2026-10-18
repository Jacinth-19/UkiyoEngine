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
    return glm::perspectiveRH_ZO(fovY, aspect, nearZ, farZ);
}

void OrbitCamera::orbit(float dYaw, float dPitch) {
    yaw += dYaw;
    pitch = std::clamp(pitch + dPitch, -1.2f, 1.2f);
}

void OrbitCamera::zoom(float steps) {
    distance = std::clamp(distance * std::pow(0.9f, steps), 0.4f, 60.0f);
}

void OrbitCamera::pan(float dxPixels, float dyPixels, float viewportHeight) {
    // Scale so the content under the cursor follows the mouse.
    float worldPerPixel = 2.0f * distance * std::tan(fovY * 0.5f) / viewportHeight;
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
