// PMX (MikuMikuDance) model import. Converts a PMX 2.0 or 2.1 file into a GLB held in
// memory, so the glTF loader builds the model. Imported: geometry, materials with their
// diffuse textures, the bone hierarchy and skinning weights. Not imported: morphs, rigid
// bodies, joints, IK and append bones (the rest pose is kept). Standard C++ only; no
// third-party code.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ukiyo::pmx {

// A skin may use at most this many joints (the engine's joint palette).
constexpr int kMaxJointsPerSkin = 128;

struct ImportOptions {
    // Metres per PMX unit, applied at the root. MMD models are roughly 12 units per metre
    // for a human figure, so this default gives a model around 1.2 m tall for Nahida.
    float unitScale = 0.08f;
};

struct ImportReport {
    int materials = 0;
    int textures = 0;       // textures that were found and embedded
    int bones = 0;
    int primitives = 0;     // one per skin chunk
    int triangles = 0;
    std::vector<std::string> warnings;
};

// Reads the bytes of a texture. `relPath` is the PMX texture path with '/' separators,
// relative to the model file's folder. Returns false if the file cannot be read.
using TextureReader = std::function<bool(const std::string& relPath, std::vector<uint8_t>& out)>;

// Converts `pmx` to GLB in `glb`. On failure returns false and sets `error`.
bool convertToGlb(const std::vector<uint8_t>& pmx, const TextureReader& readTexture,
                  std::vector<uint8_t>& glb, std::string& error,
                  ImportReport* report = nullptr, const ImportOptions& options = {});

} // namespace ukiyo::pmx
