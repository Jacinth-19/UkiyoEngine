#include "engine/vfs.h"
#include "engine/graph_material.h"

#include <algorithm>
#include <stdexcept>

#include "engine/shader_compiler.h"

#include "stb_image.h"

namespace ukiyo {

GraphMaterial::GraphMaterial(rhi::Device& dev, const engine::ShaderAsset& shader, rhi::Texture& fallbackTex,
                             rhi::Format colorFormat)
    // graph_outline.vert reads slots 0..14 of the material block, so a shader with an outline pass
    // allocates all of them even when its graph exposes only the three slots the loader requires. The
    // rest stay zero, which is the neutral value for every width and offset control.
    : slots_(std::max(1, shader.outline ? std::max(kOutlineSlots, shader.slotCount()) : shader.slotCount())),
      binding_(shader.materialBinding)
{
    const engine::SpirvResult spv = engine::compileGlslToSpirv(shader.fragment, engine::ShaderStage::Fragment);
    if (!spv.ok)
        throw std::runtime_error(shader.path + ": fragment shader does not compile:\n" + spv.log);

    rhi::PipelineDesc d;
    d.vertexShader = "graph";
    d.fragmentSpirv = spv.words;     // Vulkan
    d.fragmentGlsl = shader.fragment; // GL compiles the same source text
    d.sets = {rhi::SetType::Frame, rhi::SetType::ShaderMaterial, rhi::SetType::Skin};
    d.vertexInput = true;
    d.cull = rhi::Cull::Back;
    d.depthTest = true;
    d.depthWrite = true;
    d.blend = shader.transparent ? rhi::Blend::Alpha : rhi::Blend::None;
    transparent_ = shader.transparent;
    d.colorFormats = {colorFormat};
    d.hasDepth = true;
    pipeline_ = dev.createPipeline(d);

    if (shader.outline) {
        // graph_outline(_map).vert/.frag: inverted hull, front faces culled. Reads outline slots 0..2 (map: 0..7).
        rhi::PipelineDesc o;
        o.vertexShader = shader.outlineMap ? "graph_outline_map" : "graph_outline";
        o.fragmentShader = shader.outlineMap ? "graph_outline_map" : "graph_outline";
        o.sets = {rhi::SetType::Frame, rhi::SetType::ShaderMaterial, rhi::SetType::Skin};
        o.vertexInput = true;
        o.cull = rhi::Cull::Front;
        o.depthTest = true;
        o.depthWrite = true;
        o.blend = rhi::Blend::None;
        o.colorFormats = {colorFormat};
        o.hasDepth = true;
        outlinePipeline_ = dev.createPipeline(o);
        hasOutline_ = true;
    }

    ubo_ = dev.createBuffer(rhi::BufferUsage::Uniform, static_cast<size_t>(slots_) * 16);
    set_ = dev.createDescriptorSet(rhi::SetType::ShaderMaterial);
    set_->setBuffer(static_cast<uint32_t>(binding_), ubo_.get());
    fallback_ = &fallbackTex;
    for (uint32_t t = 0; t < kTextureSlots; ++t) {
        set_->setTexture(t, fallback_);
        bound_[t] = fallback_;
    }
}

bool GraphMaterial::setTextureFiles(rhi::Device& dev, const std::map<int, std::string>& files, std::string& error)
{
    bool ok = true;
    std::vector<std::unique_ptr<rhi::Texture>> loaded;
    std::array<rhi::Texture*, kTextureSlots> next{};
    next.fill(fallback_);
    for (const auto& [binding, path] : files) {
        if (binding < 0 || binding >= kTextureSlots) continue;
        int w = 0, h = 0, c = 0;
        std::vector<uint8_t> texBytes;
        stbi_uc* px = vfs::readBytes(path, texBytes) && !texBytes.empty()
            ? stbi_load_from_memory(texBytes.data(), static_cast<int>(texBytes.size()), &w, &h, &c, 4)
            : nullptr;
        if (!px) {
            ok = false;
            if (error.empty()) error = "cannot read texture " + path;
            continue;
        }
        rhi::TextureDesc td;
        td.width = static_cast<uint32_t>(w);
        td.height = static_cast<uint32_t>(h);
        td.wrap = rhi::Wrap::Repeat;
        loaded.push_back(dev.createTexture(td, px));
        stbi_image_free(px);
        next[static_cast<size_t>(binding)] = loaded.back().get();
    }
    // A texture that the previous draw may still read must not be destroyed: wait before rebinding.
    dev.waitIdle();
    for (uint32_t t = 0; t < kTextureSlots; ++t) {
        set_->setTexture(t, next[t]);
        bound_[t] = next[t];
    }
    owned_ = std::move(loaded);
    return ok;
}

rhi::Texture* GraphMaterial::textureAt(int binding) const
{
    if (binding < 0 || binding >= kTextureSlots) return nullptr;
    return bound_[static_cast<size_t>(binding)];
}

void GraphMaterial::setParameters(const std::vector<float>& block)
{
    const size_t bytes = static_cast<size_t>(slots_) * 16;
    std::vector<float> padded(bytes / sizeof(float), 0.0f);
    for (size_t i = 0; i < block.size() && i < padded.size(); ++i) padded[i] = block[i];
    ubo_->upload(padded.data(), bytes);
    // outlineWidth is slot 1 (x), outlineAlpha is slot 2 (x). Both must be above zero.
    outlineOn_ = hasOutline_ && padded.size() > 8 && padded[4] > 0.0f && padded[8] > 0.0f;
}

} // namespace ukiyo
