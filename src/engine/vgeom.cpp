#include "engine/vgeom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

#include "meshoptimizer.h"

namespace ukiyo {
namespace {

constexpr uint64_t kMagic = 0x3156474B55ull;   // "UKVG1" little-endian
constexpr uint32_t kVersion = 1;

const float* vertexAt(const float* positions, size_t vertex, size_t stride) {
    return reinterpret_cast<const float*>(reinterpret_cast<const char*>(positions) + vertex * stride);
}

glm::vec3 positionOf(const float* positions, size_t vertex, size_t stride) {
    const float* p = vertexAt(positions, vertex, stride);
    return glm::vec3(p[0], p[1], p[2]);
}

// One millimetre of weld tolerance. Vertices closer than that are treated as one point for adjacency and for the
// hierarchy's grouping, which is what lets a cluster boundary match its neighbour's: both sides resolve to the same
// original index wherever the loader produced one, and simplification never moves a boundary vertex.
uint64_t weldKey(const glm::vec3& p) {
    auto q = [](float v) {
        const int64_t i = int64_t(std::llround(double(v) * 1000.0));
        return uint64_t(uint32_t(int32_t(std::clamp<int64_t>(i, -(1 << 20), (1 << 20) - 1)) + (1 << 20))) & 0x1FFFFFull;
    };
    return q(p.x) | (q(p.y) << 21) | (q(p.z) << 42);
}

uint64_t spreadBits(uint64_t v) {
    v &= 0x1FFFFFull;
    v = (v | (v << 32)) & 0x1F00000000FFFFull;
    v = (v | (v << 16)) & 0x1F0000FF0000FFull;
    v = (v | (v << 8)) & 0x100F00F00F00F00Full;
    v = (v | (v << 4)) & 0x10C30C30C30C30C3ull;
    v = (v | (v << 2)) & 0x1249249249249249ull;
    return v;
}

uint64_t mortonKey(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& invExtent) {
    const glm::vec3 n = glm::clamp((p - lo) * invExtent, glm::vec3(0.0f), glm::vec3(1.0f));
    auto q = [](float v) { return uint64_t(std::min(0x1FFFFF, int(std::lround(double(v) * 2097151.0)))); };
    return spreadBits(q(n.x)) | (spreadBits(q(n.y)) << 1) | (spreadBits(q(n.z)) << 2);
}

struct TempCluster {
    std::vector<uint32_t> indices;
    uint32_t parent = kVGeomNoCluster;
    glm::vec3 lo{0.0f}, hi{0.0f};
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
    float error = 0.0f;
    uint32_t level = 0;
    uint32_t first = 0;
};

void boundsOf(const std::vector<uint32_t>& indices, const float* positions, size_t stride, glm::vec3* lo,
              glm::vec3* hi, glm::vec3* centre, float* radius) {
    glm::vec3 a(std::numeric_limits<float>::max()), b(-std::numeric_limits<float>::max());
    for (uint32_t i : indices) {
        const glm::vec3 p = positionOf(positions, i, stride);
        a = glm::min(a, p);
        b = glm::max(b, p);
    }
    if (indices.empty()) a = b = glm::vec3(0.0f);
    *lo = a;
    *hi = b;
    *centre = (a + b) * 0.5f;
    float r = 0.0f;
    for (uint32_t i : indices) r = std::max(r, glm::length(positionOf(positions, i, stride) - *centre));
    *radius = r;
}

// Fills `out` with a cluster of about `targetTriangles` triangles, grown breadth-first over shared edges from the
// lowest unassigned triangle. Deterministic: same mesh, same order.
void growClusters(const std::vector<std::array<uint32_t, 3>>& adjacency, uint32_t targetTriangles,
                  std::vector<uint32_t>* triCluster, std::vector<std::vector<uint32_t>>* clusters) {
    const uint32_t triCount = uint32_t(adjacency.size());
    triCluster->assign(triCount, kVGeomNoCluster);
    clusters->clear();
    std::vector<uint32_t> queue;
    queue.reserve(targetTriangles * 2);
    for (uint32_t seed = 0; seed < triCount; ++seed) {
        if ((*triCluster)[seed] != kVGeomNoCluster) continue;
        const uint32_t id = uint32_t(clusters->size());
        clusters->emplace_back();
        std::vector<uint32_t>& list = clusters->back();
        list.reserve(targetTriangles);
        queue.clear();
        queue.push_back(seed);
        for (size_t head = 0; head < queue.size() && list.size() < targetTriangles; ++head) {
            const uint32_t t = queue[head];
            if ((*triCluster)[t] != kVGeomNoCluster) continue;
            (*triCluster)[t] = id;
            list.push_back(t);
            for (uint32_t n : adjacency[t])
                if (n != kVGeomNoCluster && (*triCluster)[n] == kVGeomNoCluster) queue.push_back(n);
        }
    }
}

// A breadth-first growth fills its quota and leaves pockets: triangles whose neighbours are already claimed. Left
// alone those become one-triangle clusters, worthless for culling and fatal for parent simplification (every vertex
// of the patch becomes a locked border). Fold them into a neighbouring cluster; a cluster a few triangles over budget
// is harmless next to a cluster of one.
void mergePockets(const std::vector<std::array<uint32_t, 3>>& adjacency, std::vector<uint32_t>* triCluster,
                  std::vector<std::vector<uint32_t>>* clusters, uint32_t minLeaf) {
    std::vector<std::vector<uint32_t>> byCluster(clusters->size());
    for (uint32_t t = 0; t < triCluster->size(); ++t) byCluster[(*triCluster)[t]].push_back(t);
    for (size_t pass = 0; pass < 8; ++pass) {
        bool moved = false;
        for (uint32_t c = 0; c < byCluster.size(); ++c) {
            std::vector<uint32_t>& list = byCluster[c];
            if (list.empty() || list.size() >= minLeaf) continue;
            uint32_t into = kVGeomNoCluster;
            for (uint32_t t : list) {
                for (uint32_t n : adjacency[t]) {
                    if (n == kVGeomNoCluster) continue;
                    const uint32_t d = (*triCluster)[n];
                    if (d != c && (into == kVGeomNoCluster || byCluster[d].size() > byCluster[into].size())) into = d;
                }
            }
            if (into == kVGeomNoCluster) continue;
            for (uint32_t t : list) {
                (*triCluster)[t] = into;
                byCluster[into].push_back(t);
            }
            list.clear();
            moved = true;
        }
        if (!moved) break;
    }
    clusters->clear();
    for (const std::vector<uint32_t>& list : byCluster)
        if (!list.empty()) clusters->push_back(list);
}

} // namespace

size_t VGeomMesh::leafCount() const {
    size_t n = 0;
    for (const VGeomCluster& c : clusters)
        if (c.childCount == 0) ++n;
    return n;
}

uint64_t vgeomHash(const float* positions, size_t vertexCount, size_t stride, const uint32_t* indices,
                   size_t indexCount) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    for (size_t v = 0; v < vertexCount; ++v) mix(vertexAt(positions, v, stride), sizeof(float) * 3);
    if (indexCount) mix(indices, sizeof(uint32_t) * indexCount);
    return h;
}

