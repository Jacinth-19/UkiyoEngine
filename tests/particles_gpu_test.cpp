// GPU check for the particle quad pass. Renders known particles headlessly and reads back pixels.
// Runs on OpenGL (needs a display, e.g. xvfb-run) and on Vulkan (needs a Vulkan driver).
// A backend that is not available is reported as SKIP, not FAIL.
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/gpu_types.h"
#include "engine/particle_renderer.h"
#include "rhi/rhi.h"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

struct Rgba { int r, g, b, a; };

// Pixel at (x, y), with y counted from the top row.
Rgba pixelAt(const std::vector<uint8_t>& px, int w, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

// Runs the particle checks on one backend. Returns false if the backend could not be started.
bool runBackend(rhi::Backend backend, GLFWwindow* window, const char* label) {
    const int W = 128, H = 128;
    std::unique_ptr<rhi::Device> dev;
    try {
        rhi::DeviceConfig cfg;
        cfg.width = W;
        cfg.height = H;
        cfg.headless = true;
        dev = rhi::createDevice(backend, cfg, window);
    } catch (const std::exception& e) {
        std::printf("SKIP  %s: no device (%s)\n", label, e.what());
        return false;
    }
    std::printf("-- %s on %s\n", label, dev->deviceName().c_str());

    // Camera at z = 5 looking at the origin, 60 degree vertical field of view. At z = 0 the visible
    // half-height is 5 * tan(30 deg), about 2.887 world units.
    ukiyo::FrameUBO f{};
    const glm::vec3 eye(0.0f, 0.0f, 5.0f);
    f.view = glm::lookAtRH(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    f.proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 50.0f);
    f.viewProj = f.proj * f.view;
    f.invViewProj = glm::inverse(f.viewProj);
    f.camPos = glm::vec4(eye, 1.0f);
    f.screenParams = glm::vec4(float(W), float(H), 0.0f, 0.0f);
    for (auto& m : f.shadowMatrix) m = glm::mat4(1.0f);

    auto frameUbo = dev->createBuffer(rhi::BufferUsage::Uniform, sizeof(ukiyo::FrameUBO));
    frameUbo->upload(&f, sizeof(f));
    auto white = dev->createTexture(rhi::TextureDesc{}, std::vector<uint8_t>{255, 255, 255, 255}.data());
    auto frameSet = dev->createDescriptorSet(rhi::SetType::Frame);
    frameSet->setBuffer(0, frameUbo.get());
    frameSet->setTexture(1, white.get());

    rhi::TextureDesc dd;
    dd.width = W;
    dd.height = H;
    dd.format = rhi::Format::D32F;
    dd.filter = rhi::Filter::Nearest;
    auto depth = dev->createTexture(dd, nullptr);

    ukiyo::ParticleRenderer renderer(*dev, rhi::Format::Backbuffer, true);

    // Green at x = -1 (left), blue at x = +1 (right), red at y = +1 (top). All size 0.8, front of the origin.
    using ukiyo::ParticleInstance;
    auto makeInst = [](glm::vec3 p, float size, glm::vec4 c) {
        ParticleInstance i{};
        i.position[0] = p.x; i.position[1] = p.y; i.position[2] = p.z; i.position[3] = size;
        i.color[0] = c.r; i.color[1] = c.g; i.color[2] = c.b; i.color[3] = c.a;
        return i;
    };
    const std::vector<ParticleInstance> particles = {
        makeInst({-1.0f, 0.0f, 0.0f}, 0.8f, {0.0f, 1.0f, 0.0f, 1.0f}),
        makeInst({1.0f, 0.0f, 0.0f}, 0.8f, {0.0f, 0.0f, 1.0f, 1.0f}),
        makeInst({0.0f, 1.0f, 0.0f}, 0.8f, {1.0f, 0.0f, 0.0f, 1.0f}),
    };

    rhi::PassDesc pass;
    pass.backbuffer = true;
    pass.clearColor = true;
    pass.clearValue = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    pass.clearDepth = true;
    pass.depth = depth.get();

    uint32_t drawn = 0;
    if (!dev->beginFrame()) throw std::runtime_error("the device skipped the frame");
    dev->beginPass(pass);
    dev->setViewport(0, 0, W, H);
    drawn = renderer.draw(*dev, *frameSet, particles);
    dev->endPass();
    dev->endFrame(true);
    std::vector<uint8_t> px = dev->takeCapture();
    dev->waitIdle();

    check(drawn == 3, (std::string(label) + ": all three instances are drawn").c_str(), std::to_string(drawn));
    if (px.size() < size_t(W) * H * 4) {
        check(false, (std::string(label) + ": capture is the full frame").c_str());
        return true;
    }

    // Projected centres: x = -1 -> column ~42, x = +1 -> column ~86, y = +1 -> row ~42 (rows from the top).
    const Rgba left = pixelAt(px, W, 42, 64);
    const Rgba right = pixelAt(px, W, 86, 64);
    const Rgba top = pixelAt(px, W, 64, 42);
    const Rgba centre = pixelAt(px, W, 64, 64);
    const Rgba corner = pixelAt(px, W, 2, 2);

    auto isGreen = [](Rgba c) { return c.g > 200 && c.r < 60 && c.b < 60; };
    auto isBlue = [](Rgba c) { return c.b > 200 && c.r < 60 && c.g < 60; };
    auto isRed = [](Rgba c) { return c.r > 200 && c.g < 60 && c.b < 60; };
    auto isBlack = [](Rgba c) { return c.r < 8 && c.g < 8 && c.b < 8; };

    const std::string tag = label;
    check(isGreen(left), (tag + ": green particle at x = -1 is on the left").c_str(),
          "rgba " + std::to_string(left.r) + "," + std::to_string(left.g) + "," + std::to_string(left.b));
    check(isBlue(right), (tag + ": blue particle at x = +1 is on the right").c_str(),
          "rgba " + std::to_string(right.r) + "," + std::to_string(right.g) + "," + std::to_string(right.b));
    check(isRed(top), (tag + ": red particle at y = +1 is on top").c_str(),
          "rgba " + std::to_string(top.r) + "," + std::to_string(top.g) + "," + std::to_string(top.b));
    check(isBlack(centre), (tag + ": the gap between particles stays background").c_str());
    check(isBlack(corner), (tag + ": the quads stay small (a corner pixel is background)").c_str());

    // Over-limit call: one more than the block holds. Expect kMaxInstances drawn.
    std::vector<ParticleInstance> many(ukiyo::ParticleRenderer::kMaxInstances + 1, particles[0]);
    if (!dev->beginFrame()) throw std::runtime_error("the device skipped the frame");
    dev->beginPass(pass);
    dev->setViewport(0, 0, W, H);
    const uint32_t capped = renderer.draw(*dev, *frameSet, many);
    dev->endPass();
    dev->endFrame(false);
    dev->waitIdle();
    check(capped == ukiyo::ParticleRenderer::kMaxInstances, (tag + ": draws are capped at the block size").c_str(),
          std::to_string(capped));
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
        window = glfwCreateWindow(128, 128, "particles test", nullptr, nullptr);
    } else {
        std::printf("note: glfwInit failed (no display), so OpenGL is skipped\n");
    }

    try {
        if (window) {
            glfwMakeContextCurrent(window);
            runBackend(rhi::Backend::OpenGL, window, "OpenGL");
        }
    } catch (const std::exception& e) {
        check(false, "OpenGL: unexpected error", e.what());
    }
    try {
        runBackend(rhi::Backend::Vulkan, nullptr, "Vulkan");
    } catch (const std::exception& e) {
        check(false, "Vulkan: unexpected error", e.what());
    }

    std::printf("particles_gpu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
