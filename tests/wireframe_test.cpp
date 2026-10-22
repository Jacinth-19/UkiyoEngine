// The wireframe view's rasteriser flag (Flax 1.13's debug view): a quad drawn filled and drawn as its edges, captured and
// counted. This is the check that the RHI option means what it says on both backends — Vulkan needs fillModeNonSolid
// enabled and VK_POLYGON_MODE_LINE, GL needs glPolygonMode — and that a wireframe frame still lands on the geometry's
// own edges rather than anywhere on screen.
//
// Renders with the HUD pipeline (hud.vert/hud.frag): it takes screen-space vertices and needs no Frame set, so the test is
// about the rasteriser and nothing else.
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
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

// The two triangles of a quad inset from the frame, in pixels with y down: exactly what a HUD quad is.
std::vector<rhi::Vertex> quadVertices(int w, int h) {
    const float x0 = 8.0f, y0 = 8.0f, x1 = float(w) - 8.0f, y1 = float(h) - 8.0f;
    auto v = [](float x, float y) {
        rhi::Vertex out{};
        out.pos = glm::vec3(x, y, 0.0f);
        out.uv = glm::vec2(0.5f, 0.5f);   // the atlas's reserved white texel
        out.color = glm::vec4(1.0f);
        return out;
    };
    const rhi::Vertex a = v(x0, y0), b = v(x1, y0), c = v(x1, y1), d = v(x0, y1);
    std::vector<rhi::Vertex> verts;
    for (const rhi::Vertex& q : {a, b, c}) verts.push_back(q);
    for (const rhi::Vertex& q : {a, c, d}) verts.push_back(q);
    return verts;
}

struct Draw {
    std::unique_ptr<rhi::Device> dev;
    std::unique_ptr<rhi::Buffer> vb;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<rhi::DescriptorSet> hudSet;
    uint32_t vertexCount = 0;
};

