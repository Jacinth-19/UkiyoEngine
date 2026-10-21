// Physics on Jolt Physics. Bodies are addressed by a plain uint32 handle (0 is "no body").
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ukiyo {

struct WaterVolume;

// A capsule character (item 4) moved by Jolt's CharacterVirtual: it slides along walls, climbs steps up to stepHeight
// and stands on slopes up to maxSlopeDeg. It is kinematic, not a rigid body: a rigid body cannot stand still on a slope
// without jitter, and cannot climb a step at all. This is the controller a third-person or first-person player uses.
struct CharacterSettings {
    float radius = 0.3f;        // metres
    float height = 1.8f;        // metres, including both caps
    float maxSlopeDeg = 50.0f;  // steeper ground is a wall
    float stepHeight = 0.4f;    // the tallest ledge the character walks up
    float stickToFloor = 0.35f; // downward snap that keeps it on a slope instead of hopping down it
    float jumpSpeed = 5.0f;     // metres per second at the moment of a jump (about 1.2 m of height)
    float mass = 70.0f;         // used for the push it applies to dynamic bodies
};

using CharacterHandle = uint32_t;   // 0 is "no character", like PhysicsWorld::Handle

struct CharacterState {
    glm::vec3 position{0.0f};       // the feet
    glm::vec3 velocity{0.0f};       // metres per second, after the last move
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    bool grounded = false;
    bool onSteepGround = false;     // touching a slope too steep to stand on
};


class PhysicsWorld {
public:
    using Handle = uint32_t;
    static constexpr Handle kNone = 0;

    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void setGravity(const glm::vec3& g);
    glm::vec3 gravity() const;
    // Fixed step in seconds (default 1/60). Steps are clamped to [1/120, 1/20].
    void setFixedStep(float seconds);
    float fixedStep() const;
    // Jolt's solver iterations per step (default 10 velocity, 2 position). Each is clamped to [1, 32].
    void setSolverSteps(int velocity, int position);
    int velocitySolverSteps() const;
    int positionSolverSteps() const;

    // Static geometry: a box, or a triangle mesh (the same triangles a navigation mesh is built from).
    Handle addStaticBox(const glm::vec3& center, const glm::vec3& halfExtents);
    Handle addStaticMesh(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices);
    // Dynamic bodies move under gravity and collide with everything.
    Handle addDynamicSphere(const glm::vec3& position, float radius, float mass);
    Handle addDynamicBox(const glm::vec3& position, const glm::vec3& halfExtents, float mass);
    void removeBody(Handle h);

    glm::vec3 position(Handle h) const;
    glm::quat rotation(Handle h) const;
    void setPosition(Handle h, const glm::vec3& p);
    void addImpulse(Handle h, const glm::vec3& impulse);
    bool isDynamic(Handle h) const;

    // Interpolation (on by default): position() and rotation() of a dynamic body blend between its last two
    // fixed steps, so motion stays smooth when the render rate differs from the step rate. setPosition
    // moves the body at once with no blend.
    void setInterpolation(bool on);
    bool interpolation() const;

    // Water (item 17). While a volume is set, each dynamic body whose centre lies inside its rectangle gets a
    // buoyancy impulse on every fixed step (Jolt's ApplyBuoyancyImpulse). The surface is fitted at four points on
    // the body's footprint: mean wave height and mean wave normal. setWaterTime sets the wave clock in seconds; use
    // the renderer's clock so the boats float on the waves that are drawn. nullptr removes the water.
    void setWater(const WaterVolume* volume);
    void setWaterTime(float seconds);

    // Advances the simulation by dt seconds in fixed sub-steps of 1/60 s.
    void step(float dt);
    int bodyCount() const { return bodies_; }

    // A contact that began (added) or ended (removed) during the last step. Jolt raises these on its job threads,
    // so they are queued there and handed out by takeContacts(), on the calling thread, after step().
    // point and normal are zero for removed contacts. Persisting contacts are not reported.
    struct Contact {
        bool added = false;
        Handle a = kNone, b = kNone;
        glm::vec3 point{0.0f}, normal{0.0f};
    };
    std::vector<Contact> takeContacts();

    // While on, setPosition, addImpulse and removeBody are queued. The queue runs at the start of the next step(),
    // outside Jolt's update. The world turns this on while contact handlers run, because Jolt forbids body
    // changes from its contact callbacks.
    void setDeferChanges(bool on);

    // Turns collisions between two bodies off (enabled = false) or on again. Only new contacts are blocked: a pair
    // that already touches separates when it moves apart. Call it on the main thread, not from a contact callback.
    void setPairEnabled(Handle a, Handle b, bool enabled);

    // ---- Character (item 4) -----------------------------------------------------------------------------------
    // A capsule character moved by Jolt's CharacterVirtual. See CharacterSettings and CharacterState above.
    // `feet` is the ground point the capsule stands on.
    CharacterHandle addCharacter(const glm::vec3& feet, const CharacterSettings& settings = CharacterSettings());
    void removeCharacter(CharacterHandle h);
    bool hasCharacter(CharacterHandle h) const;

    // Moves the character by `planarVelocity` for dt, under gravity, jumping when `jump` is set and it is grounded.
    // The vertical velocity is kept across calls, so a jump arcs. Returns false for an unknown handle.
    bool moveCharacter(CharacterHandle h, const glm::vec3& planarVelocity, bool jump, float dt,
                       CharacterState* out = nullptr);
    bool characterState(CharacterHandle h, CharacterState* out) const;
    // Faces the character along yaw (radians about +Y). The capsule is round, but the direction is what scripts and
    // the animation use, and a character standing on a moving platform must keep it.
    void setCharacterYaw(CharacterHandle h, float yawRadians);

    // First hit along the ray within maxDistance. Returns false when nothing is hit.
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, glm::vec3* hit,
                 Handle* body = nullptr) const;

public:
    struct Impl;   // defined in physics.cpp
    void applyDeferred();
private:
    std::unique_ptr<Impl> impl_;
    int bodies_ = 0;
};

} // namespace ukiyo
