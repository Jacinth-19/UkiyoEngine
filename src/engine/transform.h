// Scene transforms: position, rotation and scale, with parent composition.
//
// An instance used to carry a position and a yaw angle. That is enough for a character standing on flat ground and
// nothing else: no tilted props, no scaled set dressing, no objects attached to other objects. This is the replacement:
// a TRS transform with a full rotation and a per-axis scale, plus the parenting rules that make a hierarchy usable
// (world-matrix resolution in parent order, cycle rejection, and a decomposition that the editor's gizmo can invert).
//
// The rotation order matches Unity's ZXY intrinsic order, which is what the editor's inspector presents: yaw (Y) is
// applied to the model first, then pitch (X), then roll (Z). Written as a matrix product that is R = Ry * Rx * Rz, so a
// yaw turns the whole object and a roll tilts it around its own forward axis.
#pragma once

#include <cstddef>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ukiyo {

// A local transform. Degrees at the boundary because the scene file, the inspector and the Python tools all speak
// degrees; radians exist only inside the math below.
struct Transform {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 rotationDeg{0.0f, 0.0f, 0.0f};   // x = pitch, y = yaw, z = roll
    glm::vec3 scale{1.0f, 1.0f, 1.0f};

    bool operator==(const Transform& o) const {
        return position == o.position && rotationDeg == o.rotationDeg && scale == o.scale;
    }
    bool operator!=(const Transform& o) const { return !(*this == o); }
};

// Rotation matrix for ZXY Euler angles in degrees.
glm::mat4 rotationMatrix(const glm::vec3& eulerDeg);

// The local matrix: T * R * S.
glm::mat4 localMatrix(const Transform& t);

// The world matrix of a local transform under an optional parent matrix.
glm::mat4 worldMatrix(const Transform& local, const glm::mat4* parent);

// Inverse of localMatrix: pulls position, ZXY Euler angles and scale back out. Returns false when the matrix has a
// zero axis (a flattened object), in which case the scale on that axis is reported as 0 and the rotation is still
// filled in from the other two axes. The editor's gizmo uses this to write a dragged matrix back into fields.
bool decompose(const glm::mat4& m, Transform* out);

// One node of a hierarchy: its parent index (-1 for a root) and its local transform.
struct HierarchyNode {
    int parent = -1;
    Transform local;
};

// True when following parent links from some node never reaches a root, i.e. the forest contains a cycle. A node with
// a parent index outside [0, size) is treated as a root rather than a cycle, because that is how a file with a dangling
// or self-referencing parent is repaired on load: the link is dropped. `firstCycle` receives one node on the cycle.
bool hasCycle(const std::vector<HierarchyNode>& nodes, int* firstCycle = nullptr);

// World matrices in parent-before-child order. A cycle (which hasCycle rejects) makes this return an empty vector; a
// dangling parent is treated as a root. The result is always the same size as the input.
std::vector<glm::mat4> resolveHierarchy(const std::vector<HierarchyNode>& nodes);

// Depth of each node in the forest (0 for roots), used to order the hierarchy panel and to validate drops.
std::vector<int> hierarchyDepth(const std::vector<HierarchyNode>& nodes);

// True when making `child` a child of `newParent` would close a loop (including child == newParent, and when
// newParent is already a descendant of child). The editor calls this before accepting a drag.
bool wouldCycle(const std::vector<HierarchyNode>& nodes, int child, int newParent);

// A placement matrix for the common character case: position and yaw only, with unit scale. Kept because half the
// editor and every scene file written before this change use it.
glm::mat4 placementOf(const glm::vec3& position, float yawDeg);

} // namespace ukiyo
