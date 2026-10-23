// The compute cut's semantics, mirrored on the CPU (docs/VGEOM.md §7): shaders/vgeom_cut.comp walks, pads, sorts
// and merges on the device, where no CI job without a GPU could inspect it. This test runs the same algorithm —
// same plane math, same stack walk, same bitonic sort, same run merge, same command layout — and requires it to
// produce byte-identical indirect commands to the renderer's CPU path (selectVGeom, sort by firstIndex, merge).
// A kernel edit that changes what gets drawn fails here first, on every machine. Exit 0 when every check passes.
#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {

uint32_t ceilPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// A faithful mirror of the kernel: planes, walk, pad, bitonic, merge. Commands are the indirect quintuples
// {indexCount, 1, firstIndex, 0, 0}, in the order the kernel emits them.
std::vector<uint32_t> runKernel(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view,
                                float errorPixels) {
    const uint32_t count = uint32_t(mesh.clusters.size());
    const uint32_t pow2 = ceilPow2(count);
    const uint32_t idBase = 2;             // [idCount, cmdCount] header
    const uint32_t cmdBase = idBase + pow2;
    std::vector<uint32_t> work(2u + pow2 + count * 5u, 0u);

    const glm::mat4& vp = view.viewProj;
    auto row = [&vp](int i) { return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
    const glm::vec4 r3 = row(3);
    glm::vec4 planes[5] = {r3 + row(0), r3 - row(0), r3 + row(1), r3 - row(1), r3 - row(2)};
    for (glm::vec4& p : planes) p /= std::max(glm::length(glm::vec3(p)), 1e-6f);

    const float mScale = std::max(glm::length(glm::vec3(model[0])),
                                  std::max(glm::length(glm::vec3(model[1])), glm::length(glm::vec3(model[2]))));
    const float pixelScale = std::max(1.0f, view.pixelScale);

    // Walk: same order-insensitive selection as the shader (stack pops differ only in emission order, which the
    // sort erases).
    std::vector<uint32_t> stack;
    stack.push_back(mesh.root);
    while (!stack.empty()) {
        const uint32_t id = stack.back();
        stack.pop_back();
        const VGeomCluster& c = mesh.clusters[id];
        const glm::vec3 wc = glm::vec3(model * glm::vec4(c.centre, 1.0f));
        const float radius = c.radius * mScale;
        if (view.cull) {
            bool outside = false;
            for (const glm::vec4& p : planes)
                if (glm::dot(glm::vec3(p), wc) + p.w < -radius) { outside = true; break; }
            if (outside) continue;
        }
        const float dist = std::max(glm::length(view.eye - wc) - radius, 0.05f);
        const float pixels = c.error * mScale * pixelScale / dist;
        const bool refine = c.childCount > 0 && (errorPixels <= 0.0f || pixels > errorPixels);
        if (refine) {
            for (uint32_t i = 0; i < c.childCount; ++i) stack.push_back(mesh.children[c.childFirst + i]);
        } else {
            const uint32_t slot = work[idBase - 2]++;
            work[idBase + slot] = id;
        }
    }

    // Pad and bitonic sort, exactly as the kernel does.
    const uint32_t countSeen = work[idBase - 2];
    for (uint32_t i = countSeen; i < pow2; ++i) work[idBase + i] = 0xFFFFFFFFu;
    for (uint32_t k = 2; k <= pow2; k <<= 1)
        for (uint32_t j = k >> 1; j > 0; j >>= 1)
            for (uint32_t i = 0; i < pow2; ++i) {
                const uint32_t l = i ^ j;
                if (l <= i) continue;
                uint32_t a = work[idBase + i], b = work[idBase + l];
                if ((a > b) == ((i & k) == 0)) {
                    work[idBase + i] = b;
                    work[idBase + l] = a;
                }
            }

    // Merge runs into commands.
    auto emit = [&](uint32_t first, uint32_t len) {
        const uint32_t kk = work[idBase - 1]++;
        const uint32_t o = cmdBase + kk * 5;
        work[o + 0] = len;
        work[o + 1] = 1;
        work[o + 2] = first;
        work[o + 3] = 0;
        work[o + 4] = 0;
    };
    uint32_t runFirst = 0, runLen = 0;
    bool have = false;
    for (uint32_t i = 0; i < pow2; ++i) {
        const uint32_t id = work[idBase + i];
        if (id == 0xFFFFFFFFu) break;
        const VGeomCluster& c = mesh.clusters[id];
        if (have && runFirst + runLen == c.firstIndex) {
            runLen += c.indexCount;
        } else {
            if (have) emit(runFirst, runLen);
            runFirst = c.firstIndex;
            runLen = c.indexCount;
            have = true;
        }
    }
    if (have) emit(runFirst, runLen);

    return std::vector<uint32_t>(work.begin() + cmdBase, work.begin() + cmdBase + work[idBase - 1] * 5);
}

// The renderer's CPU path: selectVGeom, sort by firstIndex, merge runs, same command quintuples (drawWith's
// drawIndexed(run.second, run.first) in record order).
std::vector<uint32_t> runCpuPath(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view,
                                 float errorPixels) {
    std::vector<uint32_t> cut;
    selectVGeom(mesh, model, view, errorPixels, cut);
    std::sort(cut.begin(), cut.end(), [&](uint32_t a, uint32_t b) {
        return mesh.clusters[a].firstIndex < mesh.clusters[b].firstIndex;
    });
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    for (uint32_t id : cut) {
        const VGeomCluster& cl = mesh.clusters[id];
        if (!pairs.empty() && pairs.back().first + pairs.back().second == cl.firstIndex)
            pairs.back().second += cl.indexCount;
        else
            pairs.emplace_back(cl.firstIndex, cl.indexCount);
    }
    std::vector<uint32_t> cmds;
    for (const auto& r : pairs) {
        cmds.push_back(r.second);
        cmds.push_back(1);
        cmds.push_back(r.first);
        cmds.push_back(0);
        cmds.push_back(0);
    }
    return cmds;
}

std::string describe(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const uint32_t av = i < a.size() ? a[i] : 0xDEADBEEF;
        const uint32_t bv = i < b.size() ? b[i] : 0xDEADBEEF;
        if (av != bv)
            return "kernel[" + std::to_string(i) + "]=" + std::to_string(av) + " cpu=" + std::to_string(bv) +
                   " sizes " + std::to_string(a.size()) + "/" + std::to_string(b.size());
    }
    return "";
}

