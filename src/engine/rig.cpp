#include "engine/rig.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include "engine/model.h"

namespace ukiyo {
namespace {

glm::quat identityQuat() { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }

// Model-space world transform of a node (root removed).
glm::mat4 modelMatrix(const Model& m, const glm::mat4& invRoot, int node) {
    return invRoot * m.nodes[node].world;
}

glm::vec3 modelPos(const Model& m, const glm::mat4& invRoot, int node) {
    return glm::vec3(modelMatrix(m, invRoot, node)[3]);
}

glm::quat modelRot(const Model& m, const glm::mat4& invRoot, int node) {
    return rigQuatOf(modelMatrix(m, invRoot, node));
}

// Model-space rotation of a node's parent (identity for the top of the skeleton).
glm::quat parentModelRot(const Model& m, const glm::mat4& invRoot, int node) {
    const int p = m.nodes[node].parent;
    return p >= 0 ? modelRot(m, invRoot, p) : identityQuat();
}

// Writes the overlay that makes `node` reach `worldRot` (model space), given its parent's
// model-space rotation.
void setLocalFromWorld(Model& m, int node, const glm::quat& worldRot, const glm::quat& parentRot) {
    m.ikRot[node] = glm::normalize(glm::inverse(parentRot) * worldRot);
    m.ikActive[node] = 1;
}

} // namespace

glm::quat rigQuatOf(const glm::mat4& M) {
    glm::mat3 r(M);
    r[0] = glm::normalize(r[0]);
    r[1] = glm::normalize(r[1]);
    r[2] = glm::normalize(r[2]);
    return glm::normalize(glm::quat_cast(r));
}

TwoBoneSolution solveTwoBone(const glm::vec3& S, const glm::vec3& E0, const glm::vec3& W0,
                             const glm::quat& Ru, const glm::quat& Rl, const glm::quat& Rh,
                             const glm::vec3& T, const glm::vec3& P) {
    TwoBoneSolution out;
    const float L1 = glm::length(E0 - S), L2 = glm::length(W0 - E0);
    const glm::vec3 dirF0 = glm::normalize(W0 - E0);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    glm::vec3 toT = T - S;
    float d = glm::length(toT);
    const glm::vec3 dirT = d > 1e-5f ? toT / d : glm::normalize(E0 - S);
    d = std::clamp(d, std::fabs(L1 - L2) + 1e-4f, (L1 + L2) * 0.999f);
    const float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
    const float h = std::sqrt(std::max(L1 * L1 - a * a, 0.0f));
    const glm::vec3 pn = P - S;
    glm::vec3 perp = pn - glm::dot(pn, dirT) * dirT;
    if (glm::length(perp) < 1e-5f) perp = (E0 - S) - glm::dot(E0 - S, dirT) * dirT;
    if (glm::length(perp) < 1e-5f) perp = glm::cross(dirT, up);
    perp = glm::normalize(perp);
    out.E = S + dirT * a + perp * h;
    out.W = S + dirT * d;

    const glm::quat qU = rigRotBetween(glm::normalize(E0 - S), glm::normalize(out.E - S));
    out.Ru = glm::normalize(qU * Ru);
    const glm::quat qL = rigRotBetween(glm::normalize(qU * dirF0), glm::normalize(out.W - out.E));
    out.Rl = glm::normalize(qL * qU * Rl);
    out.Rh = glm::normalize(qL * qU * Rh);
    return out;
}

void Rig::evaluate(Model& m, float dt, const std::function<void()>& refresh) {
    if (!enabled || weight <= 0.0f) return;
    const glm::mat4 invRoot = glm::inverse(m.root);

    for (RigConstraint& c : constraints) {
        const float w = std::clamp(c.weight * weight, 0.0f, 1.0f);
        if (w <= 1e-4f || c.bones.empty()) continue;
        if (c.target < 0 || c.target >= static_cast<int>(targets.size())) continue;
        refresh();   // world transforms reflect the constraints already applied
        const RigTarget& tgt = targets[c.target];

        switch (c.type) {
        case ConstraintType::TwoBoneIK: {
            if (c.bones.size() < 3) break;
            const int root = c.bones[0], mid = c.bones[1], tip = c.bones[2];
            const glm::vec3 S = modelPos(m, invRoot, root);
            const glm::vec3 E0 = modelPos(m, invRoot, mid);
            const glm::vec3 W0 = modelPos(m, invRoot, tip);
            const glm::quat Ru = modelRot(m, invRoot, root);
            const glm::quat Rl = modelRot(m, invRoot, mid);
            const glm::quat Rh = modelRot(m, invRoot, tip);
            // Weight blends the target and the pole toward the animated pose; at weight 0 the
            // solve reproduces the animation.
            const glm::vec3 P = c.hint >= 0 && c.hint < static_cast<int>(targets.size())
                                    ? targets[c.hint].position : E0;
            const glm::vec3 Tw = glm::mix(W0, tgt.position, w);
            const glm::vec3 Pw = glm::mix(E0, P, w);
            const TwoBoneSolution sol = solveTwoBone(S, E0, W0, Ru, Rl, Rh, Tw, Pw);
            setLocalFromWorld(m, root, sol.Ru, parentModelRot(m, invRoot, root));
            setLocalFromWorld(m, mid, sol.Rl, sol.Ru);
            setLocalFromWorld(m, tip, sol.Rh, sol.Rl);
            break;
        }
        case ConstraintType::ChainIK: {
            // FABRIK: forward and backward reaching passes, root fixed.
            const size_t n = c.bones.size();
            if (n < 2) break;
            std::vector<glm::vec3> oldP(n), p(n);
            std::vector<glm::quat> oldR(n);
            std::vector<float> len(n - 1);
            for (size_t i = 0; i < n; ++i) {
                oldP[i] = modelPos(m, invRoot, c.bones[i]);
                oldR[i] = modelRot(m, invRoot, c.bones[i]);
                p[i] = oldP[i];
            }
            for (size_t i = 0; i + 1 < n; ++i) len[i] = glm::length(oldP[i + 1] - oldP[i]);
            const glm::vec3 goal = glm::mix(oldP[n - 1], tgt.position, w);
            const glm::vec3 rootPos = oldP[0];
            for (int iter = 0; iter < 16; ++iter) {
                p[n - 1] = goal;
                for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
                    const glm::vec3 dir = glm::normalize(p[i] - p[i + 1]);
                    p[i] = p[i + 1] + dir * len[i];
                }
                p[0] = rootPos;
                for (size_t i = 0; i + 1 < n; ++i) {
                    const glm::vec3 dir = glm::normalize(p[i + 1] - p[i]);
                    p[i + 1] = p[i] + dir * len[i];
                }
                if (glm::length(p[n - 1] - goal) < 1e-4f) break;
            }
            // New world rotations: each bone swings from its old child direction to the new one.
            std::vector<glm::quat> newR(n);
            for (size_t i = 0; i + 1 < n; ++i) {
                const glm::vec3 oldDir = glm::normalize(oldP[i + 1] - oldP[i]);
                const glm::vec3 newDir = glm::normalize(p[i + 1] - p[i]);
                newR[i] = glm::normalize(rigRotBetween(oldDir, newDir) * oldR[i]);
            }
            // The tip keeps its rotation relative to the previous bone.
            const glm::quat tipLocal = glm::inverse(oldR[n - 2]) * oldR[n - 1];
            newR[n - 1] = glm::normalize(newR[n - 2] * tipLocal);
            for (size_t i = 0; i < n; ++i) {
                const glm::quat parentRot = i == 0 ? parentModelRot(m, invRoot, c.bones[0]) : newR[i - 1];
                setLocalFromWorld(m, c.bones[i], newR[i], parentRot);
            }
            break;
        }
        case ConstraintType::MultiAim: {
            const int b = c.bones[0];
            const glm::vec3 P = modelPos(m, invRoot, b);
            const glm::quat R = modelRot(m, invRoot, b);
            const glm::vec3 toT = tgt.position - P;
            if (glm::length(toT) < 1e-5f) break;
            const glm::vec3 axisOld = glm::normalize(R * glm::normalize(c.aimAxis));
            const glm::quat delta = glm::slerp(identityQuat(),
                                               rigRotBetween(axisOld, glm::normalize(toT)), w);
            setLocalFromWorld(m, b, glm::normalize(delta * R), parentModelRot(m, invRoot, b));
            break;
        }
        case ConstraintType::DampedTransform: {
            const int b = c.bones[0];
            const glm::quat R = modelRot(m, invRoot, b);
            if (!c.dampedInit) {
                c.dampedRot = R;
                c.dampedInit = true;
            }
            if (dt > 0.0f) {
                const float k = 1.0f - std::exp(-c.stiffness * dt);
                c.dampedRot = glm::normalize(glm::slerp(c.dampedRot, tgt.rotation, k));
            }
            setLocalFromWorld(m, b, glm::normalize(glm::slerp(R, c.dampedRot, w)),
                              parentModelRot(m, invRoot, b));
            break;
        }
        case ConstraintType::Override: {
            const int b = c.bones[0];
            const glm::quat R = modelRot(m, invRoot, b);
            setLocalFromWorld(m, b, glm::normalize(glm::slerp(R, tgt.rotation, w)),
                              parentModelRot(m, invRoot, b));
            break;
        }
        }
    }
}

int findNodeByName(const Model& m, const std::string& name) {
    for (size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

Rig makeRoboArmRig(const Model& m, bool useChainIK) {
    Rig r;
    r.name = "robo_arm";
    const int upper = findNodeByName(m, "robo_upper_arm.L");
    const int mid = findNodeByName(m, "robo_forearm.L");
    const int tip = findNodeByName(m, "robo_hand.L");
    if (upper < 0 || mid < 0 || tip < 0) return r;
    r.targets.push_back(RigTarget{"reach", glm::vec3(0.0f), identityQuat()});
    RigConstraint c;
    c.name = useChainIK ? "robo_fabrik" : "robo_two_bone";
    c.type = useChainIK ? ConstraintType::ChainIK : ConstraintType::TwoBoneIK;
    c.bones = {upper, mid, tip};
    c.target = 0;
    r.constraints.push_back(c);
    return r;
}

} // namespace ukiyo
