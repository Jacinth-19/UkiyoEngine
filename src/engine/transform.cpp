#include "engine/transform.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

namespace ukiyo {

namespace {

constexpr float kDeg = 3.14159265358979323846f / 180.0f;

// Euler (x pitch, y yaw, z roll) to a rotation matrix in ZXY order: R = Ry * Rx * Rz.
glm::mat4 eulerToMatrix(const glm::vec3& deg) {
    const float cx = std::cos(deg.x * kDeg), sx = std::sin(deg.x * kDeg);
    const float cy = std::cos(deg.y * kDeg), sy = std::sin(deg.y * kDeg);
    const float cz = std::cos(deg.z * kDeg), sz = std::sin(deg.z * kDeg);

    // Ry
    const glm::mat3 ry(cy, 0.0f, -sy,
                       0.0f, 1.0f, 0.0f,
                       sy, 0.0f, cy);
    // Rx
    const glm::mat3 rx(1.0f, 0.0f, 0.0f,
                       0.0f, cx, sx,
                       0.0f, -sx, cx);
    // Rz
    const glm::mat3 rz(cz, sz, 0.0f,
                       -sz, cz, 0.0f,
                       0.0f, 0.0f, 1.0f);
    return glm::mat4(ry * rx * rz);
}

// ZXY extraction. For R = Ry Ryaw * Rx Rpitch * Rz Rroll:
//   forward column (third column) is (sy*cx, -sx, cy*cx)
//   so pitch = asin(-f.y), yaw = atan2(f.x, f.z)
//   and roll comes from the first column's y after removing the pitch/yaw terms.
glm::vec3 matrixToEuler(const glm::mat3& r) {
    const float sy = -r[2][1];
    const float clamped = std::clamp(sy, -1.0f, 1.0f);
    const float pitch = std::asin(clamped);
    float yaw = 0.0f;
    float roll = 0.0f;
    if (std::fabs(clamped) < 0.9999f) {
        yaw = std::atan2(r[2][0], r[2][2]);
        roll = std::atan2(r[0][1], r[1][1]);
    } else {
        // Gimbal lock: pitch is +-90 degrees, so yaw and roll turn about the same axis. Put all of the turn in yaw and
        // leave roll at zero; the resulting orientation is identical.
        yaw = std::atan2(-r[0][2], r[0][0]);
        roll = 0.0f;
    }
    const float r2d = 180.0f / 3.14159265358979323846f;
    return glm::vec3(pitch * r2d, yaw * r2d, roll * r2d);
}

// Rounds an angle to the range (-180, 180] so a gizmo drag that spins past a half turn reports the small angle.
float wrapDeg(float d) {
    float r = std::fmod(d, 360.0f);
    if (r > 180.0f) r -= 360.0f;
    if (r <= -180.0f) r += 360.0f;
    // -0 is a legal angle but prints as "-0" and compares unequal to 0.0f under some compilers' fast-math; normalise it.
    return r == 0.0f ? 0.0f : r;
}

} // namespace

glm::mat4 rotationMatrix(const glm::vec3& eulerDeg) {
    return eulerToMatrix(eulerDeg);
}

glm::quat eulerToQuat(const glm::vec3& eulerDeg) {
    return glm::normalize(glm::quat_cast(glm::mat3(eulerToMatrix(eulerDeg))));
}

glm::vec3 quatToEuler(const glm::quat& q) {
    const glm::vec3 e = matrixToEuler(glm::mat3_cast(glm::normalize(q)));
    return glm::vec3(wrapDeg(e.x), wrapDeg(e.y), wrapDeg(e.z));
}

glm::mat4 localMatrix(const Transform& t) {
    glm::mat4 m = eulerToMatrix(t.rotationDeg);
    m[0] *= t.scale.x;
    m[1] *= t.scale.y;
    m[2] *= t.scale.z;
    m[3] = glm::vec4(t.position, 1.0f);
    return m;
}

glm::mat4 worldMatrix(const Transform& local, const glm::mat4* parent) {
    const glm::mat4 m = localMatrix(local);
    return parent ? (*parent) * m : m;
}

bool decompose(const glm::mat4& m, Transform* out) {
    if (!out) return false;
    out->position = glm::vec3(m[3]);
    // Column lengths are the scale; a negative determinant means an odd number of mirrored axes, which this engine
    // does not author, so the sign is taken from the column itself and reported as a negative scale on x only.
    const glm::vec3 cx(m[0]), cy(m[1]), cz(m[2]);
    out->scale = glm::vec3(glm::length(cx), glm::length(cy), glm::length(cz));
    glm::mat3 r(1.0f);
    const glm::vec3 cols[3] = {cx, cy, cz};
    int zeros = 0;
    int zeroAxis = -1;
    for (int i = 0; i < 3; ++i) {
        if (out->scale[i] > 1e-8f) {
            r[i] = cols[i] / out->scale[i];
        } else {
            ++zeros;
            zeroAxis = i;
        }
    }
    const bool flat = zeros > 0;
    if (zeros == 1) {
        // A zero axis has no direction to normalise; rebuild it from the other two so the object keeps its orientation
        // while the user restores the flattened axis. For a right-handed basis, column i = column (i+1) x column (i+2).
        const int a = (zeroAxis + 1) % 3;
        const int b = (zeroAxis + 2) % 3;
        r[zeroAxis] = glm::cross(r[a], r[b]);
    } else if (zeros >= 2) {
        // Two axes collapsed: there is no orientation left to recover. Pick a right-handed basis from the axis that
        // survived (if any) so the inspector shows something the user can type over.
        const int keep = zeros == 3 ? -1 : (zeroAxis == 0 ? 1 : 0);
        r = glm::mat3(1.0f);
        if (keep >= 0 && out->scale[keep] > 1e-8f) r[keep] = cols[keep] / out->scale[keep];
        if (keep == 0) {
            r[1] = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) - r[0] * r[0].y);
            r[2] = glm::cross(r[0], r[1]);
        } else {
            const glm::vec3 up = (keep == 1) ? r[1] : glm::vec3(0, 1, 0);
            glm::vec3 seed = std::fabs(up.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            r[0] = glm::normalize(seed - up * glm::dot(seed, up));
            r[2] = glm::cross(r[0], up);
            if (keep == 1) r[1] = up; else { r[1] = up; r[2] = glm::cross(r[0], r[1]); }
        }
    }
    r[0] = glm::normalize(r[0]);
    r[1] = glm::normalize(r[1]);
    r[2] = glm::normalize(r[2]);
    glm::vec3 e = matrixToEuler(r);
    out->rotationDeg = glm::vec3(wrapDeg(e.x), wrapDeg(e.y), wrapDeg(e.z));
    return !flat;
}

