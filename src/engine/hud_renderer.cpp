#include "engine/hud_renderer.h"

#include "engine/gpu_types.h"

#include <algorithm>
#include <cstring>

namespace ukiyo {

HudRenderer::HudRenderer(rhi::Device& dev, rhi::Format colorFormat) : colorFormat_(colorFormat) {
    rhi::PipelineDesc d;
    d.vertexShader = "hud";
    d.fragmentShader = "hud";
    d.sets = {rhi::SetType::Hud};
    d.vertexInput = true;         // the shared Vertex layout; only position, uv and colour are read
    d.cull = rhi::Cull::None;     // quads are emitted in screen space, winding is not meaningful
    d.depthTest = false;
    d.depthWrite = false;
    d.blend = rhi::Blend::Alpha;  // straight alpha over the finished frame
    d.colorFormats = {colorFormat};
    d.hasDepth = false;
    pipeline_ = dev.createPipeline(d);

    capacity_ = 16384;            // ~5460 glyphs at 20 px before the buffer grows
    vertices_ = dev.createBuffer(rhi::BufferUsage::Vertex, static_cast<size_t>(capacity_) * sizeof(rhi::Vertex));
    set_ = dev.createDescriptorSet(rhi::SetType::Hud);
}

void HudRenderer::syncAtlas(rhi::Device& dev, FontAtlas& font) {
    if (!font.loaded()) return;
    msdfRange_ = font.msdfRange();
    if (!atlasCreated_) {
        rhi::TextureDesc td;
        td.width = static_cast<uint32_t>(font.width());
        td.height = static_cast<uint32_t>(font.height());
        td.format = rhi::Format::RGBA8;
        td.mipmaps = false;
        td.filter = rhi::Filter::Linear;
        td.wrap = rhi::Wrap::Clamp;
        atlas_ = dev.createTexture(td, font.pixels().data());
        set_->setTexture(0, atlas_.get());
        atlasRevision_ = font.revision();
        atlasCreated_ = true;
        font.takeDirty();   // the whole page was uploaded; nothing is pending
        return;
    }
    if (font.revision() == atlasRevision_) return;
    const FontAtlas::DirtyRect dirty = font.takeDirty();
    if (dirty.w <= 0 || dirty.h <= 0) {
        atlasRevision_ = font.revision();
        return;
    }
    // Copy the rectangle out of the page, row by row: the atlas is square and the rect is not, so a single memcpy would
    // take neighbouring rows with it.
    std::vector<uint8_t> rect(static_cast<size_t>(dirty.w) * dirty.h * 4);
    const std::vector<uint8_t>& page = font.pixels();
    const size_t pageStride = static_cast<size_t>(font.width()) * 4;
    for (int row = 0; row < dirty.h; ++row) {
        std::memcpy(rect.data() + static_cast<size_t>(row) * dirty.w * 4,
                    page.data() + static_cast<size_t>(dirty.y + row) * pageStride + static_cast<size_t>(dirty.x) * 4,
                    static_cast<size_t>(dirty.w) * 4);
    }
    atlas_->update(static_cast<uint32_t>(dirty.x), static_cast<uint32_t>(dirty.y), static_cast<uint32_t>(dirty.w),
                   static_cast<uint32_t>(dirty.h), rect.data());
    atlasRevision_ = font.revision();
}

uint32_t HudRenderer::draw(rhi::Device& dev, const std::vector<HudVertex>& vertices, uint32_t screenW, uint32_t screenH) {
    if (vertices.empty() || !atlasCreated_) return 0;
    const uint32_t n = static_cast<uint32_t>(vertices.size());
    if (n > capacity_) {
        capacity_ = n + n / 2;
        vertices_ = dev.createBuffer(rhi::BufferUsage::Vertex, static_cast<size_t>(capacity_) * sizeof(rhi::Vertex));
    }
    // Convert to the shared vertex layout. The unused channels stay zero/white, which the shader never reads.
    std::vector<rhi::Vertex> gpu(n);
    for (uint32_t i = 0; i < n; ++i) {
        rhi::Vertex& v = gpu[i];
        v.pos = glm::vec3(vertices[i].pos, 0.0f);
        v.uv = vertices[i].uv;
        v.color = vertices[i].color;
    }
    vertices_->upload(gpu.data(), static_cast<size_t>(n) * sizeof(rhi::Vertex));

    dev.bindPipeline(pipeline_.get());
    dev.bindDescriptorSet(0, set_.get());
    dev.bindVertexBuffer(vertices_.get());
    PushConstants pc{};
    pc.model = glm::mat4(1.0f);
    // p0.z is the MSDF field's half-width in texels, which the fragment shader needs to turn the distance into an edge
    // one pixel wide; 0 selects the coverage path.
    pc.p0 = glm::vec4(static_cast<float>(screenW), static_cast<float>(screenH), msdfRange_, 0.0f);
    dev.pushConstants(&pc, sizeof(pc));
    dev.drawInstanced(n, 1);   // non-indexed: the triangle list is already expanded
    return n;
}

} // namespace ukiyo
