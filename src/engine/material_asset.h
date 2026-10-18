#pragma once

// Shader and material assets as the engine reads them. A .ukshader carries the compiled GLSL and
// the exposed-property and texture-slot layout written by the editor. A .ukmat points at a shader, gives
// values for its properties, and may give an image path for each texture slot ("textures").

#include <map>
#include <string>
#include <vector>

namespace engine {

struct ShaderProperty {
    std::string name;
    std::string kind;              // "float", "vec2" or "vec3"
    int slot = 0;                  // vec4 slot in the material block
    std::vector<float> defaults;   // from the graph node
};

// A texture slot exposed by a shader graph. It is bound at `binding` in the material set. Its default
// is the texture the Sample Texture 2D node was given. The path is relative to the assets folder.
struct ShaderTexture {
    std::string name;
    int binding = 0;
    std::string defaultPath;
};

struct ShaderAsset {
    std::string path;
    std::string fragment;          // GLSL 4.50 fragment source
    std::vector<ShaderProperty> properties;
    std::vector<ShaderTexture> textures;
    int materialBinding = 15;      // set 1 binding of the material block
    bool transparent = false;      // drawn alpha-blended in the transparent pass
    // Inverted-hull outline pass. Needs slot 0 = outlineColor (vec3), slot 1 = outlineWidth (float, world
    // units) and slot 2 = outlineAlpha (float; 0 turns the outline off). The loader checks these slots.
    bool outline = false;
    // Five-type outline (BodyAndHair): outlineMapAlpha0..4 (float, slots 3..7) pick, per pixel, whether the outline
    // draws, from the alpha of the texture at binding 1 ("normalStyle"). The colour is base * 0.25. Needs outline = true.
    bool outlineMap = false;
    int slotCount() const;         // number of vec4 slots the block needs
};

struct MaterialAsset {
    std::string name;
    std::string shaderPath;        // relative to the assets folder, e.g. "Shaders/Lit.ukshader"
    std::map<std::string, std::vector<float>> values;
    std::map<std::string, std::string> textures;   // texture slot name -> image path (see resolveTexturePaths)
};

// Each loader returns false and sets `error` when the file is missing or malformed.
bool loadShaderAsset(const std::string& path, ShaderAsset& out, std::string& error);
bool loadMaterialAsset(const std::string& path, MaterialAsset& out, std::string& error);

// Packs the material values into vec4 slots in the shader's slot order. A property the material
// does not set uses the shader's default. The result has 4 floats per slot.
std::vector<float> packMaterialParameters(const ShaderAsset& shader, const MaterialAsset& material);

// The image each texture slot of a shader uses, keyed by binding: the material's own path when it sets one,
// otherwise the shader's default. Relative paths are taken from `assetsDir` (the folder that holds Materials/
// and Shaders/); absolute paths are kept as they are. A slot with neither is left out, and the material draws
// the fallback texture there.
std::map<int, std::string> resolveTexturePaths(const ShaderAsset& shader, const MaterialAsset& material,
                                               const std::string& assetsDir);

} // namespace engine