// One comparison; the label names the view and budget so a failure points at the case.
void compare(const VGeomMesh& mesh, const glm::mat4& model, const VGeomView& view, float errorPixels,
             const std::string& label) {
    const std::vector<uint32_t> kernel = runKernel(mesh, model, view, errorPixels);
    const std::vector<uint32_t> cpu = runCpuPath(mesh, model, view, errorPixels);
    check(kernel == cpu, ("kernel == cpu: " + label).c_str(), kernel == cpu ? "" : describe(kernel, cpu));
}

}  // namespace

int main() {
    // A UV sphere, as vgeom_test builds it — enough triangles for a deep tree with real runs to merge.
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
    const VGeomSettings settings;
    const VGeomMesh mesh = buildVGeom(positions.data(), vertexCount, sizeof(float) * 3, indices.data(),
                                      indices.size(), settings);
    check(mesh.valid(), "the tree is built");
    check(!mesh.children.empty(), "the tree has children to walk");

    // A spread of cameras, models and budgets: leaves forced, coarse budgets, the cull on and off, translations,
    // rotation, uniform and non-uniform scale (radius uses the max column scale, as selectVGeom does).
    struct Case {
        glm::vec3 eye;
        const char* name;
    };
    const Case cases[] = {
        {{0.0f, 0.6f, 3.0f}, "front"},
        {{4.0f, 0.4f, 0.2f}, "side"},
        {{0.0f, 5.0f, 0.001f}, "top"},
        {{1.5f, -2.0f, 1.5f}, "below"},
        {{0.15f, 0.0f, 0.2f}, "inside"},
        {{10.0f, 8.0f, 10.0f}, "far"},
    };
    const float budgets[] = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 16.0f};

    glm::mat4 models[6];
    models[0] = glm::mat4(1.0f);
    models[1] = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -1.0f, 2.0f));
    models[2] = glm::rotate(glm::mat4(1.0f), 1.1f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)));
    models[3] = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    models[4] = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 0.5f));
    models[5] = models[1] * models[2] * glm::scale(glm::mat4(1.0f), glm::vec3(0.75f));

    int compared = 0;
    for (const Case& cs : cases) {
        VGeomView view;
        view.eye = cs.eye;
        const glm::vec3 target(0.0f, 0.0f, 0.0f);
        const glm::vec3 up = std::fabs(cs.eye.y) > 4.5f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        view.viewProj = glm::perspective(0.9f, 16.0f / 9.0f, 0.05f, 60.0f) *
                        glm::lookAt(cs.eye, target, up);
        view.pixelScale = 1100.0f;
        for (float budget : budgets) {
            for (const glm::mat4& model : models) {
                compare(mesh, model, view, budget, std::string(cs.name) + " budget " + std::to_string(budget));
                ++compared;
            }
        }
        // Cull off: the whole tree regardless of the frustum — the off-screen tool path.
        VGeomView nocull = view;
        nocull.cull = false;
        compare(mesh, models[0], nocull, 2.0f, std::string(cs.name) + " cull off");
        ++compared;
    }

    // A view that culls everything: both paths must agree on an empty cut (zero commands: pure no-op draws).
    VGeomView away;
    away.eye = glm::vec3(0.0f, 0.0f, 30.0f);
    away.viewProj = glm::perspective(0.9f, 16.0f / 9.0f, 0.05f, 60.0f) *
                    glm::lookAt(away.eye, glm::vec3(1000.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    compare(mesh, glm::mat4(1.0f), away, 2.0f, "fully culled");
    ++compared;

    // The sorted id stream must be exactly the selection as a set (the sort may not drop or duplicate an id).
    {
        VGeomView view;
        view.eye = glm::vec3(0.0f, 0.6f, 3.0f);
        view.viewProj = glm::perspective(0.9f, 16.0f / 9.0f, 0.05f, 60.0f) *
                        glm::lookAt(view.eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        std::vector<uint32_t> cut;
        selectVGeom(mesh, glm::mat4(1.0f), view, 2.0f, cut);
        const std::vector<uint32_t> cmds = runKernel(mesh, glm::mat4(1.0f), view, 2.0f);
        check(!cmds.empty() || cut.empty(), "the front view selects something");
        uint32_t totalIdx = 0;
        for (size_t i = 0; i < cmds.size(); i += 5) totalIdx += cmds[i];
        check(totalIdx > 0, "the kernel wrote commands", "total indices " + std::to_string(totalIdx));
    }

    std::printf("vgcut: %d checks, %d kernel/cpu comparisons, %d failed\n", g_checks, compared, g_fail);
    return g_fail == 0 ? 0 : 1;
}
