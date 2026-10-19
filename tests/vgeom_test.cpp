// CPU checks for virtual geometry (item 21, docs/VGEOM.md). The builder and the selector are plain CPU code, so this
// test needs no device: it builds a tree for a UV sphere, checks the invariants the renderer relies on, then exercises
// the cache. Exit code 0 when every check passes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/vgeom.h"

using namespace ukiyo;

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    ++g_checks;
    if (!ok) ++g_fail;
}

int main() {
    // A UV sphere: closed, symmetric, and enough triangles for five levels at the default settings.
    const int kSegments = 96, kRings = 48;
    std::vector<float> positions;
    for (int r = 0; r <= kRings; ++r) {
        const float v = float(r) / kRings, phi = v * 3.14159265f;
        for (int s = 0; s < kSegments; ++s) {
            const float u = float(s) / kSegments, th = u * 6.2831853f;
            positions.push_back(std::sin(phi) * std::cos(th));
            positions.push_back(std::cos(phi));
            positions.push_back(std::sin(phi) * std::sin(th));
        }
    }
    std::vector<uint32_t> indices;
    for (int r = 0; r < kRings; ++r) {
        for (int s = 0; s < kSegments; ++s) {
            const uint32_t a = uint32_t(r * kSegments + s), b = uint32_t(r * kSegments + (s + 1) % kSegments);
            const uint32_t c = uint32_t((r + 1) * kSegments + s), d = uint32_t((r + 1) * kSegments + (s + 1) % kSegments);
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    const size_t vertexCount = positions.size() / 3;
    const size_t triangleCount = indices.size() / 3;
    std::printf("sphere: %zu vertices, %zu triangles\n", vertexCount, triangleCount);

    const VGeomSettings settings;
    const VGeomMesh mesh = buildVGeom(positions.data(), vertexCount, sizeof(float) * 3, indices.data(), indices.size(),
                                      settings);
    check(mesh.valid(), "the tree is built");
    check(mesh.clusters[mesh.root].parent == kVGeomNoCluster, "the root has no parent");
    check(mesh.levels >= 3, "a 9k-triangle mesh gets several levels", std::to_string(mesh.levels) + " levels");

    // Every index range is inside the stream and is a whole number of triangles, and every index names a real vertex.
    bool rangesOk = true;
    for (const VGeomCluster& c : mesh.clusters) {
        rangesOk = rangesOk && c.firstIndex % 3 == 0 && c.indexCount % 3 == 0 &&
                   size_t(c.firstIndex) + c.indexCount <= mesh.indices.size();
        for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; ++i)
            rangesOk = rangesOk && mesh.indices[i] < vertexCount;
    }
    check(rangesOk, "every cluster range is valid");

    // The leaves cover the source exactly once: no triangle missing (a hole), none twice (z-fighting every frame).
    std::set<std::vector<uint32_t>> sourceTris, leafTris;
    auto triKey = [&](const uint32_t* t) {
        uint32_t v[3] = {t[0], t[1], t[2]};
        std::sort(v, v + 3);
        return std::vector<uint32_t>{v[0], v[1], v[2]};
    };
    for (size_t t = 0; t < triangleCount; ++t) sourceTris.insert(triKey(&indices[t * 3]));
    size_t leaves = 0, leafTriangles = 0;
    for (const VGeomCluster& c : mesh.clusters) {
        if (c.childCount != 0) continue;
        ++leaves;
        for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; i += 3) {
            leafTris.insert(triKey(&mesh.indices[i]));
            ++leafTriangles;
        }
    }
    check(leafTriangles == triangleCount, "the leaves hold every source triangle",
          std::to_string(leafTriangles) + " of " + std::to_string(triangleCount));
    check(leafTris == sourceTris, "no triangle appears twice across the leaves");

    // A parent is an error bound for its children and is coarser than they are together.
    bool errorsOk = true, coarserOk = true;
    for (const VGeomCluster& c : mesh.clusters) {
        size_t childTriangles = 0;
        for (uint32_t k = 0; k < c.childCount; ++k) {
            const VGeomCluster& child = mesh.clusters[mesh.children[c.childFirst + k]];
            errorsOk = errorsOk && c.error + 1e-6f >= child.error;
            childTriangles += child.triangleCount();
        }
        if (c.childCount) coarserOk = coarserOk && c.triangleCount() < childTriangles;
    }
    check(errorsOk, "a parent's error is at least its children's");
    check(coarserOk, "a parent is coarser than its children together");

    // A view from 12 m away, 100 degrees vertical, 900 px of pixel scale.
    const glm::mat4 identity(1.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 12), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(100.0f), 1.0f, 0.1f, 200.0f);
    VGeomView v;
    v.viewProj = proj * view;
    v.eye = glm::vec3(0, 0, 12);
    v.pixelScale = 900.0f;
    const glm::mat4 look = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -12));
    VGeomView fromLook;   // the same camera with the offset in the model matrix, as the renderer does it
    fromLook.viewProj = proj;
    fromLook.eye = glm::vec3(0);
    fromLook.pixelScale = 900.0f;

    // The closer the camera, the more triangles. This is the whole point of the tree.
    auto countForDistance = [&](float distance) {
        const glm::vec3 eye(0, 0, distance);
        const glm::mat4 mv = glm::lookAt(eye, glm::vec3(0), glm::vec3(0, 1, 0));
        VGeomView vv;
        vv.viewProj = proj * mv;
        vv.eye = eye;
        vv.pixelScale = 900.0f;
        VGeomStats stats;
        const auto cut = selectVGeom(mesh, identity, vv, 1.0f, &stats);
        size_t tris = 0;
        for (uint32_t id : cut) tris += mesh.clusters[id].triangleCount();
        return tris;
    };
    const size_t nearTris = countForDistance(2.0f), midTris = countForDistance(8.0f), farTris = countForDistance(60.0f);
    std::printf("distance 2 m: %zu, 8 m: %zu, 60 m: %zu triangles\n", nearTris, midTris, farTris);
    check(nearTris > midTris && midTris > farTris, "triangles fall with distance");
    check(nearTris <= triangleCount, "a near cut never exceeds the source");
    check(farTris > 0, "a far view still draws something");

    // The pixel budget is monotonic too: a looser budget can only remove triangles.
    size_t previous = triangleCount + 1;
    bool monotonic = true;
    for (float budget : {0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 64.0f}) {
        VGeomStats stats;
        const auto cut = selectVGeom(mesh, look, fromLook, budget, &stats);
        size_t tris = 0;
        for (uint32_t id : cut) tris += mesh.clusters[id].triangleCount();
        monotonic = monotonic && tris <= previous;
        previous = tris;
    }
    check(monotonic, "triangles fall as the pixel budget grows");

    // Zero pixels means the leaves: a shadow pass or a hero shot.
    {
        VGeomStats stats;
        const auto cut = selectVGeom(mesh, identity, v, 0.0f, &stats);
        size_t tris = 0;
        for (uint32_t id : cut) tris += mesh.clusters[id].triangleCount();
        check(tris == triangleCount && stats.culled == 0, "a zero budget inside the frustum equals the source");
    }

    // The renderer's out-parameter overload must agree with the returning one.
    {
        std::vector<uint32_t> out;
        VGeomStats a{}, b{};
        selectVGeom(mesh, identity, v, 1.0f, out, &a);
        const auto reference = selectVGeom(mesh, identity, v, 1.0f, &b);
        check(out == reference && a.clusters == b.clusters, "the out-parameter selector matches the returning one");
    }

    // Culling must be conservative: a triangle whose three vertices project inside the clip cube is on screen, so a
    // leaf cut has to contain it. An unnormalized frustum plane (whose coefficients carry the projection's scale)
    // over-culls here, which showed up as missing patches at the edges of the mesh.
    {
        const glm::vec3 eye(9.0f, 4.0f, 7.0f);
        const glm::mat4 offView = glm::lookAt(eye, glm::vec3(0.6f, 0.0f, 0.0f), glm::vec3(0, 1, 0));
        VGeomView off;
        off.viewProj = proj * offView;
        off.eye = eye;
        off.pixelScale = 900.0f;
        VGeomStats stats;
        const auto cut = selectVGeom(mesh, identity, off, 0.0f, &stats);
        std::set<std::vector<uint32_t>> cutTris;
        for (uint32_t id : cut) {
            const VGeomCluster& c = mesh.clusters[id];
            for (uint32_t i = c.firstIndex; i < c.firstIndex + c.indexCount; i += 3)
                cutTris.insert(triKey(&mesh.indices[i]));
        }
        size_t visible = 0, missing = 0;
        for (size_t t = 0; t < triangleCount; ++t) {
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) {
                const size_t vi = size_t(indices[t * 3 + k]) * 3;
                const glm::vec4 clip =
                    off.viewProj * glm::vec4(positions[vi], positions[vi + 1], positions[vi + 2], 1.0f);
                inside = clip.w > 0.0f && std::fabs(clip.x) <= clip.w && std::fabs(clip.y) <= clip.w;
            }
            if (!inside) continue;
            ++visible;
            if (!cutTris.count(triKey(&indices[t * 3]))) ++missing;
        }
        check(visible > 100, "an off-axis view sees part of the sphere", std::to_string(visible) + " triangles");
        check(missing == 0, "every fully visible triangle is in the leaf cut");
    }

    // Turning the frustum test off never culls.
    {
        VGeomView all = v;
        all.cull = false;
        VGeomStats stats;
        selectVGeom(mesh, identity, all, 64.0f, &stats);
        check(stats.culled == 0, "cull = false selects the whole tree");
    }

    // A cut with the camera far away keeps the root; the frustum test rejects the rest of the tree.
    {
        const glm::vec3 eye(0, 0, 12);
        const glm::mat4 away = glm::lookAt(eye, glm::vec3(200, 0, 12), glm::vec3(0, 1, 0));
        VGeomView vv;
        vv.viewProj = proj * away;
        vv.eye = eye;
        vv.pixelScale = 900.0f;
        VGeomStats stats;
        const auto cut = selectVGeom(mesh, identity, vv, 1.0f, &stats);
        check(cut.empty() && stats.culled > 0, "a mesh behind the camera is culled");
    }

    // Determinism: the same geometry and settings produce byte-identical streams (the cache relies on it).
    {
        const VGeomMesh again = buildVGeom(positions.data(), vertexCount, sizeof(float) * 3, indices.data(),
                                           indices.size(), settings);
        check(again.clusters.size() == mesh.clusters.size() && again.indices == mesh.indices,
              "the build is deterministic");
    }

    // Cache: a single mesh, with and without the expected hash.
    const std::string meshPath = "vgeom_test.vgeom";
    check(saveVGeom(mesh, meshPath), "save a mesh");
    VGeomMesh loaded;
    check(loadVGeom(loaded, meshPath, mesh.sourceHash), "load a mesh with its hash");
    check(loaded.clusters.size() == mesh.clusters.size() && loaded.indices == mesh.indices && loaded.root == mesh.root,
          "the round trip keeps the tree");
    check(!loadVGeom(loaded, meshPath, mesh.sourceHash ^ 1ull), "a stale hash is refused");

    // Cache: a model's set, where one primitive has changed since the cache was written.
    const std::string setPath = "vgeom_test.vgeomset";
    const VGeomMesh second = buildVGeom(positions.data(), vertexCount, sizeof(float) * 3, indices.data(), indices.size(),
                                        settings);
    check(saveVGeomSet({mesh, second}, setPath, settings), "save a primitive set");
    std::vector<VGeomMesh> set;
    check(loadVGeomSet(set, setPath, {mesh.sourceHash, second.sourceHash}, settings), "load a primitive set");
    check(set.size() == 2 && set[0].valid() && set[1].valid(), "both primitives come back");
    check(loadVGeomSet(set, setPath, {mesh.sourceHash, second.sourceHash ^ 1ull}, settings),
          "a set with one stale primitive is still usable");
    check(set.size() == 2 && set[0].valid() && !set[1].valid(), "the stale primitive is dropped, the other kept");
    check(!loadVGeomSet(set, setPath, {mesh.sourceHash, second.sourceHash}, VGeomSettings{64}),
          "a different build setting refuses the set");
    std::remove(meshPath.c_str());
    std::remove(setPath.c_str());

    if (g_fail) std::printf("VGEOM: %d of %d checks FAILED\n", g_fail, g_checks);
    else std::printf("VGEOM: all %d checks passed\n", g_checks);
    return g_fail ? 1 : 0;
}