VGeomMesh buildVGeom(const float* positions, size_t vertexCount, size_t stride, const uint32_t* indices,
                     size_t indexCount, const VGeomSettings& settings) {
    VGeomMesh mesh;
    if (!positions || !indices || vertexCount == 0 || indexCount < 3) return mesh;
    const uint32_t triCount = uint32_t(indexCount / 3);
    mesh.sourceTriangles = triCount;
    mesh.vertexCount = uint32_t(vertexCount);
    mesh.sourceHash = vgeomHash(positions, vertexCount, stride, indices, indexCount);
    const uint32_t clusterTriangles = std::max(8u, settings.clusterTriangles);
    const uint32_t groupSize = std::max(2u, settings.groupSize);

    // ------------------------------------------------------------------ adjacency, by welded position
    std::unordered_map<uint64_t, uint32_t> welds;
    welds.reserve(vertexCount * 2);
    std::vector<uint32_t> welded(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v) {
        const uint64_t key = weldKey(positionOf(positions, v, stride));
        auto it = welds.find(key);
        if (it == welds.end()) {
            welded[v] = uint32_t(v);
            welds.emplace(key, uint32_t(v));
        } else {
            welded[v] = it->second;
        }
    }

    std::vector<std::array<uint32_t, 3>> adjacency(triCount, {kVGeomNoCluster, kVGeomNoCluster, kVGeomNoCluster});
    {
        struct Edge {
            uint32_t tri = kVGeomNoCluster;
            uint8_t slot = 0;
        };
        std::unordered_map<uint64_t, Edge> edges;
        edges.reserve(triCount * 3);
        for (uint32_t t = 0; t < triCount; ++t) {
            uint32_t v[3] = {welded[indices[t * 3]], welded[indices[t * 3 + 1]], welded[indices[t * 3 + 2]]};
            for (int e = 0; e < 3; ++e) {
                uint32_t a = v[e], b = v[(e + 1) % 3];
                if (a == b) continue;   // a degenerate edge cannot be shared
                if (a > b) std::swap(a, b);
                const uint64_t key = (uint64_t(a) << 32) | b;
                auto it = edges.find(key);
                if (it == edges.end()) {
                    edges.emplace(key, Edge{t, uint8_t(e)});
                } else if (it->second.tri != kVGeomNoCluster) {
                    adjacency[t][e] = it->second.tri;
                    adjacency[it->second.tri][it->second.slot] = t;
                    it->second.tri = kVGeomNoCluster;   // a third face on one edge is non-manifold; ignore it
                }
            }
        }
    }

    // ------------------------------------------------------------------ leaves
    std::vector<uint32_t> triCluster;
    std::vector<std::vector<uint32_t>> groups;
    growClusters(adjacency, clusterTriangles, &triCluster, &groups);
    mergePockets(adjacency, &triCluster, &groups, std::max(8u, clusterTriangles / 8));

    std::vector<TempCluster> current;
    current.reserve(groups.size());
    mesh.indices.reserve(indexCount + indexCount / 2);
    for (const std::vector<uint32_t>& group : groups) {
        TempCluster c;
        c.indices.reserve(group.size() * 3);
        for (uint32_t t : group) {
            c.indices.push_back(indices[t * 3]);
            c.indices.push_back(indices[t * 3 + 1]);
            c.indices.push_back(indices[t * 3 + 2]);
        }
        c.level = 0;
        boundsOf(c.indices, positions, stride, &c.lo, &c.hi, &c.centre, &c.radius);
        current.push_back(std::move(c));
    }
    if (current.empty()) return mesh;

    std::vector<TempCluster> all = current;   // leaves are clusters 0..n-1 and keep their order

    // ------------------------------------------------------------------ parents
    glm::vec3 modelLo(std::numeric_limits<float>::max()), modelHi(-std::numeric_limits<float>::max());
    for (const TempCluster& c : current) {
        modelLo = glm::min(modelLo, c.lo);
        modelHi = glm::max(modelHi, c.hi);
    }
    const glm::vec3 extent = glm::max(modelHi - modelLo, glm::vec3(1e-4f));
    const glm::vec3 invExtent = glm::vec3(1.0f) / extent;
    // A level may move the surface by a fifth of a per cent of the model size, or twice its children's error,
    // whichever is larger. meshopt stops early when it cannot spend that budget, which leaves a denser parent rather
    // than a wrong one; the error it measures is what selection uses either way.
    const float levelError = std::max(glm::length(extent) * 0.002f, 1e-5f);

    std::vector<uint32_t> order;
    uint32_t level = 0;
    std::vector<uint32_t> ids(current.size());
    for (uint32_t i = 0; i < ids.size(); ++i) ids[i] = i;

    auto makeParent = [&](const std::vector<uint32_t>& members, uint32_t newLevel) {
        // Gather the group's triangles over the original vertex ids, then simplify a compact copy of the vertices.
        std::vector<uint32_t> source;
        size_t total = 0;
        for (uint32_t id : members) total += all[id].indices.size();
        source.reserve(total);
        for (uint32_t id : members) source.insert(source.end(), all[id].indices.begin(), all[id].indices.end());

        std::unordered_map<uint32_t, uint32_t> localOf;
        localOf.reserve(source.size());
        std::vector<uint32_t> localIndices;
        localIndices.reserve(source.size());
        std::vector<float> localPositions;
        for (uint32_t global : source) {
            auto it = localOf.find(global);
            if (it == localOf.end()) {
                it = localOf.emplace(global, uint32_t(localPositions.size() / 3)).first;
                const glm::vec3 p = positionOf(positions, global, stride);
                localPositions.push_back(p.x);
                localPositions.push_back(p.y);
                localPositions.push_back(p.z);
            }
            localIndices.push_back(it->second);
        }

        const uint32_t groupTris = uint32_t(source.size() / 3);
        const uint32_t targetTris = std::max(settings.minTriangles, uint32_t(float(groupTris) * settings.targetRatio));
        float childError = 0.0f;
        for (uint32_t id : members) childError = std::max(childError, all[id].error);
        float error = childError;
        std::vector<uint32_t> simplified;
        bool ok = false;
        if (targetTris < groupTris && !localPositions.empty()) {
            simplified.resize(localIndices.size());
            float resultError = 0.0f;
            const size_t out = meshopt_simplify(
                simplified.data(), localIndices.data(), localIndices.size(), localPositions.data(),
                localPositions.size() / 3, sizeof(float) * 3, size_t(targetTris) * 3,
                std::max(levelError, childError * 2.0f),
                meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &resultError);
            if (out >= 3 && out < localIndices.size()) {
                simplified.resize(out);
                error = std::max(childError, resultError);
                ok = true;
            }
        }
        TempCluster c;
        if (ok) {
            std::vector<uint32_t> globalOf(localPositions.size() / 3);
            for (const auto& kv : localOf) globalOf[kv.second] = kv.first;
            c.indices.resize(simplified.size());
            for (size_t i = 0; i < simplified.size(); ++i) c.indices[i] = globalOf[simplified[i]];
        } else {
            c.indices = source;
        }
        c.error = error;
        c.level = newLevel;
        boundsOf(c.indices, positions, stride, &c.lo, &c.hi, &c.centre, &c.radius);
        all.push_back(std::move(c));
        return uint32_t(all.size() - 1);
    };

    while (ids.size() > 1 && level + 1 < settings.maxLevels) {
        // Morton order keeps a parent's children together on the surface, so its patch is compact and its locked
        // border is short. Consecutive grouping on ids alone would glue opposite sides of the mesh together. Rebuilt
        // from this level's ids every pass: the previous level's children are not the level's members.
        order = ids;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return mortonKey(all[a].centre, modelLo, invExtent) < mortonKey(all[b].centre, modelLo, invExtent);
        });
        // Group sizes, spread so no group is left with a single member: a one-child parent is a duplicate LOD of the
        // same patch, and a chain of them is how a tree stops getting coarser near the root.
        std::vector<uint32_t> sizes;
        for (size_t left = ids.size(); left > 0;) {
            uint32_t take = uint32_t(std::min<size_t>(groupSize, left));
            if (left > take && left - take == 1) --take;
            sizes.push_back(std::max(1u, take));
            left -= sizes.back();
        }
        std::vector<uint32_t> next;
        next.reserve(sizes.size());
        size_t cursor = 0;
        for (uint32_t take : sizes) {
            std::vector<uint32_t> members;
            for (uint32_t j = 0; j < take; ++j) members.push_back(order[cursor + j]);
            cursor += take;
            const uint32_t id = makeParent(members, level + 1);
            for (uint32_t child : members) all[child].parent = id;
            next.push_back(id);
        }
        ids = std::move(next);
        ++level;
    }
    if (ids.size() > 1) {
        const uint32_t rootId = makeParent(ids, level + 1);
        for (uint32_t child : ids) all[child].parent = rootId;
        ids[0] = rootId;
    }
    mesh.root = ids.empty() ? kVGeomNoCluster : ids[0];
    mesh.levels = level + 1;

    // ------------------------------------------------------------------ flatten into the public form
    mesh.clusters.resize(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        VGeomCluster& out = mesh.clusters[i];
        out.firstIndex = uint32_t(mesh.indices.size());
        out.indexCount = uint32_t(all[i].indices.size());
        out.level = all[i].level;
        out.parent = all[i].parent;
        out.centre = all[i].centre;
        out.radius = all[i].radius;
        out.error = all[i].error;
        mesh.indices.insert(mesh.indices.end(), all[i].indices.begin(), all[i].indices.end());
    }
    // Children are stored contiguously per parent: count, prefix-sum, fill.
    std::vector<uint32_t> childCount(all.size(), 0);
    for (const TempCluster& c : all)
        if (c.parent != kVGeomNoCluster) ++childCount[c.parent];
    uint32_t cursor = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        mesh.clusters[i].childFirst = cursor;
        mesh.clusters[i].childCount = childCount[i];
        cursor += childCount[i];
    }
    mesh.children.resize(cursor);
    std::vector<uint32_t> fill = childCount;
    for (size_t j = 0; j < all.size(); ++j) {
        const uint32_t p = all[j].parent;
        if (p == kVGeomNoCluster) continue;
        mesh.children[mesh.clusters[p].childFirst + (childCount[p] - fill[p])] = uint32_t(j);
        --fill[p];
    }
    return mesh;
}

