// GPU side of the HUD: one vertex buffer, one atlas, one draw.
//
// The canvas (engine/hud.h) produces pixels; this converts them to the engine's single vertex layout, uploads the glyph
// atlas when new glyphs appear, and issues one non-indexed draw. Because text and solid fills share the atlas (its
// texel (0,0) is opaque white), a whole HUD — panels, bars and labels — is a single draw call.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/hud.h"
#include "engine/text_atlas.h"
#include "rhi/rhi.h"

namespace ukiyo {

class HudRenderer {
public:
    // colorFormat is the target's format: Format::Backbuffer for the presentation target.
    HudRenderer(rhi::Device& dev, rhi::Format colorFormat);

    // Creates the atlas texture on first use, then uploads any new glyphs. Call once per frame before draw().
    void syncAtlas(rhi::Device& dev, FontAtlas& font);

    // Records the draw into an open pass whose viewport covers the whole target. Vertices are in pixels.
    // Returns the number of vertices drawn (0 when the canvas is empty or no atlas is loaded).
    uint32_t draw(rhi::Device& dev, const std::vector<HudVertex>& vertices, uint32_t screenW, uint32_t screenH);

    // Vertices the buffer can hold before it is reallocated.
    uint32_t capacity() const { return capacity_; }

private:
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> vertices_;
    std::unique_ptr<rhi::DescriptorSet> set_;
    std::unique_ptr<rhi::Texture> atlas_;
    uint64_t atlasRevision_ = 0;
    bool atlasCreated_ = false;
    uint32_t capacity_ = 0;
    rhi::Format colorFormat_;
};

} // namespace ukiyo