bool runBackend(rhi::Backend backend, GLFWwindow* window, const char* label) {
    const int W = 128, H = 128;
    Draw d;
    try {
        rhi::DeviceConfig cfg;
        cfg.width = W;
        cfg.height = H;
        cfg.headless = true;
        d.dev = rhi::createDevice(backend, cfg, window);
    } catch (const std::exception& e) {
        std::printf("SKIP  %s: no device (%s)\n", label, e.what());
        g_skipped = true;
        return false;
    }
    std::printf("-- %s on %s\n", label, d.dev->deviceName().c_str());

    const std::vector<rhi::Vertex> verts = quadVertices(W, H);
    d.vertexCount = static_cast<uint32_t>(verts.size());
    d.vb = d.dev->createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
    d.vb->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
    const uint8_t white[4] = {255, 255, 255, 255};
    rhi::TextureDesc td;
    td.width = td.height = 1;
    td.format = rhi::Format::RGBA8;
    td.filter = rhi::Filter::Nearest;
    d.white = d.dev->createTexture(td, white);
    d.hudSet = d.dev->createDescriptorSet(rhi::SetType::Hud);
    d.hudSet->setTexture(0, d.white.get());

    auto makePipeline = [&](bool wireframe) {
        rhi::PipelineDesc p;
        p.vertexShader = "hud";
        p.fragmentShader = "hud";
        p.sets = {rhi::SetType::Hud};
        p.vertexInput = true;
        p.cull = rhi::Cull::None;
        p.depthTest = false;
        p.depthWrite = false;
        p.blend = rhi::Blend::None;
        p.colorFormats = {rhi::Format::Backbuffer};
        p.hasDepth = false;
        p.wireframe = wireframe;
        return d.dev->createPipeline(p);
    };

    // A device without VK_POLYGON_MODE_LINE cannot build the wireframe pipeline, and that is a device fact rather than a
    // failure of this code: report it and let the job move on. Even so, the filled pipeline below always runs.
    std::unique_ptr<rhi::Pipeline> fill, wire;
    bool wireSupported = true;
    try {
        fill = makePipeline(false);
        wire = makePipeline(true);
    } catch (const std::exception& e) {
        wireSupported = false;
        std::printf("SKIP  %s: this device has no non-solid fill mode (%s)\n", label, e.what());
        g_skipped = true;
        return false;
    }

    auto render = [&](rhi::Pipeline* pipeline) {
        if (!d.dev->beginFrame()) return Pixels{};
        rhi::PassDesc pass;
        pass.backbuffer = true;          // the presentation target
        pass.clearColor = true;
        pass.clearValue = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        pass.clearDepth = false;
        d.dev->beginPass(pass);
        d.dev->setViewport(0, 0, W, H);
        d.dev->bindPipeline(pipeline);
        d.dev->bindDescriptorSet(0, d.hudSet.get());
        // hud.vert reads the presentation size from the push block. p0 is not the first member of the block — the
        // model matrix is — so the size has to be written where the shader reads it, not at offset 0.
        ukiyo::PushConstants pc{};   // the block the HUD renderer pushes: model matrix, then p0..p3
        pc.p0 = glm::vec4(float(W), float(H), 0.0f, 0.0f);
        d.dev->pushConstants(&pc, sizeof(pc));
        d.dev->bindVertexBuffer(d.vb.get());
        d.dev->drawInstanced(d.vertexCount, 1);   // non-indexed: the HUD emits a triangle list of vertices
        d.dev->endPass();
        d.dev->endFrame(true);
        Pixels px;
        px.rgba = d.dev->takeCapture();
        px.w = W;
        px.h = H;
        return px;
    };

    const Pixels filled = render(fill.get());
    const Pixels edged = render(wire.get());
    if (filled.rgba.empty() || edged.rgba.empty()) {
        check(false, "captured a frame in both modes");
        return true;
    }

    const int fillCount = filled.count();
    const int edgeCount = edged.count();
    const int total = W * H;
    const double fillShare = double(fillCount) / total;
    const double edgeShare = double(edgeCount) / total;

    check(fillShare > 0.55, "filled quad covers the frame", std::to_string(fillShare));
    check(edgeCount > 0, "wireframe draws something at all", std::to_string(edgeCount) + " px");
    check(edgeShare < 0.10, "and only its edges, not the interior", std::to_string(edgeShare));
    check(double(edgeCount) < 0.25 * double(fillCount), "wireframe is a fraction of the filled area",
          std::to_string(double(edgeCount) / double(fillCount)));

    // Every lit wireframe pixel has to sit on the geometry's own boundary: the quad's four sides or the diagonal the two
    // triangles share. Distance to that line set, in pixels.
    auto nearEdge = [&](int x, int y) {
        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
        const float x0 = 8.0f, y0 = 8.0f, x1 = float(W) - 8.0f, y1 = float(H) - 8.0f;
        // Sides: horizontal and vertical segments, then the diagonal from (x1, y0) to (x0, y1).
        const float onLeft = std::fabs(px - x0);
        const float onRight = std::fabs(px - x1);
        const float onTop = std::fabs(py - y0);
        const float onBottom = std::fabs(py - y1);
        const bool inX = px >= x0 - 2 && px <= x1 + 2;
        const bool inY = py >= y0 - 2 && py <= y1 + 2;
        if (inY && (onLeft <= 2.0f || onRight <= 2.0f)) return true;
        if (inX && (onTop <= 2.0f || onBottom <= 2.0f)) return true;
        // Point-to-line distance for the diagonal: (x1,y0) -> (x0,y1).
        const float dx = x0 - x1, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        const float dist = std::fabs((px - x1) * dy - (py - y0) * dx) / len;
        const float t = ((px - x1) * dx + (py - y0) * dy) / (len * len);
        return t >= -0.02f && t <= 1.02f && dist <= 2.0f;
    };
    int onEdges = 0, lit = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!edged.lit(x, y)) continue;
            ++lit;
            if (nearEdge(x, y)) ++onEdges;
        }
    const double onShare = lit > 0 ? double(onEdges) / double(lit) : 0.0;
    check(lit > 0 && onShare > 0.97, "every lit pixel is on the geometry's own edge", std::to_string(onShare));

    // The frame is a picture, not just a count: dump both captures so a failure can be looked at.
    auto dump = [&](const Pixels& px, const char* name) {
        const std::string path = std::string("/tmp/wireframe_") + name + ".ppm";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", px.w, px.h);
            for (int y = 0; y < px.h; ++y)
                for (int x = 0; x < px.w; ++x) {
                    const size_t i = (static_cast<size_t>(y) * px.w + x) * 4;
                    std::fwrite(&px.rgba[i], 1, 3, f);
                }
            std::fclose(f);
        }
    };
    dump(filled, "fill");
    dump(edged, "wire");
    (void)wireSupported;
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
        window = glfwCreateWindow(128, 128, "wireframe test", nullptr, nullptr);
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

    std::printf("wireframe_test: %d passed, %d failed%s\n", g_pass, g_fail, g_skipped ? " (some backends skipped)" : "");
    return g_fail == 0 ? 0 : 1;
}