void selectVGeom(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view, float errorPixels,
                 std::vector<uint32_t>& out, VGeomStats* stats) {
    out.clear();
    if (!mesh.valid()) {
        if (stats) *stats = VGeomStats{};
        return;
    }
    // Frustum planes from the view-projection's rows (Gribb-Hartmann). The near plane is left out: its convention
    // differs between a zero-to-one and a minus-one-to-one projection, and a cluster behind the camera only costs a
    // clipped draw. Left, right, bottom, top and far are enough to cull the world.
    const glm::mat4& m = view.viewProj;
    auto row = [&m](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 r3 = row(3);
    glm::vec4 planes[5] = {r3 + row(0), r3 - row(0), r3 + row(1), r3 - row(1), r3 - row(2)};
    // The plane coefficients carry the projection's scale, which differs per plane, so a world-space radius only
    // compares against them once they are normalized. Skipping this over-culls by the plane's normal length (2-3x on
    // the side planes of a typical perspective), and the visible edges of every coarse patch drop out.
    for (glm::vec4& p : planes) p /= std::max(glm::length(glm::vec3(p)), 1e-6f);

    const glm::vec3 eye = view.eye;
    auto maxScale = [](const glm::mat4& mtx) {
        return std::max(glm::length(glm::vec3(mtx[0])), std::max(glm::length(glm::vec3(mtx[1])), glm::length(glm::vec3(mtx[2]))));
    };
    const float scale = maxScale(model);
    const float pixelScale = std::max(1.0f, view.pixelScale);

    VGeomStats counts;
    std::vector<uint32_t> stack;
    stack.push_back(mesh.root);
    while (!stack.empty()) {
        const uint32_t id = stack.back();
        stack.pop_back();
        const VGeomCluster& c = mesh.clusters[id];
        const glm::vec3 centre = glm::vec3(model * glm::vec4(c.centre, 1.0f));
        const float radius = c.radius * scale;
        bool outside = false;
        if (view.cull) {
            for (const glm::vec4& p : planes) {
                if (glm::dot(glm::vec3(p), centre) + p.w < -radius) {
                    outside = true;
                    break;
                }
            }
        }
        if (outside) {
            ++counts.culled;
            continue;
        }
        const float distance = std::max(glm::length(eye - centre) - radius, 0.05f);
        const float pixels = c.error * scale * pixelScale / distance;
        // errorPixels <= 0 forces the leaves: a shadow pass or a hero shot wants the source surface, not a budget.
        const bool refine = c.childCount > 0 && (errorPixels <= 0.0f || pixels > errorPixels);
        if (refine) {
            for (uint32_t i = 0; i < c.childCount; ++i) stack.push_back(mesh.children[c.childFirst + i]);
        } else {
            out.push_back(id);
            ++counts.clusters;
            if (c.childCount == 0) ++counts.leaves;
            counts.triangles += c.triangleCount();
        }
    }
    if (stats) *stats = counts;
}

std::vector<uint32_t> selectVGeom(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view,
                                  float errorPixels, VGeomStats* stats) {
    std::vector<uint32_t> out;
    selectVGeom(mesh, model, view, errorPixels, out, stats);
    return out;
}

namespace {

template <typename T>
void writePod(std::ostream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
bool readPod(std::istream& in, T* v) {
    return bool(in.read(reinterpret_cast<char*>(v), sizeof(T)));
}

// The tree, without the file header: the single-mesh file and the per-model set share it so there is one reader and
// one writer for the layout, and the set can put several of these back to back.
void writeMesh(std::ostream& out, const VGeomMesh& mesh) {
    writePod(out, kMagic);
    writePod(out, kVersion);
    writePod(out, mesh.root);
    writePod(out, mesh.levels);
    writePod(out, mesh.sourceTriangles);
    writePod(out, mesh.vertexCount);
    writePod(out, mesh.sourceHash);
    writePod(out, uint64_t(mesh.clusters.size()));
    for (const VGeomCluster& c : mesh.clusters) {
        writePod(out, c.firstIndex);
        writePod(out, c.indexCount);
        writePod(out, c.level);
        writePod(out, c.parent);
        writePod(out, c.childFirst);
        writePod(out, c.childCount);
        writePod(out, c.centre.x);
        writePod(out, c.centre.y);
        writePod(out, c.centre.z);
        writePod(out, c.radius);
        writePod(out, c.error);
    }
    writePod(out, uint64_t(mesh.children.size()));
    if (!mesh.children.empty()) out.write(reinterpret_cast<const char*>(mesh.children.data()),
                                          std::streamsize(mesh.children.size() * sizeof(uint32_t)));
    writePod(out, uint64_t(mesh.indices.size()));
    if (!mesh.indices.empty())
        out.write(reinterpret_cast<const char*>(mesh.indices.data()), std::streamsize(mesh.indices.size() * sizeof(uint32_t)));
}

bool readMesh(std::istream& in, VGeomMesh& mesh) {
    uint64_t magic = 0;
    uint32_t version = 0;
    if (!readPod(in, &magic) || magic != kMagic || !readPod(in, &version) || version != kVersion) return false;
    VGeomMesh loaded;
    uint64_t count = 0;
    if (!readPod(in, &loaded.root) || !readPod(in, &loaded.levels) || !readPod(in, &loaded.sourceTriangles) ||
        !readPod(in, &loaded.vertexCount) || !readPod(in, &loaded.sourceHash) || !readPod(in, &count) ||
        count > 100000000ull)
        return false;
    loaded.clusters.resize(size_t(count));
    for (VGeomCluster& c : loaded.clusters) {
        if (!readPod(in, &c.firstIndex) || !readPod(in, &c.indexCount) || !readPod(in, &c.level) ||
            !readPod(in, &c.parent) || !readPod(in, &c.childFirst) || !readPod(in, &c.childCount) ||
            !readPod(in, &c.centre.x) || !readPod(in, &c.centre.y) || !readPod(in, &c.centre.z) ||
            !readPod(in, &c.radius) || !readPod(in, &c.error))
            return false;
    }
    if (!readPod(in, &count) || count > 100000000ull) return false;
    loaded.children.resize(size_t(count));
    if (count && !in.read(reinterpret_cast<char*>(loaded.children.data()), std::streamsize(count * sizeof(uint32_t))))
        return false;
    if (!readPod(in, &count) || count > 100000000ull) return false;
    loaded.indices.resize(size_t(count));
    if (count && !in.read(reinterpret_cast<char*>(loaded.indices.data()), std::streamsize(count * sizeof(uint32_t))))
        return false;
    if (!in) return false;
    mesh = std::move(loaded);
    return mesh.valid();
}

constexpr uint64_t kSetMagic = 0x54455347564B55ull;   // "UKVGST" little-endian, low byte 'T'
constexpr uint32_t kSetVersion = 1;

uint32_t hashOf(const void* data, size_t bytes) {
    // FNV-1a. The cache is validated by exact geometry hash anyway; this only has to notice a changed setting.
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

} // namespace

uint32_t vgeomSettingsHash(const VGeomSettings& s) {
    const uint32_t fields[6] = {s.clusterTriangles, s.groupSize, s.minTriangles, s.maxLevels, 0u, 0u};
    uint32_t h = hashOf(fields, sizeof(fields));
    // targetRatio is a float that only ever takes simple values (0.5, 0.25, ...), so its bits are stable to hash.
    const uint32_t ratio = hashOf(&s.targetRatio, sizeof(s.targetRatio));
    return h ^ (ratio * 16777619u);
}

bool saveVGeom(const VGeomMesh& mesh, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    writeMesh(out, mesh);
    return bool(out);
}

bool loadVGeom(VGeomMesh& mesh, const std::string& path, uint64_t expectHash) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    VGeomMesh loaded;
    if (!readMesh(in, loaded)) return false;
    if (expectHash != 0 && loaded.sourceHash != expectHash) return false;
    mesh = std::move(loaded);
    return true;
}

bool saveVGeomSet(const std::vector<VGeomMesh>& meshes, const std::string& path, const VGeomSettings& settings) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    writePod(out, kSetMagic);
    writePod(out, kSetVersion);
    writePod(out, vgeomSettingsHash(settings));
    writePod(out, uint64_t(meshes.size()));
    for (const VGeomMesh& mesh : meshes) {
        std::ostringstream blob(std::ios::binary);
        writeMesh(blob, mesh);
        const std::string bytes = blob.str();
        writePod(out, uint64_t(bytes.size()));
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    return bool(out);
}

bool loadVGeomSet(std::vector<VGeomMesh>& meshes, const std::string& path, const std::vector<uint64_t>& sourceHashes,
                  const VGeomSettings& settings) {
    meshes.assign(sourceHashes.size(), VGeomMesh{});
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint64_t magic = 0;
    uint32_t version = 0;
    uint32_t settingsHash = 0;
    uint64_t count = 0;
    if (!readPod(in, &magic) || magic != kSetMagic || !readPod(in, &version) || version != kSetVersion ||
        !readPod(in, &settingsHash) || settingsHash != vgeomSettingsHash(settings) || !readPod(in, &count) ||
        count != sourceHashes.size())
        return false;
    size_t accepted = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t bytes = 0;
        if (!readPod(in, &bytes) || bytes > 1ull << 32) return false;
        std::string blob(size_t(bytes), '\0');
        if (bytes && !in.read(blob.data(), std::streamsize(bytes))) return false;
        std::istringstream entry(blob, std::ios::binary);
        VGeomMesh mesh;
        if (readMesh(entry, mesh) && mesh.sourceHash == sourceHashes[size_t(i)]) {
            meshes[size_t(i)] = std::move(mesh);
            ++accepted;
        }
    }
    return accepted > 0;
}

} // namespace ukiyo
