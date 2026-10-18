// Navigation on Recast (navmesh baking) and Detour (path queries, DetourCrowd agents).
#pragma once

#include <atomic>

#include <memory>
#include <vector>

#include <glm/glm.hpp>

class dtNavMesh;
class dtNavMeshQuery;
class dtCrowd;

namespace ukiyo {

// Baking parameters, in metres and degrees. Defaults suit a human-sized agent.
struct NavSettings {
    float cellSize = 0.3f;
    float cellHeight = 0.2f;
    float agentRadius = 0.3f;
    float agentHeight = 1.8f;
    float maxClimb = 0.4f;
    float maxSlopeDeg = 45.0f;
};

class NavMesh {
public:
    using Settings = NavSettings;
    // Bake stages reported through the progress counter: it goes 0, 1, ... kBakeStages when the bake is done.
    static constexpr int kBakeStages = 8;

    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Bakes walkable surfaces from triangles (world space). Replaces any previous mesh and its agents.
    bool build(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices,
               const Settings& settings = NavSettings(),
               std::atomic<int>* progress = nullptr);
    bool built() const { return nav_ != nullptr; }
    int polyCount() const;

    // Straight path from one point to another. Returns false when either end is off the mesh or no path exists.
    bool findPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>* path) const;

    // Agents steer along the navmesh and avoid each other. Ids are stable until removed.
    int addAgent(const glm::vec3& position, float maxSpeed = 3.5f);
    bool moveAgent(int id, const glm::vec3& target);
    void removeAgent(int id);
    void update(float dt);
    glm::vec3 agentPosition(int id) const;
    glm::vec3 agentVelocity(int id) const;
    bool agentArrived(int id) const;

private:
    Settings settings_;
    std::unique_ptr<dtNavMesh, void (*)(dtNavMesh*)> nav_{nullptr, nullptr};
    std::unique_ptr<dtNavMeshQuery, void (*)(dtNavMeshQuery*)> query_{nullptr, nullptr};
    std::unique_ptr<dtCrowd, void (*)(dtCrowd*)> crowd_{nullptr, nullptr};
    std::vector<int> agents_;   // crowd slot per id, -1 when removed
    int polys_ = 0;
};

} // namespace ukiyo
