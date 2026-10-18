// Headless GPU check for the Endfield-style shaders. Binds the 8 packed texture slots of EndfieldHybrid
// through GraphMaterial::setTextureFiles, draws the material on a sphere with renderMaterialSphere, and
// writes PNGs. Checks that the slots bind, that the frames are not blank, and that the debug and wet
// variants differ from the normal look.
// Usage: endfield_gpu_test <assetsDir> <packedDir> <outDir>   (SKIP when no Vulkan device can be made)
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/graph_material.h"
#include "engine/material_asset.h"
#include "engine/material_preview.h"
#include "rhi/rhi.h"
#include "stb_image_write.h"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    ok ? ++g_pass : ++g_fail;
}

// Sets one property in a packed block by name. Values are written from the start of its slot.
bool setProp(const engine::ShaderAsset& s, std::vector<float>& block, const std::string& name,
             std::vector<float> values) {
    for (const auto& p : s.properties) {
        if (p.name != name) continue;
        for (size_t i = 0; i < values.size() && i < 4; ++i) block[static_cast<size_t>(p.slot) * 4 + i] = values[i];
        return true;
    }
    return false;
}

// Mean brightness and the number of distinct colours in a frame (sampled).
void frameStats(const std::vector<uint8_t>& px, double& meanLuma, size_t& distinct) {
    double sum = 0;
    size_t n = 0;
    std::map<uint32_t, int> seen;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        sum += 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
        ++n;
        if (n % 7 == 0) seen[(uint32_t(px[i]) << 16) | (uint32_t(px[i + 1]) << 8) | px[i + 2]] = 1;
    }
    meanLuma = n ? sum / double(n) : 0.0;
    distinct = seen.size();
}

