// The compute path, end to end: a dispatch writes an indirect draw command, the draw reads it from the buffer, and
// pixels are the verdict (docs/VGEOM.md's GPU path exists on these primitives).
//
// Two captures per backend. The first runs the dispatch and then draws from the buffer with capacity 16 commands —
// fifteen of which nobody wrote — so it proves three things at once: the compute stage ran, the indirect draw
// consumed what it wrote, and all-zero commands outside the written range draw nothing (the safety contract a GPU
// producer with an atomic count depends on, never reading the count back). The second zeroes the buffer and draws
// again: black, which is what makes the first capture evidence rather than a coincidence. A device without compute
// reports no such thing here — both backends are core-4.3/Vulkan-1.0 features and every target this repository
// builds for has them; a missing *shader file* throws and is caught as the usual backend skip.
//
//   compute_test          (no arguments: OpenGL with a hidden window, Vulkan headless)
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/gpu_types.h"
#include "rhi/rhi.h"

namespace {

int g_pass = 0, g_fail = 0;
bool g_skipped = false;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

struct Pixels {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool lit(int x, int y) const {
        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
        return rgba[i] > 32 || rgba[i + 1] > 32 || rgba[i + 2] > 32;
    }
    int count() const {
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) n += lit(x, y) ? 1 : 0;
        return n;
    }
};

// The HUD quad's two triangles, in screen pixels: wireframe_test's geometry, which this test reuses so a failure
// difference is the compute path and not a different shape.
std::vector<rhi::Vertex> quadVertices(int w, int h) {
    const float x0 = 8.0f, y0 = 8.0f, x1 = float(w) - 8.0f, y1 = float(h) - 8.0f;
    auto v = [](float x, float y) {
        rhi::Vertex out{};
        out.pos = glm::vec3(x, y, 0.0f);
        out.uv = glm::vec2(0.5f, 0.5f);
        out.color = glm::vec4(1.0f);
        return out;
    };
    const rhi::Vertex a = v(x0, y0), b = v(x1, y0), c = v(x1, y1), d = v(x0, y1);
    std::vector<rhi::Vertex> verts;
    for (const rhi::Vertex& q : {a, b, c}) verts.push_back(q);
    for (const rhi::Vertex& q : {a, c, d}) verts.push_back(q);
    return verts;
}

constexpr uint32_t kCommandStride = 20;   // five uint32s, both backends
constexpr uint32_t kDrawCapacity = 16;    // one written command, fifteen all-zero no-ops

struct Scene {
    std::unique_ptr<rhi::Device> dev;
    std::unique_ptr<rhi::Buffer> vb;
    std::unique_ptr<rhi::Buffer> ib;
    std::unique_ptr<rhi::Buffer> indirect;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<rhi::DescriptorSet> hudSet;
    std::unique_ptr<rhi::DescriptorSet> computeSet;
    std::unique_ptr<rhi::Pipeline> graphics;
    std::unique_ptr<rhi::Pipeline> compute;
    uint32_t vertexCount = 0;
};

void zeroCommands(rhi::Buffer* buffer) {
    const std::vector<uint8_t> zeros(size_t(kDrawCapacity) * kCommandStride, 0);
    buffer->upload(zeros.data(), zeros.size());
}

