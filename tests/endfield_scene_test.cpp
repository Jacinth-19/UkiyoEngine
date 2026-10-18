// Headless scene check: the Endfield-style look on the Nahida model (personal assets, not in the repo).
// Loads the GLB, gives each primitive the look named for its material in material_map.json, and renders
// through the engine Renderer. Then renders again with every outlineAlpha set to 0, to check that the
// inverted-hull outline pass changes the frame. Writes the outline-on frame as a PNG.
// Usage: endfield_scene_test <assetsDir> <outDir>   (SKIP without Vulkan or without the personal assets)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/camera.h"
#include "engine/graph_material.h"
#include "engine/material_asset.h"
#include "engine/model.h"
#include "engine/renderer.h"
#include "rhi/rhi.h"
#include "stb_image_write.h"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    ok ? ++g_pass : ++g_fail;
}

bool exists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: endfield_scene_test <assetsDir> <outDir>\n");
        return 2;
    }
    const std::string assets = argv[1];
    const std::string outDir = argv[2];
    const std::string personal = assets + "/personal/nahida";
    const std::string glb = personal + "/nahida.glb";
    const std::string mapFile = personal + "/endfield/material_map.json";
    if (!exists(glb) || !exists(mapFile)) {
        std::printf("SKIP  Nahida personal assets not present (%s)\n", glb.c_str());
        return 0;
    }

    try {
        std::string err;
        // Shaders by path. A map keeps the references stable for the GraphMaterials built from them.
        std::map<std::string, engine::ShaderAsset> shaders;
        auto shaderFor = [&](const std::string& shaderPath) -> const engine::ShaderAsset& {
            auto it = shaders.find(shaderPath);
            if (it == shaders.end()) {
                engine::ShaderAsset s;
                if (!engine::loadShaderAsset(assets + "/" + shaderPath, s, err))
                    throw std::runtime_error("shader " + shaderPath + ": " + err);
                it = shaders.emplace(shaderPath, std::move(s)).first;
            }
            return it->second;
        };

        std::map<std::string, std::string> lookFor;   // GLB material name -> look name
        {
            std::ifstream in(mapFile);
            nlohmann::json j = nlohmann::json::parse(in);
            for (auto it = j.begin(); it != j.end(); ++it) lookFor[it.key()] = it.value().get<std::string>();
        }

        rhi::DeviceConfig cfg;
        cfg.width = 640;
        cfg.height = 640;
        cfg.headless = true;
        cfg.validation = true;
        auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
        rhi::TextureDesc td;
        const uint8_t white[4] = {255, 255, 255, 255};
        auto fallback = device->createTexture(td, white);

        // One GraphMaterial per look, each on the shader its .ukmat names. Keep the packed block so the
        // outline can be switched off later.
        std::map<std::string, std::unique_ptr<ukiyo::GraphMaterial>> looks;
        std::map<std::string, std::vector<float>> blocks;
        for (const std::string& look : {std::string("NahidaBody"), std::string("NahidaHair"),
                                         std::string("NahidaFace"), std::string("NahidaPlain")}) {
            engine::MaterialAsset m;
            const std::string file = personal + "/endfield/" + look + ".ukmat";
            if (!engine::loadMaterialAsset(file, m, err)) throw std::runtime_error(look + ": " + err);
            const engine::ShaderAsset& shader = shaderFor(m.shaderPath);
            auto gm = std::make_unique<ukiyo::GraphMaterial>(*device, shader, *fallback, rhi::Format::RGBA16F);
            std::vector<float> block = engine::packMaterialParameters(shader, m);
            gm->setParameters(block);
            if (!gm->setTextureFiles(*device, engine::resolveTexturePaths(shader, m, assets), err))
                throw std::runtime_error(look + " textures: " + err);
            blocks[look] = block;
            looks[look] = std::move(gm);
        }
        check(looks.size() == 4, "four Nahida looks build (body and hair on NahidaHybrid, face on NahidaFace, plain on EndfieldHybrid)");
        check(shaderFor("Shaders/NahidaHybrid.ukshader").outlineMap, "body and hair use the five-type outline pipeline");

        auto model = ukiyo::loadModel(*device, glb);
        if (!model) throw std::runtime_error("cannot load " + glb);
        ukiyo::Renderer renderer(*device);
        size_t bound = 0;
        for (ukiyo::Primitive& prim : model->primitives) {
            const std::string matName = prim.material ? prim.material->name : std::string();
            auto it = lookFor.find(matName);
            if (it == lookFor.end()) continue;
            renderer.setGraphMaterial(&prim, looks.at(it->second).get());
            ++bound;
        }
        check(bound == model->primitives.size(),
              "every primitive has a look (" + std::to_string(bound) + " of " + std::to_string(model->primitives.size()) + ")");

        ukiyo::OrbitCamera cam;
        ukiyo::RenderSettings settings;
        ukiyo::updateModel(*model, 0.0f, cam.eye());
        auto renderOnce = [&](std::vector<uint8_t>& out) {
            if (!device->beginFrame()) throw std::runtime_error("the device skipped the frame");
            std::vector<ukiyo::Model*> models{model.get()};
            renderer.render(cam, models, 0.0f, 0, settings);
            device->endFrame(true);
            out = device->takeCapture();
            device->waitIdle();
        };
        const int w = int(device->backbufferWidth());
        const int h = int(device->backbufferHeight());

        std::vector<uint8_t> withLine, noLine;
        renderOnce(withLine);
        check(withLine.size() >= size_t(w) * size_t(h) * 4, "the outline-on frame was captured");

        // Outline off: outlineAlpha is slot 2 (x), at index 8 of the packed block.
        for (auto& [look, block] : blocks) {
            block[8] = 0.0f;
            looks.at(look)->setParameters(block);
        }
        renderOnce(noLine);

        size_t changed = 0;
        double luma = 0;
        for (size_t i = 0; i + 3 < withLine.size() && i + 3 < noLine.size(); i += 4) {
            int d = 0;
            for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(withLine[i + k]) - int(noLine[i + k])));
            if (d > 4) ++changed;
            luma += 0.2126 * withLine[i] + 0.7152 * withLine[i + 1] + 0.0722 * withLine[i + 2];
        }
        luma /= double(w) * double(h);
        check(luma > 5.0, "frame is not blank (mean luma " + std::to_string(luma) + ")");
        check(changed > 200, "the outline pass changes the frame (" + std::to_string(changed) + " pixels)");

        // SplitScreen line. A horizontal split through the centre: the NPR side (red tint) and the PBR side
        // (blue tint) take opposite colour balances, and a green line sits on the split. The outline is still off.
        const ukiyo::RenderSettings base = settings;
        ukiyo::RenderSettings split = settings;
        split.splitDir = glm::vec2(1.0f, 0.0f);   // the split runs along x: side = y offset from the centre
        split.splitOffsetPx = 0.0f;
        split.splitLineWidthPx = 3.0f;
        split.splitLineColor = glm::vec3(0.0f, 1.0f, 0.0f);
        split.splitNprTint = glm::vec3(1.0f, 0.0f, 0.0f);
        split.splitPbrTint = glm::vec3(0.0f, 0.0f, 1.0f);
        settings = split;
        std::vector<uint8_t> splitFrame;
        renderOnce(splitFrame);
        settings = base;
        {
            double topBal = 0.0, botBal = 0.0;
            size_t topN = 0, botN = 0, greenLine = 0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4;
                    const int r = splitFrame[i], g = splitFrame[i + 1], b = splitFrame[i + 2];
                    if (std::abs(y - h / 2) <= 1 && g > r + 40 && g > b + 40) ++greenLine;
                    if (std::abs(y - h / 2) <= 4) continue;   // keep the line out of the balance
                    const double bal = double(r) - double(b);
                    if (y < h / 2) { topBal += bal; ++topN; } else { botBal += bal; ++botN; }
                }
            }
            topBal /= double(std::max<size_t>(topN, 1));
            botBal /= double(std::max<size_t>(botN, 1));
            check((topBal > 0.0) != (botBal > 0.0) && std::abs(topBal - botBal) > 8.0,
                  "the NPR and PBR sides take opposite tints (R-B top " + std::to_string(topBal) + ", bottom " + std::to_string(botBal) + ")");
            // Screen-up is the positive side for dir (1,0), so the NPR (red) side is the top half.
            check(topBal > 0.0 && botBal < 0.0, "the NPR side is the upper half for dir (1,0)");
            check(greenLine > 100, "the split line is drawn on the split (" + std::to_string(greenLine) + " green pixels)");
        }
        size_t splitChanged = 0;
        for (size_t i = 0; i + 3 < splitFrame.size() && i + 3 < noLine.size(); i += 4) {
            if (splitFrame[i] != noLine[i] || splitFrame[i + 1] != noLine[i + 1] || splitFrame[i + 2] != noLine[i + 2]) ++splitChanged;
        }
        check(splitChanged > 1000, "the split changes the frame (" + std::to_string(splitChanged) + " pixels)");

        // Post grade (Kaze's Genshin post, ported): glow tint, bloom threshold modes, bloom scale, the Genshin
        // rational tonemap, log-space contrast and linear-light saturation. Each new option is off by default.
        auto diffPixels = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            size_t n = 0;
            for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ++n;
            return n;
        };
        auto meanOf = [](const std::vector<uint8_t>& a, int channel) {   // channel 0 R, 1 G, 2 B, 3 R-G, 4 luma
            double sum = 0.0;
            size_t n = 0;
            for (size_t i = 0; i + 3 < a.size(); i += 4, ++n) {
                const double r = a[i], g = a[i + 1], b = a[i + 2];
                if (channel == 0) sum += r;
                else if (channel == 1) sum += g;
                else if (channel == 2) sum += b;
                else if (channel == 3) sum += r - g;
                else sum += (r + g + b) / 3.0;
            }
            return n ? sum / double(n) : 0.0;
        };
        auto frameWith = [&](const ukiyo::RenderSettings& st, std::vector<uint8_t>& out) {
            settings = st;
            renderOnce(out);
            settings = base;
        };

        // The default grade is deterministic: a re-render matches the outline-off frame. (The default bloom
        // changed when bloom moved to Kaze's design, so this checks repeatability, not the old look.)
        std::vector<uint8_t> defaultAgain;
        frameWith(base, defaultAgain);
        check(diffPixels(noLine, defaultAgain) == 0, "the default grade is deterministic across renders");

        // Glow settings: a low threshold and strong bloom so the glow is visible in the frame.
        ukiyo::RenderSettings glow = base;
        glow.bloomThreshold = 0.5f;
        glow.bloomStrength = 0.5f;
        std::vector<uint8_t> glowA, glowTint, glowColor, glowBright, glowSmall, glowAgain;
        frameWith(glow, glowA);

        ukiyo::RenderSettings tinted = glow;
        tinted.bloomTint = glm::vec3(1.0f, 0.2f, 0.2f);
        frameWith(tinted, glowTint);
        check(diffPixels(glowA, glowTint) > 0 && meanOf(glowTint, 3) > meanOf(glowA, 3),
              "the glow tint shifts the bloom towards red (R-G " + std::to_string(meanOf(glowA, 3)) + " -> " + std::to_string(meanOf(glowTint, 3)) + ")");

        ukiyo::RenderSettings colorMode = glow;
        colorMode.bloomThresholdMode = 1;
        frameWith(colorMode, glowColor);
        ukiyo::RenderSettings brightMode = glow;
        brightMode.bloomThresholdMode = 2;
        frameWith(brightMode, glowBright);
        check(diffPixels(glowA, glowColor) > 0, "the Color threshold differs from the luminance knee");
        check(diffPixels(glowA, glowBright) > 0 && diffPixels(glowColor, glowBright) > 0, "the Brightness threshold differs from the other two");

        // Bloom scale rebuilds the bloom targets. A smaller chain changes the glow, and going back to 0.5 reproduces the frame.
        ukiyo::RenderSettings smallBloom = glow;
        smallBloom.bloomScale = 0.25f;
        frameWith(smallBloom, glowSmall);
        check(diffPixels(glowA, glowSmall) > 0, "a smaller bloom chain changes the glow");
        frameWith(glow, glowAgain);
        check(diffPixels(glowA, glowAgain) == 0, "returning to the bloom scale reproduces the first frame");

        // Kaze's per-level weights and blur radius. Weights on only the finest level and only the coarsest differ;
        // a zero radius (no blur) differs from the default.
        std::vector<uint8_t> weightFine, weightCoarse, noBlur, preset;
        ukiyo::RenderSettings fineOnly = glow;
        fineOnly.bloomWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        frameWith(fineOnly, weightFine);
        ukiyo::RenderSettings coarseOnly = glow;
        coarseOnly.bloomWeights = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        frameWith(coarseOnly, weightCoarse);
        check(diffPixels(weightFine, weightCoarse) > 0 && diffPixels(glowA, weightFine) > 0,
              "the per-level bloom weights change the glow (finest vs coarsest)");
        ukiyo::RenderSettings sharp = glow;
        sharp.bloomBlurRadius = 0.0f;
        frameWith(sharp, noBlur);
        check(diffPixels(glowA, noBlur) > 0, "the bloom blur radius changes the glow");

        // The Kaze Unity match preset renders a plausible frame.
        ukiyo::RenderSettings kaze = base;
        ukiyo::applyKazeUnityPost(kaze);
        frameWith(kaze, preset);
        check(meanOf(preset, 4) > 10.0 && diffPixels(noLine, preset) > 0,
              "the Kaze Unity match preset renders a plausible frame (luma " + std::to_string(meanOf(preset, 4)) + ")");

        // Tonemap operator 10, the Genshin rational curve.
        std::vector<uint8_t> genshin, logC, linSat;
        ukiyo::RenderSettings genshinSet = base;
        genshinSet.tonemap = 10;
        frameWith(genshinSet, genshin);
        check(meanOf(genshin, 4) > 10.0 && diffPixels(noLine, genshin) > 0,
              "the Genshin rational tonemapper renders a plausible frame that differs from Custom Anime");

        ukiyo::RenderSettings logSet = base;
        logSet.contrastLogC = true;
        frameWith(logSet, logC);
        check(diffPixels(noLine, logC) > 0, "log-space contrast changes the grade");

        ukiyo::RenderSettings linSet = base;
        linSet.saturationLinear = true;
        frameWith(linSet, linSat);
        check(diffPixels(noLine, linSat) > 0, "linear-light saturation changes the grade");

        const std::string out = outDir + "/nahida_endfield_vk.png";
        check(stbi_write_png(out.c_str(), w, h, 4, withLine.data(), w * 4) != 0, "wrote " + out);
    } catch (const std::exception& ex) {
        const std::string msg = ex.what();
        if (msg.find("Vulkan") != std::string::npos || msg.find("device") != std::string::npos) {
            std::printf("SKIP  no Vulkan device: %s\n", msg.c_str());
            return 0;
        }
        std::printf("FAIL  %s\n", msg.c_str());
        return 1;
    }

    std::printf("\n%d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