bool savePng(const std::string& path, const std::vector<uint8_t>& px, int w, int h) {
    return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: endfield_gpu_test <assetsDir> <packedDir> <outDir>\n");
        return 2;
    }
    const std::string assets = argv[1];
    const std::string packedDir = argv[2];
    const std::string outDir = argv[3];

    engine::ShaderAsset hybrid, debugShader;
    engine::MaterialAsset hybridMat, debugMat;
    std::string err;
    if (!engine::loadShaderAsset(assets + "/Shaders/EndfieldHybrid.ukshader", hybrid, err) ||
        !engine::loadMaterialAsset(assets + "/Materials/EndfieldHybrid.ukmat", hybridMat, err) ||
        !engine::loadShaderAsset(assets + "/Shaders/EndfieldDebug.ukshader", debugShader, err) ||
        !engine::loadMaterialAsset(assets + "/Materials/EndfieldDebug.ukmat", debugMat, err)) {
        std::fprintf(stderr, "load FAILED: %s\n", err.c_str());
        return 1;
    }

    // The packed texture set (pack_endfield.py): one PNG per shader slot, named by the slot.
    std::map<std::string, std::string> files;
    for (const auto& t : hybrid.textures) files[t.name] = packedDir + "/" + t.name + ".png";
    for (const auto& [name, path] : files) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "missing packed texture %s (run pack_endfield.py first)\n", path.c_str());
            return 1;
        }
        std::fclose(f);
    }

    try {
        rhi::DeviceConfig cfg;
        cfg.width = 256;
        cfg.height = 256;
        cfg.headless = true;
        cfg.validation = true;
        auto device = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);

        rhi::TextureDesc td;
        const uint8_t white[4] = {255, 255, 255, 255};
        auto fallback = device->createTexture(td, white);

        // Hybrid: every slot must bind. Map the texture name to its binding through the shader's list.
        check(hybrid.textures.size() == 8, "EndfieldHybrid declares 8 packed texture slots");
        std::map<int, std::string> byBinding;
        for (const auto& t : hybrid.textures) {
            auto it = files.find(t.name);
            if (it == files.end()) {
                check(false, "no test texture for slot " + t.name);
                continue;
            }
            byBinding[t.binding] = it->second;
        }
        auto materialFor = [&](const engine::ShaderAsset& s, const std::map<int, std::string>& texFiles,
                               std::vector<float> block, std::string& e) {
            auto gm = std::make_unique<ukiyo::GraphMaterial>(*device, s, *fallback, rhi::Format::Backbuffer);
            gm->setParameters(block);
            if (!gm->setTextureFiles(*device, texFiles, e)) return std::unique_ptr<ukiyo::GraphMaterial>();
            return gm;
        };

        std::string e;
        std::vector<float> block = engine::packMaterialParameters(hybrid, hybridMat);
        auto gm = materialFor(hybrid, byBinding, block, e);
        check(gm != nullptr, "EndfieldHybrid material builds with 8 packed textures" + (e.empty() ? "" : " (" + e + ")"));
        if (!gm) throw std::runtime_error("no material");

        bool allBound = true;
        for (const auto& [binding, path] : byBinding) {
            (void)path;
            allBound = allBound && gm->textureAt(binding) != nullptr && gm->textureAt(binding) != fallback.get();
        }
        check(allBound, "all 8 packed Hybrid slots are bound to the packed textures (not the fallback)");

        auto frame = ukiyo::renderMaterialSphere(*device, *gm);
        double luma = 0;
        size_t distinct = 0;
        frameStats(frame, luma, distinct);
        check(luma > 5.0 && distinct > 10, "normal look: frame is not blank (mean luma " + std::to_string(luma) + ")");
        savePng(outDir + "/endfield_hybrid_vk.png", frame, int(device->backbufferWidth()), int(device->backbufferHeight()));

        // Debug view of the base texture (mode 1), and the face SDF shadow (mode 7).
        std::vector<float> dbg = block;
        setProp(hybrid, dbg, "debugMode", {1});
        gm->setParameters(dbg);
        auto dbgFrame = ukiyo::renderMaterialSphere(*device, *gm);
        check(dbgFrame != frame, "debug mode 1 (base) differs from the normal look");
        savePng(outDir + "/endfield_hybrid_debug_base_vk.png", dbgFrame, int(device->backbufferWidth()), int(device->backbufferHeight()));

        setProp(hybrid, dbg, "debugMode", {7});
        gm->setParameters(dbg);
        auto sdfFrame = ukiyo::renderMaterialSphere(*device, *gm);
        frameStats(sdfFrame, luma, distinct);
        check(luma > 0.5, "debug mode 7 (face SDF shadow) draws");

        // Wet look: rain on, glossy and cloth masks on, a fixed preview time so the output is repeatable.
        std::vector<float> wet = block;
        setProp(hybrid, wet, "debugMode", {0});
        setProp(hybrid, wet, "wetness", {1});
        setProp(hybrid, wet, "wetGlossMask", {1});
        setProp(hybrid, wet, "wetClothMask", {1});
        setProp(hybrid, wet, "wetPreviewTime", {0.7f});
        gm->setParameters(wet);
        auto wetFrame = ukiyo::renderMaterialSphere(*device, *gm);
        check(wetFrame != frame, "wet look differs from the dry look");
        frameStats(wetFrame, luma, distinct);
        check(luma > 5.0, "wet look: frame is not blank (mean luma " + std::to_string(luma) + ")");
        savePng(outDir + "/endfield_hybrid_wet_vk.png", wetFrame, int(device->backbufferWidth()), int(device->backbufferHeight()));

        // Debug shader: channel 1 (red) of the base texture.
        check(debugShader.textures.size() == 1 && debugShader.textures[0].binding == 0, "EndfieldDebug declares one texture at binding 0");
        std::map<int, std::string> dbgTex;
        dbgTex[0] = files["base"];   // the packed base texture is the RGB base colour
        std::vector<float> dblock = engine::packMaterialParameters(debugShader, debugMat);
        setProp(debugShader, dblock, "channel", {1});
        auto dm = materialFor(debugShader, dbgTex, dblock, e);
        check(dm != nullptr, "EndfieldDebug material builds" + (e.empty() ? "" : " (" + e + ")"));
        if (dm) {
            auto dframe = ukiyo::renderMaterialSphere(*device, *dm);
            frameStats(dframe, luma, distinct);
            check(luma > 5.0, "EndfieldDebug draws (mean luma " + std::to_string(luma) + ")");
            savePng(outDir + "/endfield_debug_vk.png", dframe, int(device->backbufferWidth()), int(device->backbufferHeight()));
        }
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
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
