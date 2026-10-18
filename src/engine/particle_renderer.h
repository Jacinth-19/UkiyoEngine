// GPU draw for particle effects: one instanced, camera-facing quad per live particle.
// The CPU simulates particles (systems/vfx.h). This class uploads their instance list to a uniform
// buffer and draws it in a single instanced call.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rhi/rhi.h"
#include "systems/vfx.h"

namespace ukiyo {

enum class ParticleBlend { Alpha, Additive };

class ParticleRenderer {
public:
    // Up to this many particles are drawn per call. The instance block is 32 KB.
    static constexpr uint32_t kMaxInstances = 1024;

    // colorFormat: format of the target the pass draws into (Format::Backbuffer for the presentation target).
    // hasDepth: whether that pass has a D32F depth attachment. With one, particles depth-test but do not write depth.
    ParticleRenderer(rhi::Device& dev, rhi::Format colorFormat, bool hasDepth,
                     ParticleBlend blend = ParticleBlend::Alpha);

    // Draws the instances inside an open pass. `frameSet` is the engine's Frame set (camera, view, proj).
    // Returns the number drawn; anything past kMaxInstances is dropped. Call at most once per pass,
    // because the instance block is uploaded once per call.
    uint32_t draw(rhi::Device& dev, rhi::DescriptorSet& frameSet,
                  const std::vector<ukiyo::ParticleInstance>& instances);

private:
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> instanceBlock_;
    std::unique_ptr<rhi::DescriptorSet> particleSet_;
};

} // namespace ukiyo
