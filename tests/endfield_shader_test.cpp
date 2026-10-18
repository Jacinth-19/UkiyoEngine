// Checks the Endfield-style shaders and their materials without a GPU:
//  - each .ukshader loads and its fragment compiles to SPIR-V;
//  - property slots are 0..n-1, names are unique, and the packed block has 4 floats per slot;
//  - texture bindings are unique and fit under GraphMaterial::kTextureSlots (binding 15 is the UBO);
//  - each .ukmat loads, names the right shader, and its values reach the packed block.
// Usage: endfield_shader_test   (reads UKIYO_ROOT/assets/...)
#include <cstdio>
#include <set>
#include <string>
#include <utility>

#include "engine/graph_material.h"
#include "engine/material_asset.h"
#include "engine/shader_compiler.h"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    ok ? ++g_pass : ++g_fail;
}

const std::string kRoot = UKIYO_ROOT;

} // namespace

int main() {
    // Shaders
    struct Case { const char* file; int textures; };
    const Case shaders[] = {{"EndfieldHybrid.ukshader", 8}, {"NahidaHybrid.ukshader", 8}, {"NahidaFace.ukshader", 8}, {"EndfieldDebug.ukshader", 1}};
    for (const Case& c : shaders) {
        engine::ShaderAsset s;
        std::string err;
        const std::string path = kRoot + "/assets/Shaders/" + c.file;
        bool loaded = engine::loadShaderAsset(path, s, err);
        check(loaded, std::string(c.file) + " loads" + (loaded ? "" : " (" + err + ")"));
        if (!loaded) continue;

        engine::SpirvResult spv = engine::compileGlslToSpirv(s.fragment, engine::ShaderStage::Fragment);
        check(spv.ok, std::string(c.file) + " fragment compiles to SPIR-V" + (spv.ok ? "" : ":\n" + spv.log));

        std::set<std::string> names;
        std::set<int> slots;
        bool slotsOk = true;
        for (const auto& p : s.properties) {
            names.insert(p.name);
            slots.insert(p.slot);
            if (p.kind != "float" && p.kind != "vec2" && p.kind != "vec3") slotsOk = false;
        }
        bool contiguous = true;
        for (int i = 0; i < static_cast<int>(s.properties.size()); ++i) contiguous = contiguous && slots.count(i) == 1;
        check(names.size() == s.properties.size(), std::string(c.file) + " property names are unique");
        check(slotsOk && contiguous, std::string(c.file) + " property slots are 0..n-1 with valid kinds");
        check(s.slotCount() == static_cast<int>(s.properties.size()), std::string(c.file) + " slotCount equals property count");
        check(s.materialBinding == 15, std::string(c.file) + " material block is at binding 15");
        if (s.outline) {
            check(s.properties.size() >= 3 && s.properties[0].name == "outlineColor"
                      && s.properties[1].name == "outlineWidth" && s.properties[2].name == "outlineAlpha",
                  std::string(c.file) + " outline slots are outlineColor, outlineWidth, outlineAlpha at 0, 1, 2");
        }

        std::set<int> bindings;
        bool bindingsOk = true;
        for (const auto& t : s.textures) {
            bindings.insert(t.binding);
            if (t.binding < 0 || t.binding >= ukiyo::GraphMaterial::kTextureSlots) bindingsOk = false;
        }
        check(bindings.size() == s.textures.size() && bindingsOk,
              std::string(c.file) + " texture bindings are unique and below kTextureSlots");
        check(static_cast<int>(s.textures.size()) == c.textures,
              std::string(c.file) + " has " + std::to_string(c.textures) + " texture slots");

        // Packing with the shader's own defaults: 4 floats per slot, default values in place.
        engine::MaterialAsset none;
        std::vector<float> packed = engine::packMaterialParameters(s, none);
        check(packed.size() == static_cast<size_t>(s.slotCount()) * 4,
              std::string(c.file) + " packed block has 4 floats per slot");
    }

    // Materials
    engine::ShaderAsset hybrid;
    std::string err;
    bool hybridLoaded = engine::loadShaderAsset(kRoot + "/assets/Shaders/EndfieldHybrid.ukshader", hybrid, err);
    check(hybridLoaded, "EndfieldHybrid shader loads for the material check");
    if (hybridLoaded) {
        engine::MaterialAsset m;
        bool ok = engine::loadMaterialAsset(kRoot + "/assets/Materials/EndfieldHybrid.ukmat", m, err);
        check(ok, "EndfieldHybrid.ukmat loads" + (ok ? std::string() : " (" + err + ")"));
        if (ok) {
            check(m.shaderPath == "Shaders/EndfieldHybrid.ukshader", "EndfieldHybrid.ukmat names its shader");
            std::vector<float> packed = engine::packMaterialParameters(hybrid, m);
            int normalSlot = -1, matCapSlot = -1;
            for (const auto& p : hybrid.properties) {
                if (p.name == "normalStrength") normalSlot = p.slot;
                if (p.name == "matCapStrength") matCapSlot = p.slot;
            }
            check(normalSlot >= 0 && matCapSlot >= 0, "normalStrength and matCapStrength are exposed");
            if (normalSlot >= 0 && matCapSlot >= 0 && packed.size() > static_cast<size_t>(matCapSlot) * 4) {
                check(packed[static_cast<size_t>(normalSlot) * 4] == 1.0f, "material value normalStrength=1 reaches the block");
                check(packed[static_cast<size_t>(matCapSlot) * 4] == 0.0f, "material value matCapStrength=0 reaches the block");
            }
        }
    }

    // The five-colour outline: only NahidaHybrid sets outlineMap, and its slots 3..7 are the five colours.
    {
        engine::ShaderAsset nh, eh;
        std::string e5;
        bool okN = engine::loadShaderAsset(kRoot + "/assets/Shaders/NahidaHybrid.ukshader", nh, e5);
        bool okE = engine::loadShaderAsset(kRoot + "/assets/Shaders/EndfieldHybrid.ukshader", eh, e5);
        check(okN && nh.outlineMap, "NahidaHybrid sets outlineMap");
        check(okE && !eh.outlineMap, "EndfieldHybrid has the single-colour outline (outlineMap off)");
        bool five = okN && nh.properties.size() >= 8;
        for (int i = 0; five && i < 5; ++i) {
            five = nh.properties[static_cast<size_t>(3 + i)].name == "outlineMapAlpha" + std::to_string(i)
                && nh.properties[static_cast<size_t>(3 + i)].kind == "float"
                && nh.properties[static_cast<size_t>(3 + i)].slot == 3 + i;
        }
        check(five, "NahidaHybrid slots 3..7 are outlineMapAlpha0..4 (float)");
        bool ilm = false, base = false;
        for (const auto& t : nh.textures) {
            ilm = ilm || (t.name == "normalStyle" && t.binding == 1);
            base = base || (t.name == "base" && t.binding == 0);
        }
        check(ilm && base, "NahidaHybrid reads base at binding 0 and the ILM alpha from normalStyle at binding 1");
        // Slots 8..14 hold the outline controls (seven of them), so the ramp rows start at slot 15.
        bool ramps = okN && nh.properties.size() >= 20;
        for (int i = 0; ramps && i < 5; ++i) {
            ramps = nh.properties[static_cast<size_t>(15 + i)].name == "rampRow" + std::to_string(i);
        }
        check(ramps, "NahidaHybrid slots 15..19 are rampRow0..4");
    }

    // The Hybrid has its outline; the Debug shader does not.
    {
        engine::ShaderAsset hy;
        std::string e2;
        bool ok = engine::loadShaderAsset(kRoot + "/assets/Shaders/EndfieldHybrid.ukshader", hy, e2);
        check(ok && hy.outline, "EndfieldHybrid has an outline pass");
        engine::ShaderAsset dbg;
        ok = engine::loadShaderAsset(kRoot + "/assets/Shaders/EndfieldDebug.ukshader", dbg, e2);
        check(ok && !dbg.outline, "EndfieldDebug has no outline pass");
    }

    // Nahida test materials: each loads, names the Endfield shader, and every texture file it names exists.
    {
        // Body and hair take the ILM Hybrid, the face takes Face.shader's port, and plain stays on the Endfield Hybrid.
        const std::pair<const char*, const char*> mats[] = {
            {"NahidaBody", "NahidaHybrid"}, {"NahidaHair", "NahidaHybrid"},
            {"NahidaFace", "NahidaFace"}, {"NahidaPlain", "EndfieldHybrid"}};
        for (const auto& [name, shaderName] : mats) {
            engine::ShaderAsset sh;
            std::string e6;
            if (!engine::loadShaderAsset(kRoot + "/assets/Shaders/" + shaderName + ".ukshader", sh, e6)) {
                check(false, std::string(shaderName) + " shader loads (" + e6 + ")");
                continue;
            }
            engine::MaterialAsset m;
            std::string e4;
            const std::string file = kRoot + "/assets/personal/nahida/endfield/" + name + ".ukmat";
            std::FILE* probe = std::fopen(file.c_str(), "rb");
            if (!probe) {
                std::printf("SKIP  %s.ukmat not present (personal assets are not in the repo)\n", name);
                continue;
            }
            std::fclose(probe);
            bool ok = engine::loadMaterialAsset(file, m, e4);
            check(ok && m.shaderPath == std::string("Shaders/") + shaderName + ".ukshader",
                  std::string(name) + ".ukmat loads and names " + shaderName);
            if (!ok) continue;
            bool filesOk = true;
            for (const auto& [binding, path] : engine::resolveTexturePaths(sh, m, kRoot + "/assets")) {
                (void)binding;
                std::FILE* f = std::fopen(path.c_str(), "rb");
                filesOk = filesOk && f != nullptr;
                if (f) std::fclose(f);
            }
            check(filesOk, std::string(name) + " texture files all exist");
        }
    }

    std::printf("\n%d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
