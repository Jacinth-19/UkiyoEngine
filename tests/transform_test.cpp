// Transform math: ZXY Euler angles, TRS composition, hierarchy resolution, cycle rejection and decomposition.
// Built without a GPU so it runs everywhere: g++ -std=c++17 -I src -I third_party/glm tests/transform_test.cpp
// src/engine/transform.cpp
#include "engine/transform.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ukiyo;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool near3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

bool nearMat(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!near(a[c][r], b[c][r], eps)) return false;
    return true;
}

std::string fmt(const glm::vec3& v) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return b;
}

void testIdentityAndTranslation() {
    Transform t;
    check(nearMat(localMatrix(t), glm::mat4(1.0f)), "identity transform is the identity matrix");
    t.position = {2.0f, -1.0f, 0.5f};
    const glm::vec4 p = localMatrix(t) * glm::vec4(0, 0, 0, 1);
    check(near3(glm::vec3(p), t.position), "translation moves the origin", fmt(glm::vec3(p)));
}

void testYawMatchesLegacy() {
    // The scene files written before this change encoded position + yaw. The new matrix must agree exactly, otherwise
    // every existing scene would shift by a degree or a mirror.
    for (float yaw = -180.0f; yaw <= 180.0f; yaw += 15.0f) {
        Transform t;
        t.position = {3.0f, 2.0f, -4.0f};
        t.rotationDeg = {0.0f, yaw, 0.0f};
        check(nearMat(localMatrix(t), placementOf(t.position, yaw), 1e-5f),
              "yaw-only transform equals the legacy placement", std::to_string(yaw));
    }
}

void testRotationOrderIsZxy() {
    // R = Ry * Rx * Rz. With pitch 90 and yaw 90, applying roll last must differ from roll first, which is the whole
    // reason the order is pinned down in the header.
    Transform a;
    a.rotationDeg = {90.0f, 90.0f, 45.0f};
    const glm::mat4 m = localMatrix(a);
    const glm::mat4 expected = rotationMatrix({0, 90, 0}) * rotationMatrix({90, 0, 0}) * rotationMatrix({0, 0, 45});
    check(nearMat(m, expected), "rotation composition is Ry * Rx * Rz");
    const glm::mat4 other = rotationMatrix({0, 0, 45}) * rotationMatrix({90, 0, 0}) * rotationMatrix({0, 90, 0});
    check(!nearMat(m, other), "ZXY order is distinguishable from ZYX");
}

void testAxisDirections() {
    // Right-handed X rotation: pitching +90 turns forward (0,0,1) to down (0,-1,0). Asserted here so a convention
    // change breaks a test rather than a scene.
    Transform t;
    t.rotationDeg = {90.0f, 0.0f, 0.0f};
    const glm::vec3 fwd = glm::vec3(localMatrix(t) * glm::vec4(0, 0, 1, 0));
    check(near3(fwd, glm::vec3(0, -1, 0), 1e-5f), "pitch +90 points forward down", fmt(fwd));
    t.rotationDeg = {0.0f, 90.0f, 0.0f};
    const glm::vec3 right = glm::vec3(localMatrix(t) * glm::vec4(1, 0, 0, 0));
    check(near3(right, glm::vec3(0, 0, -1), 1e-5f), "yaw +90 turns right towards -Z", fmt(right));
    t.rotationDeg = {0.0f, 0.0f, 90.0f};
    const glm::vec3 up = glm::vec3(localMatrix(t) * glm::vec4(0, 1, 0, 0));
    check(near3(up, glm::vec3(-1, 0, 0), 1e-5f), "roll +90 turns up towards -X", fmt(up));
}

void testScale() {
    Transform t;
    t.scale = {2.0f, 3.0f, 0.5f};
    const glm::vec3 p = glm::vec3(localMatrix(t) * glm::vec4(1, 1, 1, 1));
    check(near3(p, glm::vec3(2, 3, 0.5f)), "scale multiplies each axis", fmt(p));
}

