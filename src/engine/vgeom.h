// Virtual geometry (item 21): cluster LODs for static meshes, in the spirit of Nanite and of the reference project
// Jacinth-19/nanite-at-home. The offline half splits a mesh into 128-triangle clusters, welds groups of four into a
// simplified parent, and repeats until one root is left, so the mesh becomes a tree of clusters with a geometric error
// on each. The runtime half walks that tree per view and returns a cut: the coarse clusters whose projected error is
// under a pixel budget, and/or their children where the surface needs detail.
//
// This header is plain CPU data: no device, no RHI, so the builder and the selector are testable without a GPU. The
// renderer turns the cut into one index buffer and draws it (see Renderer::drawWith); no compute or indirect draw is
// involved, which is the honest difference from real Nanite and is written down in docs/VGEOM.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

constexpr uint32_t kVGeomNoCluster = 0xFFFFFFFFu;

struct VGeomSettings {
    // Triangles in a leaf cluster. Real Nanite uses 128; it keeps the tree shallow and the clusters small enough to
    // cull and to fit a page.
    uint32_t clusterTriangles = 128;
    // Children merged into one parent. Four gives the classic hierarchy (128 -> 32 -> 8 ... triangles); the reference
    // uses METIS with a group count, which is the same idea without the balanced-cut machinery.
    uint32_t groupSize = 4;
    // Target triangle ratio of a parent against the triangles of all its children together. Four children at 0.25
    // keep the triangle count per level halving (a parent covers the same surface as one child), which is what keeps
    // the tree from growing denser as it climbs; 0.5 barely simplifies a patch whose border is already locked.
    float targetRatio = 0.25f;
    // Never simplify a parent below this many triangles: a 4-triangle cluster has no silhouette left.
    uint32_t minTriangles = 24;
    // Hard stop for a pathological mesh; a 1M-triangle mesh needs nine levels at ratio 0.5.
    uint32_t maxLevels = 12;
};

// One cluster: a contiguous run in VGeomMesh::indices (which indexes the original vertex buffer), plus the bounds and
// error that drive selection.
struct VGeomCluster {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t level = 0;
    uint32_t parent = kVGeomNoCluster;
    uint32_t childFirst = 0;    // into VGeomMesh::children
    uint32_t childCount = 0;
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
    float error = 0.0f;         // geometric error of this LOD against the original surface, model units

    uint32_t triangleCount() const { return indexCount / 3; }
};

struct VGeomMesh {
    std::vector<VGeomCluster> clusters;
    std::vector<uint32_t> children;   // contiguous per parent, in childFirst/childCount order
    std::vector<uint32_t> indices;    // every LOD of every cluster, concatenated; indexes the original vertices
    uint32_t root = kVGeomNoCluster;
    uint32_t levels = 0;
    uint32_t sourceTriangles = 0;
    uint32_t vertexCount = 0;
    uint64_t sourceHash = 0;          // of the source positions and indices, so a cache can be validated

    bool valid() const { return root != kVGeomNoCluster && !indices.empty(); }
    size_t triangleCount() const { return indices.size() / 3; }
    size_t leafCount() const;
};

// Builds the tree. `positions` is the first member of each vertex (rhi::Vertex starts with pos, so the engine can
// pass its vertex array and sizeof(rhi::Vertex) as the stride); `indices` is the source index buffer.
VGeomMesh buildVGeom(const float* positions, size_t vertexCount, size_t stride, const uint32_t* indices,
                     size_t indexCount, const VGeomSettings& settings = {});

struct VGeomView {
    glm::mat4 viewProj{1.0f};
    glm::vec3 eye{0.0f};
    // Screen height / (2 tan(fovY / 2)): world metres at one metre of depth cover this many pixels. A 1080p view with
    // a 50 degree vertical FOV is about 1159.
    float pixelScale = 1000.0f;
    // False selects the whole tree regardless of the frustum. The renderer keeps it on; a tool that wants one cut for
    // an off-screen pass, or a test that wants to measure the tree itself, turns it off.
    bool cull = true;
};

struct VGeomStats {
    uint32_t clusters = 0;
    uint32_t leaves = 0;
    uint32_t triangles = 0;
    uint32_t culled = 0;
};

// Walks the tree from the root and returns the clusters to draw, in model space. A 0 or negative `errorPixels`
// forces the leaves (a shadow or a hero shot). Clusters outside the frustum are skipped; their children are not
// visited, so a fully culled mesh costs one sphere test.
std::vector<uint32_t> selectVGeom(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view,
                                  float errorPixels, VGeomStats* stats = nullptr);

// The same walk, writing into `out` instead of returning a fresh vector. The renderer calls this once per draw and
// keeps the vector across frames, so a scene of static props does not allocate per pass.
void selectVGeom(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view, float errorPixels,
                 std::vector<uint32_t>& out, VGeomStats* stats = nullptr);

// Cache files: a small versioned binary so a scene does not rebuild its simplification every load. Pass the mesh's
// hash (vgeomHash of the same data that built it) as `expectHash` and a stale file is refused instead of used.
bool saveVGeom(const VGeomMesh& mesh, const std::string& path);
bool loadVGeom(VGeomMesh& mesh, const std::string& path, uint64_t expectHash = 0);

// A model is a list of primitives, so its cache is a set: one header with the build settings, then one blob per
// primitive. `sourceHashes` is one entry per primitive, in order; a primitive whose tree is missing or stale comes
// back empty and is rebuilt by the caller.
bool saveVGeomSet(const std::vector<VGeomMesh>& meshes, const std::string& path,
                  const VGeomSettings& settings = {});
bool loadVGeomSet(std::vector<VGeomMesh>& meshes, const std::string& path, const std::vector<uint64_t>& sourceHashes,
                  const VGeomSettings& settings = {});
uint32_t vgeomSettingsHash(const VGeomSettings& settings);
uint64_t vgeomHash(const float* positions, size_t vertexCount, size_t stride, const uint32_t* indices,
                   size_t indexCount);

} // namespace ukiyo
