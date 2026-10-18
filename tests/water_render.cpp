// Renders the water surface headlessly on Vulkan and writes PNGs (item 16). Checks that the water, copy and
// underwater passes run without error and that the frame is not blank. A backend that cannot start is SKIP.
// Usage: water_render <outdir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/camera.h"
#include "engine/renderer.h"
#include "engine/water.h"
#include "rhi/rhi.h"
#include "stb_image_write.h"

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

// Mean colour and spread of the frame. A blank frame has no spread.
void stats(const std::vector<uint8_t>& px, double* mean, double* spread) {
    double sum = 0.0, sum2 = 0.0;
    const size_t n = px.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        const double l = 0.299 * px[i * 4] + 0.587 * px[i * 4 + 1] + 0.114 * px[i * 4 + 2];
        sum += l;
        sum2 += l * l;
    }
    *mean = sum / double(n);
    *spread = std::sqrt(std::max(0.0, sum2 / double(n) - (*mean) * (*mean)));
}

} // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : ".";
    const uint32_t W = 640, H = 360;
    rhi::DeviceConfig cfg;
    cfg.width = W;
    cfg.height = H;
    cfg.headless = true;
    std::unique_ptr<rhi::Device> dev;
    try {
        dev = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
    } catch (const std::exception& e) {
        std::printf("SKIP  no Vulkan device (%s)\n", e.what());
        return 0;
    }
    std::printf("-- Vulkan on %s\n", dev->deviceName().c_str());

    ukiyo::Renderer renderer(*dev);
    ukiyo::WaterVolume water;
    water.half = glm::vec2(30.0f);
    water.level = 0.0f;
    renderer.setWater(&water);
    ukiyo::RenderSettings settings;

    auto shoot = [&](const ukiyo::OrbitCamera& cam, const std::string& name, double* mean, double* spread) {
        bool ok = true;
        for (uint32_t i = 0; i < 3; ++i) {
            if (!dev->beginFrame()) {
                ok = false;
                break;
            }
            renderer.render(cam, {}, 1.0f + 0.1f * float(i), i, settings);
            dev->endFrame(i == 2);
        }
        if (!ok) return false;
        std::vector<uint8_t> px = dev->takeCapture();
        if (px.size() != size_t(W) * H * 4) return false;
        stats(px, mean, spread);
        const std::string path = outDir + "/" + name;
        return stbi_write_png(path.c_str(), int(W), int(H), 4, px.data(), int(W * 4)) != 0;
    };

    // Above the surface: sky, water with Fresnel reflection, crest foam.
    ukiyo::OrbitCamera above;
    above.target = glm::vec3(0.0f, 0.0f, 0.0f);
    above.distance = 12.0f;
    above.pitch = 0.18f;
    double mean = 0.0, spread = 0.0;
    const bool wrote = shoot(above, "water_above.png", &mean, &spread);
    check(wrote, "water: the frame above the surface renders and writes");
    check(spread > 2.0, "water: the frame has detail", "luma spread " + std::to_string(spread));

    // Below the surface: the underwater fog pass runs.
    ukiyo::OrbitCamera under;
    under.target = glm::vec3(0.0f, -0.8f, 0.0f);
    under.distance = 3.0f;
    under.pitch = 0.0f;
    double meanU = 0.0, spreadU = 0.0;
    const bool wroteU = shoot(under, "water_under.png", &meanU, &spreadU);
    check(wroteU, "water: the frame under the surface renders and writes");
    check(std::fabs(meanU - mean) > 1.0, "water: the underwater frame differs from the one above");

    // Without water the same view has no surface, so the frame differs.
    renderer.setWater(nullptr);
    double meanN = 0.0, spreadN = 0.0;
    const bool wroteN = shoot(above, "no_water.png", &meanN, &spreadN);
    check(wroteN && std::fabs(meanN - mean) > 0.5, "water: removing the volume changes the frame");

    std::printf("water_render: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