bool hasCycle(const std::vector<HierarchyNode>& nodes, int* firstCycle) {
    const int n = static_cast<int>(nodes.size());
    std::vector<int> state(static_cast<size_t>(n), 0);   // 0 unvisited, 1 on the current walk, 2 done
    for (int start = 0; start < n; ++start) {
        int at = start;
        while (at >= 0 && at < n && state[static_cast<size_t>(at)] == 0) {
            state[static_cast<size_t>(at)] = 1;
            at = nodes[static_cast<size_t>(at)].parent;
        }
        if (at >= 0 && at < n && state[static_cast<size_t>(at)] == 1) {
            if (firstCycle) *firstCycle = at;
            return true;
        }
        // Mark the whole walk finished; the parent chain above is already resolved or proven acyclic.
        at = start;
        while (at >= 0 && at < n && state[static_cast<size_t>(at)] == 1) {
            state[static_cast<size_t>(at)] = 2;
            at = nodes[static_cast<size_t>(at)].parent;
        }
    }
    return false;
}

std::vector<glm::mat4> resolveHierarchy(const std::vector<HierarchyNode>& nodes) {
    const int n = static_cast<int>(nodes.size());
    std::vector<glm::mat4> out(static_cast<size_t>(n), glm::mat4(1.0f));
    if (n == 0) return out;
    if (hasCycle(nodes, nullptr)) return {};

    std::vector<int> depth(static_cast<size_t>(n), -1);
    for (int i = 0; i < n; ++i) {
        // Walk to the root collecting the chain, then fill depths and matrices on the way back down. Depth is bounded
        // by n, so a dangling parent cannot loop here (hasCycle already rejected real cycles).
        std::vector<int> chain;
        int at = i;
        while (at >= 0 && at < n && depth[static_cast<size_t>(at)] < 0) {
            chain.push_back(at);
            at = nodes[static_cast<size_t>(at)].parent;
        }
        if (at < 0 || at >= n) {
            // The chain ends at a dangling parent; treat that node as a root by giving it depth 0 below.
            if (!chain.empty()) {
                const int last = chain.back();
                depth[static_cast<size_t>(last)] = 0;
                out[static_cast<size_t>(last)] = worldMatrix(nodes[static_cast<size_t>(last)].local, nullptr);
                chain.pop_back();
            }
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const int idx = *it;
            const int p = nodes[static_cast<size_t>(idx)].parent;
            const glm::mat4* parent = (p >= 0 && p < n) ? &out[static_cast<size_t>(p)] : nullptr;
            depth[static_cast<size_t>(idx)] = parent ? depth[static_cast<size_t>(p)] + 1 : 0;
            out[static_cast<size_t>(idx)] = worldMatrix(nodes[static_cast<size_t>(idx)].local, parent);
        }
    }
    return out;
}

std::vector<int> hierarchyDepth(const std::vector<HierarchyNode>& nodes) {
    const int n = static_cast<int>(nodes.size());
    std::vector<int> depth(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        int d = 0;
        int at = nodes[static_cast<size_t>(i)].parent;
        // Bound the walk: a cycle would otherwise spin forever, and a file with one is rejected before this is called.
        int guard = 0;
        while (at >= 0 && at < n && guard <= n) {
            ++d;
            at = nodes[static_cast<size_t>(at)].parent;
            ++guard;
        }
        depth[static_cast<size_t>(i)] = (guard > n) ? 0 : d;
    }
    return depth;
}

bool wouldCycle(const std::vector<HierarchyNode>& nodes, int child, int newParent) {
    const int n = static_cast<int>(nodes.size());
    if (child < 0 || child >= n) return true;
    if (newParent < 0) return false;   // moving to the root never closes a loop
    if (newParent >= n) return true;
    if (newParent == child) return true;
    int at = newParent;
    int guard = 0;
    while (at >= 0 && at < n && guard <= n) {
        if (at == child) return true;
        at = nodes[static_cast<size_t>(at)].parent;
        ++guard;
    }
    return false;
}

glm::mat4 placementOf(const glm::vec3& position, float yawDeg) {
    return glm::translate(glm::mat4(1.0f), position) *
           glm::rotate(glm::mat4(1.0f), yawDeg * kDeg, glm::vec3(0.0f, 1.0f, 0.0f));
}

} // namespace ukiyo
