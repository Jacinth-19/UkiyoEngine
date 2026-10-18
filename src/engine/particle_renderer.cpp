#include "engine/particle_renderer.h"

#include <algorithm>
#include <cstring>

namespace ukiyo {

namespace {

// Matches ParticleInst in shaders/particle.vert: two vec4s per instance.
struct GpuParticle {
    float posSize[4];
    float color[4];
};
static_assert(sizeof(GpuParticle) == 32, "particle instance layout is shared with shaders/particle.vert");

} // namespace

ParticleRenderer::ParticleRenderer(rhi::Device& dev, rhi::Format colorFormat, bool hasDepth, ParticleBlend blend)
{
    rhi::PipelineDesc d;
    d.vertexShader = "particle";
    d.fragmentShader = "particle";
    d.sets = {rhi::SetType::Frame, rhi::SetType::Particles};
    d.vertexInput = false;          // procedural quads, no vertex buffer
    d.cull = rhi::Cull::None;       // billboards face the camera, winding does not matter
    d.depthTest = hasDepth;         // without a depth attachment there is nothing to test against
    d.depthWrite = false;
    d.blend = blend == ParticleBlend::Additive ? rhi::Blend::Additive : rhi::Blend::Alpha;
    d.colorFormats = {colorFormat};
    d.hasDepth = hasDepth;
    pipeline_ = dev.createPipeline(d);

    instanceBlock_ = dev.createBuffer(rhi::BufferUsage::Uniform, kMaxInstances * sizeof(GpuParticle));
    particleSet_ = dev.createDescriptorSet(rhi::SetType::Particles);
    particleSet_->setBuffer(11, instanceBlock_.get());
}

uint32_t ParticleRenderer::draw(rhi::Device& dev, rhi::DescriptorSet& frameSet,
                                const std::vector<ukiyo::ParticleInstance>& instances)
{
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(instances.size()), kMaxInstances);
    if (count == 0) return 0;

    std::vector<GpuParticle> gpu(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(gpu[i].posSize, instances[i].position, sizeof(gpu[i].posSize));
        std::memcpy(gpu[i].color, instances[i].color, sizeof(gpu[i].color));
    }
    instanceBlock_->upload(gpu.data(), count * sizeof(GpuParticle));

    dev.bindPipeline(pipeline_.get());
    dev.bindDescriptorSet(0, &frameSet);
    dev.bindDescriptorSet(1, particleSet_.get());
    dev.drawInstanced(6, count);
    return count;
}

} // namespace ukiyo
