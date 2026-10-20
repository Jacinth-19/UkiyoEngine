#include "engine/trail_renderer.h"

#include <algorithm>
#include <cstring>

namespace ukiyo {

namespace {

// Matches the PushBlock in shaders/common.glsl: 128 bytes, the guaranteed push-constant size.
struct Push {
    glm::mat4 model{1.0f};
    glm::vec4 p0{0.0f};   // x edge softness, y opacity
    glm::vec4 p1{0.0f};
    glm::vec4 p2{0.0f};
    glm::vec4 p3{0.0f};
};
static_assert(sizeof(Push) == 128, "push constants are shared with shaders/common.glsl");

} // namespace

TrailRenderer::TrailRenderer(rhi::Device& dev, rhi::Format colorFormat, bool hasDepth, TrailBlend blend, float softEdge)
    : softEdge_(softEdge < 0.0f ? 0.0f : (softEdge > 1.0f ? 1.0f : softEdge))
{
    rhi::PipelineDesc d;
    d.vertexShader = "trail";
    d.fragmentShader = "trail";
    d.sets = {rhi::SetType::Frame};   // per-frame camera only: the ribbon is already in world space
    d.vertexInput = true;             // rhi::Vertex: position, uv, colour
    d.cull = rhi::Cull::None;         // the strip can twist and fold; culling it would make it vanish
    d.depthTest = hasDepth;
    d.depthWrite = false;             // transparent geometry must not occlude what is behind it
    d.blend = blend == TrailBlend::Additive ? rhi::Blend::Additive : rhi::Blend::Alpha;
    d.colorFormats = {colorFormat};
    d.hasDepth = hasDepth;
    pipeline_ = dev.createPipeline(d);

    ensureCapacity(dev, kInitialVertices, (kInitialVertices / 2) * 3);
}

bool TrailRenderer::ensureCapacity(rhi::Device& dev, uint32_t vertices, uint32_t indices)
{
    // Quads are two triangles, so the index count follows from the vertex count. Grown separately anyway: a caller can
    // ask for a big index count directly.
    vertices = std::max(vertices, 2u);
    if (vertices > kMaxVertices) return false;
    indices = std::max(indices, 3u);

    uint32_t wanted = vertexCapacity_ == 0 ? kInitialVertices : vertexCapacity_;
    while (wanted < vertices) {
        if (wanted > kMaxVertices / 2) { wanted = kMaxVertices; break; }
        wanted *= 2;
    }
    if (wanted == vertexCapacity_ && vertexBuffer_ && indexBuffer_ &&
        indices <= indexBuffer_->size() / sizeof(uint32_t)) return true;

    vertexBuffer_ = dev.createBuffer(rhi::BufferUsage::Vertex, static_cast<size_t>(wanted) * sizeof(rhi::Vertex));
    // Three indices per triangle, two triangles per quad, one quad per section pair: generous, and the exact figure is
    // checked at draw time.
    const size_t indexCount = std::max<size_t>(static_cast<size_t>(wanted) * 3 / 2, indices);
    indexBuffer_ = dev.createBuffer(rhi::BufferUsage::Index, indexCount * sizeof(uint32_t));
    vertexCapacity_ = wanted;
    return true;
}

uint32_t TrailRenderer::draw(rhi::Device& dev, rhi::DescriptorSet& frameSet, const glm::vec3& eye,
                             const std::vector<const Trail*>& trails)
{
    vertices_.clear();
    indices_.clear();
    // Count first, so one oversized trail cannot leave the buffer half full and the draw half of two trails.
    uint32_t wantedVertices = 0, wantedIndices = 0;
    for (const Trail* t : trails) {
        if (!t || !t->hasGeometry()) continue;
        const uint32_t sections = static_cast<uint32_t>(t->sectionCount());
        if (sections < 2) continue;
        wantedVertices += sections * 2;
        wantedIndices += (sections - 1) * 6;
    }
    if (wantedVertices == 0) return 0;
    if (wantedVertices > kMaxVertices || !ensureCapacity(dev, wantedVertices, wantedIndices)) return 0;

    vertices_.reserve(wantedVertices);
    indices_.reserve(wantedIndices);
    for (const Trail* t : trails) {
        if (!t || !t->hasGeometry()) continue;
        const uint32_t base = static_cast<uint32_t>(vertices_.size());
        t->buildRibbon(eye, &trailVertices_, &trailIndices_);
        if (trailVertices_.size() != static_cast<size_t>(t->sectionCount()) * 2) continue;
        for (const TrailVertex& tv : trailVertices_) {
            rhi::Vertex v;
            v.pos = tv.position;
            v.uv = tv.uv;
            v.color = tv.color;
            vertices_.push_back(v);
        }
        for (uint32_t idx : trailIndices_) indices_.push_back(base + idx);
    }
    if (vertices_.empty() || indices_.empty()) return 0;
    if (vertices_.size() > vertexCapacity_ || indices_.size() > indexBuffer_->size() / sizeof(uint32_t)) {
        vertices_.clear();
        indices_.clear();
        return 0;
    }

    vertexBuffer_->upload(vertices_.data(), vertices_.size() * sizeof(rhi::Vertex));
    indexBuffer_->upload(indices_.data(), indices_.size() * sizeof(uint32_t));

    Push pc;
    pc.p0 = glm::vec4(softEdge_, opacity_, 0.0f, 0.0f);
    dev.bindPipeline(pipeline_.get());
    dev.bindDescriptorSet(0, &frameSet);
    dev.bindVertexBuffer(vertexBuffer_.get());
    dev.bindIndexBuffer(indexBuffer_.get());
    dev.pushConstants(&pc, sizeof(pc));
    dev.drawIndexed(static_cast<uint32_t>(indices_.size()));
    return static_cast<uint32_t>(indices_.size() / 3);
}

} // namespace ukiyo