void testDecomposeRoundTrip() {
    const glm::vec3 angles[] = {{0, 0, 0}, {10, 25, -15}, {-40, 170, 5}, {89, -60, 30}, {-89, 10, -120}};
    for (const glm::vec3& e : angles) {
        Transform t;
        t.position = {1, 2, 3};
        t.rotationDeg = e;
        t.scale = {1.5f, 0.75f, 2.0f};
        Transform back;
        check(decompose(localMatrix(t), &back), "decompose succeeds on a normal matrix", fmt(e));
        check(nearMat(localMatrix(back), localMatrix(t), 1e-3f), "decompose round-trips the matrix", fmt(e));
        check(near3(back.scale, t.scale, 1e-3f), "decompose recovers the scale", fmt(back.scale));
    }
}

void testDecomposeGimbalAndFlat() {
    Transform t;
    t.rotationDeg = {90.0f, 40.0f, 10.0f};   // gimbal lock: roll and yaw are one axis
    Transform back;
    decompose(localMatrix(t), &back);
    check(nearMat(localMatrix(back), localMatrix(t), 1e-3f), "gimbal-locked orientation still round-trips");

    Transform flat;
    flat.scale = {1.0f, 0.0f, 1.0f};
    flat.rotationDeg = {0.0f, 30.0f, 0.0f};
    Transform fb;
    check(!decompose(localMatrix(flat), &fb), "decompose reports a flattened matrix");
    check(near(fb.scale.y, 0.0f), "flattened axis has zero scale");
    check(near(fb.rotationDeg.y, 30.0f, 1e-3f), "the surviving yaw is still recovered", std::to_string(fb.rotationDeg.y));
}

void testHierarchy() {
    // root at (10,0,0) yaw 90, child at local (0,0,2). With yaw 90 the child's +Z points to +X in world space.
    std::vector<HierarchyNode> nodes(2);
    nodes[0].parent = -1;
    nodes[0].local.position = {10, 0, 0};
    nodes[0].local.rotationDeg = {0, 90, 0};
    nodes[1].parent = 0;
    nodes[1].local.position = {0, 0, 2};
    const std::vector<glm::mat4> world = resolveHierarchy(nodes);
    check(world.size() == 2, "world matrices are produced for every node");
    const glm::vec3 p = glm::vec3(world[1] * glm::vec4(0, 0, 0, 1));
    check(near3(p, glm::vec3(12, 0, 0), 1e-4f), "child follows the parent's rotation", fmt(p));

    const std::vector<int> depth = hierarchyDepth(nodes);
    check(depth.size() == 2 && depth[0] == 0 && depth[1] == 1, "depth counts the chain");
}

void testCycles() {
    std::vector<HierarchyNode> nodes(3);
    nodes[0].parent = 2;
    nodes[1].parent = 0;
    nodes[2].parent = 1;
    int at = -1;
    check(hasCycle(nodes, &at), "a three-node loop is a cycle");
    check(at >= 0 && at < 3, "the cycle reports one of its nodes");
    check(resolveHierarchy(nodes).empty(), "world resolution refuses a cycle");

    nodes[1].parent = -1;
    check(!hasCycle(nodes, nullptr), "breaking the loop clears the cycle");

    // Dangling parent indices are repaired as roots, not treated as a cycle: that is what a hand-edited scene looks like.
    std::vector<HierarchyNode> dangling(1);
    dangling[0].parent = 7;
    check(!hasCycle(dangling, nullptr), "a missing parent is not a cycle");
    check(resolveHierarchy(dangling).size() == 1, "a missing parent resolves as a root");

    // Self-parenting is a cycle of length one.
    std::vector<HierarchyNode> self(1);
    self[0].parent = 0;
    check(hasCycle(self, nullptr), "self-parenting is a cycle");
}

