#include "engine/material_preview.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>
#include <stdexcept>

#include "engine/gpu_types.h"
#include "engine/graph_material.h"

namespace ukiyo {

namespace {

struct SphereMesh {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    uint32_t indexCount = 0;
};

SphereMesh makeSphere(rhi::Device& dev, int slices, int stacks)
{
    std::vector<rhi::Vertex> verts;
    for (int j = 0; j <= stacks; ++j) {
        const float v = float(j) / float(stacks);
        const float theta = v * glm::pi<float>();
        for (int i = 0; i <= slices; ++i) {
            const float u = float(i) / float(slices);
            const float phi = u * glm::two_pi<float>();
            const glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            rhi::Vertex vx{};
            vx.pos = n;
            vx.normal = n;
            vx.uv = glm::vec2(u, v);
            vx.weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            verts.push_back(vx);
        }
    }
    std::vector<uint32_t> idx;
    const uint32_t row = static_cast<uint32_t>(slices + 1);
    for (int j = 0; j < stacks; ++j) {
        for (int i = 0; i < slices; ++i) {
            const uint32_t a = static_cast<uint32_t>(j) * row + static_cast<uint32_t>(i);
            const uint32_t b = a + row;
            idx.insert(idx.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
    SphereMesh m;
    m.vertices = dev.createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
    m.vertices->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
    m.indices = dev.createBuffer(rhi::BufferUsage::Index, idx.size() * sizeof(uint32_t));
    m.indices->upload(idx.data(), idx.size() * sizeof(uint32_t));
    m.indexCount = static_cast<uint32_t>(idx.size());
    return m;
}

} // namespace

std::vector<uint8_t> renderMaterialSphere(rhi::Device& dev, GraphMaterial& material)
{
    const uint32_t w = dev.backbufferWidth();
    const uint32_t h = dev.backbufferHeight();
    if (!material.pipeline() || !material.set()) throw std::runtime_error("material has no pipeline");

    // Frame block: a fixed camera on the sphere and the engine's default sun and sky.
    FrameUBO f{};
    const glm::vec3 eye(0.0f, 0.3f, 4.0f);
    f.view = glm::lookAtRH(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    f.proj = glm::perspectiveRH_ZO(glm::radians(30.0f), float(w) / float(h), 0.1f, 20.0f);
    f.viewProj = f.proj * f.view;
    f.invViewProj = glm::inverse(f.viewProj);
    f.camPos = glm::vec4(eye, 1.0f);
    f.sunDir = glm::vec4(glm::normalize(glm::vec3(0.35f, 0.75f, 0.55f)), 0.0f);
    f.sunColor = glm::vec4(glm::vec3(1.0f, 0.96f, 0.9f) * 1.25f, 0.0f);
    f.skyColor = glm::vec4(glm::vec3(0.55f, 0.68f, 0.85f) * 0.6f, 0.0f);
    f.groundColor = glm::vec4(glm::vec3(0.35f, 0.30f, 0.25f) * 0.4f, 0.0f);
    f.screenParams = glm::vec4(float(w), float(h), 0.0f, 0.0f);
    for (auto& m : f.shadowMatrix) m = glm::mat4(1.0f);

    auto frameUbo = dev.createBuffer(rhi::BufferUsage::Uniform, sizeof(FrameUBO));
    frameUbo->upload(&f, sizeof(f));
    auto whiteDesc = rhi::TextureDesc{};
    auto white = dev.createTexture(whiteDesc, std::vector<uint8_t>{255, 255, 255, 255}.data());
    auto frameSet = dev.createDescriptorSet(rhi::SetType::Frame);
    frameSet->setBuffer(0, frameUbo.get());
    frameSet->setTexture(1, white.get());

    // Skin block: identity joints (the sphere is static).
    std::vector<glm::mat4> joints(128, glm::mat4(1.0f));
    auto skinUbo = dev.createBuffer(rhi::BufferUsage::Uniform, joints.size() * sizeof(glm::mat4));
    skinUbo->upload(joints.data(), joints.size() * sizeof(glm::mat4));
    auto skinSet = dev.createDescriptorSet(rhi::SetType::Skin);
    skinSet->setBuffer(9, skinUbo.get());

    rhi::TextureDesc dd;
    dd.width = w;
    dd.height = h;
    dd.format = rhi::Format::D32F;
    dd.filter = rhi::Filter::Nearest;
    auto depth = dev.createTexture(dd, nullptr);

    const SphereMesh mesh = makeSphere(dev, 48, 24);

    rhi::PassDesc pass;
    pass.backbuffer = true;
    pass.clearColor = true;
    pass.clearValue = glm::vec4(0.16f, 0.17f, 0.19f, 1.0f);
    pass.clearDepth = true;
    pass.depth = depth.get();

    if (!dev.beginFrame()) throw std::runtime_error("the device skipped the frame");
    dev.beginPass(pass);
    dev.setViewport(0, 0, static_cast<int>(w), static_cast<int>(h));
    dev.bindPipeline(material.pipeline());
    dev.bindDescriptorSet(0, frameSet.get());
    dev.bindDescriptorSet(1, material.set());
    dev.bindDescriptorSet(2, skinSet.get());
    dev.bindVertexBuffer(mesh.vertices.get());
    dev.bindIndexBuffer(mesh.indices.get());
    PushConstants pc{};
    pc.model = glm::mat4(1.0f);
    pc.p0 = glm::vec4(0.0f);   // x = 0: static mesh
    dev.pushConstants(&pc, sizeof(pc));
    dev.drawIndexed(mesh.indexCount);
    dev.endPass();
    dev.endFrame(true);
    std::vector<uint8_t> pixels = dev.takeCapture();
    dev.waitIdle();
    return pixels;
}

} // namespace ukiyo