bool runBackend(rhi::Backend backend, GLFWwindow* window, const char* label) {
    const int W = 128, H = 128;
    Scene s;
    try {
        rhi::DeviceConfig cfg;
        cfg.width = W;
        cfg.height = H;
        cfg.headless = true;
        s.dev = rhi::createDevice(backend, cfg, window);
    } catch (const std::exception& e) {
        std::printf("SKIP  %s: no device (%s)\n", label, e.what());
        g_skipped = true;
        return false;
    }
    std::printf("-- %s on %s\n", label, s.dev->deviceName().c_str());

    try {
        const std::vector<rhi::Vertex> verts = quadVertices(W, H);
        s.vertexCount = static_cast<uint32_t>(verts.size());
        s.vb = s.dev->createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
        s.vb->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
        const uint32_t indices[6] = {0, 1, 2, 3, 4, 5};
        s.ib = s.dev->createBuffer(rhi::BufferUsage::Index, sizeof(indices));
        s.ib->upload(indices, sizeof(indices));
        s.indirect = s.dev->createBuffer(rhi::BufferUsage::Indirect, size_t(kDrawCapacity) * kCommandStride);
        zeroCommands(s.indirect.get());

        const uint8_t white[4] = {255, 255, 255, 255};
        rhi::TextureDesc td;
        td.width = td.height = 1;
        td.format = rhi::Format::RGBA8;
        td.filter = rhi::Filter::Nearest;
        s.white = s.dev->createTexture(td, white);

        rhi::PipelineDesc g;
        g.vertexShader = "hud";
        g.fragmentShader = "hud";
        g.sets = {rhi::SetType::Hud};
        g.vertexInput = true;
        g.cull = rhi::Cull::None;
        g.colorFormats = {rhi::Format::Backbuffer};
        s.graphics = s.dev->createPipeline(g);
        s.hudSet = s.dev->createDescriptorSet(rhi::SetType::Hud);
        s.hudSet->setTexture(0, s.white.get());

        rhi::PipelineDesc c;
        c.computeShader = "indirect_fill";
        c.sets = {rhi::SetType::Compute};
        s.compute = s.dev->createPipeline(c);
        s.computeSet = s.dev->createDescriptorSet(rhi::SetType::Compute);
        s.computeSet->setBuffer(0, s.indirect.get());
    } catch (const std::exception& e) {
        std::printf("SKIP  %s: no compute pipeline (%s)\n", label, e.what());
        g_skipped = true;
        return false;
    }

    auto frame = [&](bool dispatch) -> Pixels {
        if (!s.dev->beginFrame()) return Pixels{};
        if (dispatch) {
            // Outside any render pass: bind the compute pipeline and its set, dispatch one workgroup.
            s.dev->bindPipeline(s.compute.get());
            s.dev->bindDescriptorSet(0, s.computeSet.get());
            s.dev->dispatch(1, 1, 1);
        }
        rhi::PassDesc pass;
        pass.backbuffer = true;
        pass.clearColor = true;
        pass.clearValue = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        pass.clearDepth = false;
        s.dev->beginPass(pass);
        s.dev->setViewport(0, 0, W, H);
        s.dev->bindPipeline(s.graphics.get());
        s.dev->bindDescriptorSet(0, s.hudSet.get());
        ukiyo::PushConstants pc{};
        pc.p0 = glm::vec4(float(W), float(H), 0.0f, 0.0f);
        s.dev->pushConstants(&pc, sizeof(pc));
        s.dev->bindVertexBuffer(s.vb.get());
        s.dev->bindIndexBuffer(s.ib.get());
        // Capacity, not count: the written command plus the fifteen zeros after it.
        s.dev->drawIndexedIndirect(s.indirect.get(), 0, kDrawCapacity);
        s.dev->endPass();
        s.dev->endFrame(true);
        Pixels px;
        px.rgba = s.dev->takeCapture();
        px.w = static_cast<int>(s.dev->backbufferWidth());
        px.h = static_cast<int>(s.dev->backbufferHeight());
        return px;
    };

    // A warm-up frame, thrown away: the first frame through a fresh device is where the backends create their
    // swapchain images and readback paths (wireframe_test's reasoning).
    frame(true);

    const Pixels lit = frame(true);
    if (lit.rgba.empty() || lit.w != W || lit.h != H ||
        lit.rgba.size() != size_t(lit.w) * lit.h * 4) {
        check(false, "captured the dispatch frame",
              lit.rgba.empty() ? std::string("no capture")
                               : std::to_string(lit.w) + "x" + std::to_string(lit.h));
        return true;
    }
    const int litCount = lit.count();
    const double litShare = double(litCount) / (double(W) * H);
    check(litShare > 0.55, "compute wrote the command and the indirect draw consumed it",
          std::to_string(litShare) + " of the frame lit");

    // Zero the buffer and draw the same capacity: black, or the first capture meant nothing.
    zeroCommands(s.indirect.get());
    const Pixels dark = frame(false);
    if (dark.rgba.empty() || dark.rgba.size() != size_t(dark.w) * dark.h * 4) {
        check(false, "captured the zeroed control frame");
        return true;
    }
    const int darkCount = dark.count();
    check(darkCount == 0, "with the buffer zeroed the same draw paints nothing",
          std::to_string(darkCount) + " lit px");
    check(darkCount < litCount / 8, "the two captures are visibly different",
          std::to_string(darkCount) + " vs " + std::to_string(litCount));
    return true;
}

} // namespace

int main() {
    GLFWwindow* window = nullptr;
    if (glfwInit()) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(128, 128, "compute test", nullptr, nullptr);
    }
    try {
        if (window) runBackend(rhi::Backend::OpenGL, window, "OpenGL");
    } catch (const std::exception& e) {
        check(false, "OpenGL: unexpected error", e.what());
    }
    try {
        runBackend(rhi::Backend::Vulkan, nullptr, "Vulkan");
    } catch (const std::exception& e) {
        check(false, "Vulkan: unexpected error", e.what());
    }
    if (window) glfwDestroyWindow(window);

    std::printf("compute_test: %d passed, %d failed%s\n", g_pass, g_fail, g_skipped ? " (some backends skipped)" : "");
    return g_fail == 0 ? 0 : 1;
}
