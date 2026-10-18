#pragma once

// A generated (Shader Graph) material on the GPU: the pipeline built from a compiled
// .ukshader and the descriptor set that holds the material's parameter block.
// Vulkan only; the GL backend rejects generated pipelines.

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "engine/material_asset.h"
#include "rhi/rhi.h"

namespace ukiyo {

class GraphMaterial {
public:
    // Compiles the shader's fragment stage to SPIR-V and builds the pipeline.
    // Pairs with graph.vert (vertex stage) and the sets Frame, ShaderMaterial, Skin.
    // Throws std::runtime_error when compilation or pipeline creation fails.
    // colorFormat is the colour target the material draws into: rhi::Format::RGBA16F for the HDR
    // frame, rhi::Format::Backbuffer for previews drawn straight to the presentation target.
    GraphMaterial(rhi::Device& dev, const engine::ShaderAsset& shader, rhi::Texture& fallbackTex,
                  rhi::Format colorFormat = rhi::Format::RGBA16F);

    // Uploads packed parameters (engine::packMaterialParameters). Size must be slotCount * 16 bytes.
    void setParameters(const std::vector<float>& block);

    // Binds the textures this material uses. `files` maps a binding (0 to 7) to an image path; decoded to
    // RGBA8 and owned by this material. Bindings not listed use the fallback texture. Returns false with
    // `error` when a file cannot be read; the other bindings are still bound.
    bool setTextureFiles(rhi::Device& dev, const std::map<int, std::string>& files, std::string& error);
    // The texture bound at a binding, for tests and previews.
    rhi::Texture* textureAt(int binding) const;
    static constexpr int kTextureSlots = 8;    // bindings 0..7; binding 15 is the material UBO

    rhi::Pipeline* pipeline() const { return pipeline_.get(); }
    rhi::DescriptorSet* set() const { return set_.get(); }
    int slotCount() const { return slots_; }
    bool transparent() const { return transparent_; }
    // Inverted-hull outline pass (only when the shader has one). Uses the same material set.
    rhi::Pipeline* outlinePipeline() const { return outlinePipeline_.get(); }
    // True when the packed block has the outline on: outlineWidth > 0 and outlineAlpha > 0.
    bool outlineOn() const { return outlineOn_; }

private:
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Pipeline> outlinePipeline_;
    bool hasOutline_ = false;
    bool outlineOn_ = false;
    std::unique_ptr<rhi::Buffer> ubo_;
    std::unique_ptr<rhi::DescriptorSet> set_;
    rhi::Texture* fallback_ = nullptr;
    std::vector<std::unique_ptr<rhi::Texture>> owned_;   // decoded images, bound in set_
    std::array<rhi::Texture*, kTextureSlots> bound_{};
    int slots_ = 1;
    int binding_ = 15;
    bool transparent_ = false;
};

} // namespace ukiyo
