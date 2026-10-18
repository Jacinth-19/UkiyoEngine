// Animation rigging: a stack of constraints that run after the animation pose and before
// the final forward kinematics, in the spirit of Unity's Animation Rigging package.
//
// A Rig is one layer. It holds targets (points or rotations in model space, set by code
// each frame) and constraints evaluated in order:
//   TwoBoneIK   root/mid/tip chain reaches a target, with a pole (elbow/knee) hint.
//   ChainIK     FABRIK solver over a chain of any length.
//   MultiAim    a bone's aim axis points at a target position.
//   DampedTransform  a bone's rotation follows a target rotation with damping.
//   Override    a bone's rotation is blended to a target rotation.
// Each constraint and the layer have a weight in [0, 1]. Results are written to the model's
// IK overlays, so they compose with the animation and with the arm collision solver.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ukiyo {

struct Model;

// Shortest-arc rotation from a to b (both unit vectors).
inline glm::quat rigRotBetween(const glm::vec3& a, const glm::vec3& b) {
    const float d = glm::dot(a, b);
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

// Rotation part of a model-space matrix (scale removed).
glm::quat rigQuatOf(const glm::mat4& M);

// Two-bone solve in model space. S is the root joint, E0/W0 the animated mid and tip joints,
// Ru/Rl/Rh their animated world rotations, T the target for the tip, and P the pole point
// (the elbow or knee side). Returns the solved joint positions and world rotations. When
// T == W0 and P == E0 the animated pose comes back unchanged.
struct TwoBoneSolution {
    glm::vec3 E{0.0f}, W{0.0f};
    glm::quat Ru{1.0f, 0.0f, 0.0f, 0.0f}, Rl{1.0f, 0.0f, 0.0f, 0.0f}, Rh{1.0f, 0.0f, 0.0f, 0.0f};
};
TwoBoneSolution solveTwoBone(const glm::vec3& S, const glm::vec3& E0, const glm::vec3& W0,
                             const glm::quat& Ru, const glm::quat& Rl, const glm::quat& Rh,
                             const glm::vec3& T, const glm::vec3& P);

enum class ConstraintType { TwoBoneIK, ChainIK, MultiAim, DampedTransform, Override };

struct RigConstraint {
    std::string name;
    ConstraintType type = ConstraintType::TwoBoneIK;
    std::vector<int> bones;       // model node indices: root, mid, tip (TwoBone), chain (ChainIK), bone (others)
    int target = 0;               // index into Rig::targets
    int hint = -1;                // optional pole target index (TwoBoneIK); -1 keeps the animated elbow
    glm::vec3 aimAxis{0.0f, 1.0f, 0.0f};   // local aim axis (MultiAim)
    float stiffness = 12.0f;      // DampedTransform response (1/s)
    float weight = 1.0f;
    // Runtime state.
    glm::quat dampedRot{1.0f, 0.0f, 0.0f, 0.0f};
    bool dampedInit = false;
};

// Target in model space (position and rotation). Written by the application each frame.
struct RigTarget {
    std::string name;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

class Rig {
public:
    std::string name = "rig";
    float weight = 1.0f;          // layer weight
    bool enabled = true;
    std::vector<RigTarget> targets;
    std::vector<RigConstraint> constraints;

    // Evaluates all constraints in order. `refresh` recomputes node world transforms with the
    // current overlays, so each constraint sees the previous one's result.
    void evaluate(Model& m, float dt, const std::function<void()>& refresh);
};

// Rig builders for the robotic arm that Seed-san carries on its backpack. The arm has joints
// robo_upper_arm.L, robo_forearm.L and robo_hand.L. Returns an empty rig when the model has
// no such nodes.
Rig makeRoboArmRig(const Model& m, bool useChainIK);

// Finds a node by its glTF name. Returns -1 when absent.
int findNodeByName(const Model& m, const std::string& name);

} // namespace ukiyo
