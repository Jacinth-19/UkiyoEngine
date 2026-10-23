// Orbit camera with pan and zoom. Yaw 0 looks at the target from +Z,
// the side VRM avatars face.
#pragma once

#include <glm/glm.hpp>

namespace ukiyo {

struct OrbitCamera {
    glm::vec3 target{0.0f, 0.95f, 0.0f};
    float yaw = 0.0f;          // radians around +Y
    float pitch = 0.10f;       // radians above the horizon
    float distance = 3.4f;     // metres from target
    float fovY = 0.52f;        // ~30 degrees
    float nearZ = 0.1f;
    float farZ = 400.0f;
    // Orthographic view, for blocking out a level: no perspective, so two objects of the same size read as the same size
    // wherever they are. `orthoHeight` is the world height the view covers; the width follows the viewport's aspect.
    // Everything else about the camera — target, yaw, pitch, distance — still applies, so a switch to ortho keeps the
    // view looking the same way.
    bool orthographic = false;
    float orthoHeight = 8.0f;
    // Fly: moving the target is how an orbit camera pans, but a level designer wants WASD to *travel*. These are the
    // increments for that, and they keep yaw and pitch, so the view turns with the movement like a fly camera should.
    void fly(float forwardAmt, float rightAmt, float upAmt);

    glm::vec3 eye() const;
    glm::vec3 forward() const;  // unit vector from eye to target
    glm::vec3 right() const;
    glm::vec3 up() const;
    glm::mat4 view() const;
    glm::mat4 proj(float aspect) const;

    void orbit(float dYaw, float dPitch);
    void zoom(float steps);                 // positive = closer
    void pan(float dxPixels, float dyPixels, float viewportHeight);
    void moveTargetPlanar(float forwardAmt, float rightAmt);   // WASD on the ground plane
    void moveTargetVertical(float amt);
};

} // namespace ukiyo
