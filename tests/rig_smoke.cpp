// Smoke test for the rig constraints (MultiAim, DampedTransform, Override) on Seed-san's
// robotic arm. Each check runs one constraint in isolation through updateModel and reads the
// resulting world transforms. Exit code 0 when all checks pass.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/model.h"
#include "engine/rig.h"
#include "rhi/rhi.h"

using namespace ukiyo;

static int g_fail = 0;

static void check(bool ok, const char* what, float value, float tol) {
    std::printf("[%s] %-52s value %.5f (tol %.5f)\n", ok ? "PASS" : "FAIL", what, value, tol);
    if (!ok) ++g_fail;
}

static float angleBetween(const glm::quat& a, const glm::quat& b) {
    return glm::angle(glm::normalize(a) * glm::inverse(glm::normalize(b)));
}

static float vecAngle(const glm::vec3& a, const glm::vec3& b) {
    return std::acos(std::fmin(1.0f, std::fmax(-1.0f, glm::dot(glm::normalize(a), glm::normalize(b)))));
}

static glm::vec3 worldPos(const Model& m, int node) { return glm::vec3(m.nodes[node].world[3]); }

// World-space direction of a local axis of a node.
static glm::vec3 worldAxis(const Model& m, int node, const glm::vec3& localAxis) {
    return glm::normalize(glm::mat3(m.nodes[node].world) * localAxis);
}

int main(int argc, char** argv) {
    const std::string modelPath = argc > 1 ? argv[1] : "assets/Seed-san.vrm";
    try {
        rhi::DeviceConfig cfg;
        cfg.width = 320;
        cfg.height = 180;
        cfg.headless = true;
        auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
        auto model = loadModel(*device, modelPath);
        model->physics = false;
        model->armCollision = false;
        const glm::mat4 invRoot = glm::inverse(model->root);
        const glm::vec3 eye(0.0f, 1.0f, 3.0f);

        const int hand = findNodeByName(*model, "robo_hand.L");
        const int fore = findNodeByName(*model, "robo_forearm.L");
        if (hand < 0 || fore < 0) {
            std::fprintf(stderr, "robo arm nodes not found\n");
            return 2;
        }

        // Settle once with no rig so the pose is the animated rest pose.
        float t = 0.0f;
        updateModel(*model, t, eye);
        const glm::vec3 handRest = worldPos(*model, hand);
        const glm::quat foreRest = rigQuatOf(model->nodes[fore].world);
        const glm::vec3 handRestAxis = worldAxis(*model, hand, glm::vec3(0, 1, 0));

        // ---- MultiAim: hand's +Y axis points at a point in front of the robo arm. ----
        {
            const glm::vec3 goalWorld = handRest + glm::vec3(0.4f, 0.3f, 0.5f);
            const glm::vec3 wantDir = glm::normalize(goalWorld - handRest);
            for (float weight : {1.0f, 0.5f, 0.0f}) {
                model->rigs.clear();
                Rig r;
                r.targets.push_back(RigTarget{"aim", glm::vec3(invRoot * glm::vec4(goalWorld, 1.0f)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
                RigConstraint c;
                c.name = "aim";
                c.type = ConstraintType::MultiAim;
                c.bones = {hand};
                c.target = 0;
                c.aimAxis = glm::vec3(0.0f, 1.0f, 0.0f);
                c.weight = weight;
                r.constraints.push_back(c);
                model->rigs.push_back(r);
                updateModel(*model, t += 1.0f / 60.0f, eye);
                const float alignErr = vecAngle(worldAxis(*model, hand, glm::vec3(0, 1, 0)), wantDir);
                const float restErr = vecAngle(handRestAxis, wantDir);
                if (weight == 1.0f)
                    check(alignErr < 0.01f, "MultiAim w=1 aims axis at target", alignErr, 0.01f);
                else if (weight == 0.0f)
                    check(std::fabs(alignErr - restErr) < 0.01f, "MultiAim w=0 leaves pose unchanged", alignErr, 0.01f);
                else
                    check(alignErr < restErr && alignErr > 0.01f,
                          "MultiAim w=0.5 partially aims (between rest and full)", alignErr, 0.01f);
            }
        }

        // ---- Override: forearm rotation is blended to a target rotation. ----
        {
            const glm::quat want = glm::angleAxis(0.9f, glm::normalize(glm::vec3(0.3f, 0.2f, 1.0f)));
            for (float weight : {1.0f, 0.5f, 0.0f}) {
                model->rigs.clear();
                Rig r;
                r.targets.push_back(RigTarget{"ovr", glm::vec3(0.0f), want});
                RigConstraint c;
                c.name = "ovr";
                c.type = ConstraintType::Override;
                c.bones = {fore};
                c.target = 0;
                c.weight = weight;
                r.constraints.push_back(c);
                model->rigs.push_back(r);
                updateModel(*model, t += 1.0f / 60.0f, eye);
                const glm::quat got = rigQuatOf(model->nodes[fore].world);
                const float err = angleBetween(got, want);
                const float restAngle = angleBetween(foreRest, want);
                if (weight == 1.0f)
                    check(err < 0.01f, "Override w=1 matches target rotation", err, 0.01f);
                else if (weight == 0.0f)
                    check(std::fabs(err - restAngle) < 0.01f, "Override w=0 leaves pose unchanged", err, 0.01f);
                else
                    check(err > 0.01f && err < restAngle, "Override w=0.5 lies between rest and target", err, 0.01f);
            }
        }

        // ---- DampedTransform: rotation follows the target with a lag, then converges. ----
        {
            const glm::quat want = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 0.0f, 0.4f)));
            model->rigs.clear();
            Rig r;
            r.targets.push_back(RigTarget{"damp", glm::vec3(0.0f), want});
            RigConstraint c;
            c.name = "damp";
            c.type = ConstraintType::DampedTransform;
            c.bones = {fore};
            c.target = 0;
            c.stiffness = 12.0f;
            c.weight = 1.0f;
            r.constraints.push_back(c);
            model->rigs.push_back(r);
            // The first frame initialises the damped state at the current pose (dt = 0 there).
            updateModel(*model, t += 1.0f / 60.0f, eye);
            const float errStart = angleBetween(rigQuatOf(model->nodes[fore].world), want);
            updateModel(*model, t += 1.0f / 60.0f, eye);
            const float errOne = angleBetween(rigQuatOf(model->nodes[fore].world), want);
            check(errOne < errStart && errOne > 1e-3f, "DampedTransform lags after one frame", errOne, 1e-3f);
            for (int i = 0; i < 240; ++i) updateModel(*model, t += 1.0f / 60.0f, eye);
            const float errEnd = angleBetween(rigQuatOf(model->nodes[fore].world), want);
            check(errEnd < 0.01f, "DampedTransform converges after 4 s", errEnd, 0.01f);
        }

        // Hand stays attached to the forearm during the checks (sanity of the chain).
        const float attach = glm::length(worldPos(*model, hand) - worldPos(*model, fore));
        check(attach > 0.1f && attach < 1.0f, "robo hand still attached to forearm (m)", attach, 1.0f);

        std::printf("%s: %d failure(s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
        return g_fail == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
