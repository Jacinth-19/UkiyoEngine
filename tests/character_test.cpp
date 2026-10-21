// The capsule character (item 4): the controller a player uses.
//
// A rigid body cannot stand still on a slope without jitter and cannot climb a step at all, which is why the engine had
// no third-person controller: there was no capsule and no character solver. This drives Jolt's CharacterVirtual through
// the situations that define a controller - landing, walking, a wall, a step, a slope too steep to climb, and a jump -
// and checks the numbers, not just that the calls return true.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "systems/physics.h"

using namespace ukiyo;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static constexpr float kStep = 1.0f / 60.0f;

// Runs `seconds` of simulation at 60 Hz, calling `each` after every step.
template <typename Fn>
static void run(PhysicsWorld& w, CharacterHandle h, const glm::vec3& planar, bool jump, float seconds,
                Fn each) {
    const int steps = int(seconds / kStep);
    for (int i = 0; i < steps; ++i) {
        w.moveCharacter(h, planar, jump && i == 0, kStep, nullptr);
        each();
    }
}

int main() {
    // A floor big enough that nothing walks off it, plus a wall and a step.
    PhysicsWorld world;
    world.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    world.addStaticBox(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(20.0f, 0.5f, 20.0f));
    world.addStaticBox(glm::vec3(6.0f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 20.0f));    // a wall at x = 5.5
    world.addStaticBox(glm::vec3(-4.0f, 0.15f, 0.0f), glm::vec3(1.0f, 0.15f, 2.0f));  // a 0.3 m step from x=-5..-3

    CharacterSettings settings;
    settings.height = 1.8f;
    settings.radius = 0.3f;
    CharacterHandle h = world.addCharacter(glm::vec3(0.0f, 0.05f, 0.0f), settings);
    check(h != 0, "a character is created");
    check(world.hasCharacter(h), "the character exists");
    check(!world.hasCharacter(1234), "an unknown handle has no character");

    // Landing: dropped just above the floor, it settles with its feet on the floor and is grounded.
    run(world, h, glm::vec3(0.0f), false, 0.5f, [] {});
    CharacterState st;
    check(world.characterState(h, &st), "the state is readable");
    check(st.grounded, "the character lands and is grounded");
    check(std::fabs(st.position.y) < 0.06f, "the feet rest on the floor (y = " + std::to_string(st.position.y) + ")");
    check(st.groundNormal.y > 0.9f, "the ground normal points up");

    // Walking: 1 second at 3 m/s covers about 3 m. The tolerance covers acceleration and the fixed step; the point is
    // that it covers distance at the speed asked for, in the direction asked for.
    const float x0 = st.position.x;
    run(world, h, glm::vec3(3.0f, 0.0f, 0.0f), false, 1.0f, [] {});
    world.characterState(h, &st);
    const float walked = st.position.x - x0;
    check(walked > 2.6f && walked < 3.1f, "walking 3 m/s for 1 s covers about 3 m (" + std::to_string(walked) + ")");
    check(std::fabs(st.position.z) < 0.1f, "walking along x does not drift in z");
    check(st.grounded, "the character stays grounded while walking");

    // The wall stops it: its capsule radius keeps the centre off the wall face.
    run(world, h, glm::vec3(3.0f, 0.0f, 0.0f), false, 2.0f, [] {});
    world.characterState(h, &st);
    check(st.position.x < 5.5f - 0.3f + 0.05f, "the wall stops the character before its face");
    check(st.position.x > 4.0f, "the character reached the wall rather than stopping early");

    // A step: 0.3 m is under the 0.4 m step height, so walking onto it raises the feet.
    world.setCharacterYaw(h, 0.0f);
    run(world, h, glm::vec3(-3.0f, 0.0f, 0.0f), false, 3.2f, [] {});
    world.characterState(h, &st);
    check(st.position.x < -3.5f, "the character crossed to the step (x = " + std::to_string(st.position.x) + ")");
    check(st.position.y > 0.2f, "the character stands on top of the step (y = " + std::to_string(st.position.y) + ")");

    // A jump: it leaves the ground, rises around a metre, and lands again.
    float peak = 0.0f;
    bool leftGround = false;
    for (int i = 0; i < 90; ++i) {
        world.moveCharacter(h, glm::vec3(0.0f), i == 0, kStep, nullptr);
        world.characterState(h, &st);
        peak = std::max(peak, st.position.y);
        if (!st.grounded) leftGround = true;
    }
    check(leftGround, "a jump leaves the ground");
    check(peak > 0.5f, "a jump rises visibly (peak " + std::to_string(peak) + " m)");
    check(peak < 2.2f, "a jump does not launch the character into orbit");
    check(st.grounded, "the character lands again");

    // Too steep: a 60 degree ramp with a 50 degree limit is a wall. The character may slide, but it must not climb it.
    PhysicsWorld steep;
    steep.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    const float rise = std::tan(glm::radians(60.0f)) * 4.0f;
    std::vector<glm::vec3> ramp = {{0.0f, 0.0f, -3.0f}, {4.0f, rise, -3.0f}, {4.0f, rise, 3.0f},
                                   {0.0f, 0.0f, -3.0f}, {4.0f, rise, 3.0f},  {0.0f, 0.0f, 3.0f}};
    std::vector<uint32_t> rampIndex(ramp.size());
    for (size_t i = 0; i < ramp.size(); ++i) rampIndex[i] = static_cast<uint32_t>(i);
    steep.addStaticMesh(ramp, rampIndex);
    CharacterHandle c = steep.addCharacter(glm::vec3(0.3f, 0.05f, 0.0f), settings);
    // Stepping onto the ramp reports steep ground at once, and the character never rises up the slope.
    run(steep, c, glm::vec3(2.0f, 0.0f, 0.0f), false, 0.2f, [] {});
    CharacterState cs;
    steep.characterState(c, &cs);
    check(cs.onSteepGround, "a 60 degree slope is reported as steep ground");
    check(cs.groundNormal.y < 0.75f, "the steep ground's normal is the slope's (Ny = " + std::to_string(cs.groundNormal.y) + ")");
    run(steep, c, glm::vec3(2.0f, 0.0f, 0.0f), false, 1.0f, [] {});
    steep.characterState(c, &cs);
    check(cs.position.y < 1.2f, "a 60 degree slope cannot be climbed (y = " + std::to_string(cs.position.y) + ")");

    // Falling off an edge: no ground under it, so it falls.
    PhysicsWorld voidWorld;
    voidWorld.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    CharacterHandle falling = voidWorld.addCharacter(glm::vec3(0.0f, 5.0f, 0.0f), settings);
    run(voidWorld, falling, glm::vec3(0.0f), false, 0.5f, [] {});
    CharacterState fs;
    voidWorld.characterState(falling, &fs);
    check(!fs.grounded, "there is no ground, so the character is not grounded");
    check(fs.position.y < 4.0f, "the character falls");

    // A big dt is clamped rather than tunnelling through the floor.
    CharacterHandle fast = world.addCharacter(glm::vec3(0.0f, 0.05f, 2.0f), settings);
    world.moveCharacter(fast, glm::vec3(0.0f), false, 5.0f, nullptr);
    world.characterState(fast, &fs);
    check(fs.position.y > -1.0f, "a huge dt does not drop the character through the floor");

    // Housekeeping.
    world.removeCharacter(fast);
    check(!world.hasCharacter(fast), "a removed character is gone");
    check(!world.moveCharacter(fast, glm::vec3(0.0f), false, kStep, nullptr), "moving a removed character fails");
    check(!world.moveCharacter(9999, glm::vec3(0.0f), false, kStep, nullptr), "an unknown handle is refused");

    // Two characters do not share state: each has its own vertical velocity.
    CharacterHandle a = world.addCharacter(glm::vec3(-1.0f, 0.05f, 3.0f), settings);
    CharacterHandle b = world.addCharacter(glm::vec3(1.0f, 0.05f, 3.0f), settings);
    check(a != b, "each character gets its own handle");
    for (int i = 0; i < 30; ++i) {
        world.moveCharacter(a, glm::vec3(0.0f), i == 0, kStep, nullptr);
        world.moveCharacter(b, glm::vec3(0.0f), false, kStep, nullptr);
    }
    CharacterState sa, sb;
    world.characterState(a, &sa);
    world.characterState(b, &sb);
    check(sa.position.y != sb.position.y, "the jumping character is higher than the idle one");

    std::printf("character_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