void testWouldCycle() {
    std::vector<HierarchyNode> nodes(3);
    nodes[0].parent = -1;
    nodes[1].parent = 0;
    nodes[2].parent = 1;
    check(wouldCycle(nodes, 0, 1), "parent cannot be its own descendant");
    check(wouldCycle(nodes, 1, 1), "a node cannot parent itself");
    check(!wouldCycle(nodes, 2, 0), "moving a leaf elsewhere is allowed");
    check(!wouldCycle(nodes, 1, -1), "moving to the root is allowed");
    check(wouldCycle(nodes, 5, 0), "an out-of-range child is rejected");
    check(wouldCycle(nodes, 0, 9), "an out-of-range parent is rejected");
}

void testDeepChain() {
    // 256 nodes in a line: the resolver must not recurse (stack depth) and must produce correct depths.
    std::vector<HierarchyNode> nodes(256);
    for (int i = 0; i < 256; ++i) {
        nodes[static_cast<size_t>(i)].parent = i - 1;
        nodes[static_cast<size_t>(i)].local.position = {0, 1, 0};
    }
    const std::vector<glm::mat4> world = resolveHierarchy(nodes);
    check(world.size() == 256, "a deep chain resolves");
    const glm::vec3 p = glm::vec3(world[255] * glm::vec4(0, 0, 0, 1));
    check(near3(p, glm::vec3(0, 256, 0)), "a deep chain sums the translations", fmt(p));
    const std::vector<int> depth = hierarchyDepth(nodes);
    check(depth[255] == 255, "depth of the last node is 255");
}

void testQuaternionRoundTrip() {
    // The editor writes Euler degrees, Jolt holds a quaternion, and the entity reads degrees again: a tilt the gizmo
    // made about X or Z has to survive the way there and back without being quietly flattened to a yaw.
    const glm::vec3 cases[] = {{0, 0, 0}, {90, 0, 0}, {0, 0, 90}, {30, 45, -60}, {-15, 170, 25}};
    for (const glm::vec3& e : cases) {
        const glm::quat q = eulerToQuat(e);
        const glm::vec3 back = quatToEuler(q);
        check(near3(back, e, 1e-3f), "a quaternion round trip keeps the Euler angles", fmt(back) + " for " + fmt(e));
        const glm::vec3 byMatrix = glm::mat3(rotationMatrix(e)) * glm::vec3(1, 0, 0);
        const glm::vec3 byQuat = glm::mat3_cast(q) * glm::vec3(1, 0, 0);
        check(near3(byQuat, byMatrix, 1e-4f), "eulerToQuat is the rotation rotationMatrix builds", fmt(byQuat));
    }
    // A yaw alone matches the placement matrices every scene written before this change was built from.
    const glm::vec3 spun = glm::mat3_cast(eulerToQuat(glm::vec3(0, 37, 0))) * glm::vec3(0, 0, 1);
    const glm::vec3 legacy = glm::vec3(placementOf(glm::vec3(0.0f), 37.0f) * glm::vec4(0, 0, 1, 1));
    check(near3(spun, legacy, 1e-5f), "a yaw-only quaternion is the legacy placement", fmt(spun));
    // A quaternion is a unit quaternion whatever the angles.
    check(near(glm::length(eulerToQuat(glm::vec3(12, -130, 400))), 1.0f, 1e-5f),
          "eulerToQuat returns a unit quaternion", fmt(glm::vec3(glm::length(eulerToQuat(glm::vec3(12, -130, 400))))));
}

} // namespace

int main() {
    testIdentityAndTranslation();
    testYawMatchesLegacy();
    testRotationOrderIsZxy();
    testAxisDirections();
    testScale();
    testDecomposeRoundTrip();
    testDecomposeGimbalAndFlat();
    testHierarchy();
    testCycles();
    testWouldCycle();
    testDeepChain();
    testQuaternionRoundTrip();
    std::printf("transform_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
