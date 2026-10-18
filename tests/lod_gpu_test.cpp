// GPU check for opt-in mesh LOD (item 15). Loads a real model on the headless Vulkan device, builds LOD levels
// with buildModelLods, and checks the level counts. Exit code 0 when all checks pass.
#include <cstdio>
#include <memory>
#include <string>

#include "engine/model.h"
#include "rhi/rhi.h"

using namespace ukiyo;

static int g_fail = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
    const std::string modelPath = argc > 1 ? argv[1] : "assets/AliciaSolid_vrm-0.51.vrm";
    try {
        rhi::DeviceConfig cfg;
        cfg.width = 320;
        cfg.height = 180;
        cfg.headless = true;
        auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
        auto model = loadModel(*device, modelPath);
        model->physics = false;

        check(model->lodSwitch.empty(), "lod off by default: no switches on a fresh model");
        bool noLevels = true;
        for (const Primitive& p : model->primitives) noLevels = noLevels && p.lods.empty();
        check(noLevels, "lod off by default: no primitive has levels");

        const std::vector<float> switches = {25.0f, 60.0f};
        const int built = buildModelLods(*device, *model, switches);
        check(built > 0, "lod on: levels were built", std::to_string(built) + " levels");
        check(model->lodSwitch == switches, "lod on: switches are stored on the model");

        bool ok = true;
        std::string detail;
        for (size_t i = 0; i < model->primitives.size(); ++i) {
            const Primitive& p = model->primitives[i];
            uint32_t prev = p.indexCount;
            for (size_t k = 0; k < p.lods.size(); ++k) {
                const LodLevel& l = p.lods[k];
                const bool good = l.indices && l.indexCount > 0 && l.indexCount % 3 == 0 && l.indexCount < prev;
                if (!good) {
                    ok = false;
                    detail = "primitive " + std::to_string(i) + " level " + std::to_string(k + 1);
                }
                prev = l.indexCount;
            }
            if (p.lods.size() > switches.size()) ok = false;
        }
        check(ok, "lod on: each level is a shorter, multiple-of-3 index list", detail);

        // Turning LOD off again clears the levels.
        buildModelLods(*device, *model, {});
        bool cleared = model->lodSwitch.empty();
        for (const Primitive& p : model->primitives) cleared = cleared && p.lods.empty();
        check(cleared, "lod off again: levels are cleared");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lod_gpu_test: %s\n", e.what());
        return 2;
    }
    std::printf("lod_gpu_test: %s\n", g_fail == 0 ? "all passed" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
