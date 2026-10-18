#include "engine/material_asset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace engine {

namespace {

bool readFile(const std::string& path, std::string& text, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

bool parseJson(const std::string& path, nlohmann::json& out, std::string& error)
{
    std::string text;
    if (!readFile(path, text, error)) return false;
    try {
        out = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        error = path + ": " + e.what();
        return false;
    }
    return true;
}

int componentCount(const std::string& kind)
{
    if (kind == "float") return 1;
    if (kind == "vec2") return 2;
    return 3;   // vec3
}

} // namespace

int ShaderAsset::slotCount() const
{
    int n = 0;
    for (const ShaderProperty& p : properties) n = std::max(n, p.slot + 1);
    return n;
}

bool loadShaderAsset(const std::string& path, ShaderAsset& out, std::string& error)
{
    nlohmann::json doc;
    if (!parseJson(path, doc, error)) return false;
    if (doc.value("format", std::string()) != "ukiyo-shader-graph") {
        error = path + ": not a Ukiyo shader graph";
        return false;
    }
    if (!doc.contains("compiled") || !doc["compiled"].is_object()) {
        error = path + ": no compiled output (save the graph in the Shader Graph editor)";
        return false;
    }
    const nlohmann::json& c = doc["compiled"];
    if (!c.value("ok", false)) {
        error = path + ": the graph has errors";
        if (c.contains("errors") && !c["errors"].empty()) error += ": " + c["errors"][0].get<std::string>();
        return false;
    }

    out = ShaderAsset{};
    out.path = path;
    out.fragment = c.value("fragment", std::string());
    out.materialBinding = c.value("materialBinding", 15);
    out.transparent = c.value("blend", std::string("none")) == "alpha";
    for (const nlohmann::json& t : c.value("textures", nlohmann::json::array())) {
        ShaderTexture tex;
        tex.name = t.value("name", std::string());
        tex.binding = t.value("binding", 0);
        tex.defaultPath = t.value("defaultPath", std::string());
        out.textures.push_back(tex);
    }
    for (const nlohmann::json& p : c.value("properties", nlohmann::json::array())) {
        ShaderProperty prop;
        prop.name = p.value("name", std::string());
        prop.kind = p.value("kind", std::string("float"));
        prop.slot = p.value("slot", 0);
        for (const nlohmann::json& d : p.value("defaults", nlohmann::json::array())) {
            prop.defaults.push_back(std::stof(d.get<std::string>()));
        }
        prop.defaults.resize(componentCount(prop.kind), 0.0f);
        out.properties.push_back(prop);
    }
    if (out.fragment.empty()) {
        error = path + ": empty fragment shader";
        return false;
    }
    out.outline = c.value("outline", false);
    if (out.outline) {
        const bool ok = out.properties.size() >= 3
            && out.properties[0].name == "outlineColor" && out.properties[0].kind == "vec3" && out.properties[0].slot == 0
            && out.properties[1].name == "outlineWidth" && out.properties[1].kind == "float" && out.properties[1].slot == 1
            && out.properties[2].name == "outlineAlpha" && out.properties[2].kind == "float" && out.properties[2].slot == 2;
        if (!ok) {
            error = path + ": outline needs outlineColor (vec3), outlineWidth and outlineAlpha at slots 0, 1 and 2";
            return false;
        }
    }
    out.outlineMap = c.value("outlineMap", false);
    if (out.outlineMap) {
        bool ok = out.outline && out.properties.size() >= 8;
        for (int i = 0; ok && i < 5; ++i) {
            const ShaderProperty& p = out.properties[static_cast<size_t>(3 + i)];
            ok = p.name == "outlineMapAlpha" + std::to_string(i) && p.kind == "float" && p.slot == 3 + i;
        }
        bool base = false, ilm = false;
        for (const ShaderTexture& t : out.textures) {
            base = base || (t.name == "base" && t.binding == 0);
            ilm = ilm || (t.name == "normalStyle" && t.binding == 1);
        }
        if (!ok || !base || !ilm) {
            error = path + ": outlineMap needs outlineMapAlpha0..4 (float) at slots 3..7, base at binding 0 and normalStyle at binding 1";
            return false;
        }
    }
    return true;
}

bool loadMaterialAsset(const std::string& path, MaterialAsset& out, std::string& error)
{
    nlohmann::json doc;
    if (!parseJson(path, doc, error)) return false;
    if (doc.value("format", std::string()) != "ukiyo-material") {
        error = path + ": not a Ukiyo material";
        return false;
    }
    out = MaterialAsset{};
    out.name = doc.value("name", std::string());
    out.shaderPath = doc.value("shader", std::string());
    if (out.shaderPath.empty()) {
        error = path + ": the material has no shader";
        return false;
    }
    if (doc.contains("textures") && doc["textures"].is_object()) {
        for (auto& [name, value] : doc["textures"].items()) {
            if (value.is_string()) out.textures[name] = value.get<std::string>();
        }
    }
    if (doc.contains("properties") && doc["properties"].is_object()) {
        for (auto& [name, value] : doc["properties"].items()) {
            std::vector<float> v;
            if (value.is_array()) {
                for (const nlohmann::json& x : value) v.push_back(x.get<float>());
            } else {
                v.push_back(value.get<float>());
            }
            out.values[name] = v;
        }
    }
    return true;
}

std::vector<float> packMaterialParameters(const ShaderAsset& shader, const MaterialAsset& material)
{
    std::vector<float> block(static_cast<size_t>(shader.slotCount()) * 4, 0.0f);
    for (const ShaderProperty& p : shader.properties) {
        const int n = componentCount(p.kind);
        const std::vector<float>* src = &p.defaults;
        auto it = material.values.find(p.name);
        if (it != material.values.end()) src = &it->second;
        for (int k = 0; k < n; ++k) {
            const float v = k < static_cast<int>(src->size()) ? (*src)[static_cast<size_t>(k)] : 0.0f;
            block[static_cast<size_t>(p.slot) * 4 + static_cast<size_t>(k)] = v;
        }
    }
    return block;
}

std::map<int, std::string> resolveTexturePaths(const ShaderAsset& shader, const MaterialAsset& material,
                                               const std::string& assetsDir)
{
    std::map<int, std::string> out;
    for (const ShaderTexture& t : shader.textures) {
        auto it = material.textures.find(t.name);
        const std::string& path = (it != material.textures.end() && !it->second.empty()) ? it->second : t.defaultPath;
        // operator/ keeps an absolute right-hand side as it is, so absolute paths need no special case.
        if (!path.empty()) out[t.binding] = (std::filesystem::path(assetsDir) / path).string();
    }
    return out;
}

} // namespace engine
