// GPU side of trails: uploads the ribbon triangles `Trail` builds and draws them in one indexed call.
//
// Mirrors ParticleRenderer: the CPU owns the simulation, this class owns one pipeline and one pair of vertex/index
// buffers, and a frame is a single draw when all the trails in it share a blend mode. Vertices go out in the engine's
// standard rhi::Vertex layout (position, uv, colour), so the trail shader reads them exactly like a mesh and the RHI
// needs no new vertex format.
//
// Blending is chosen at construction. Light streaks and sword swipes usually want Additive; dust, skid marks and
// cloth-like trails want Alpha. Two TrailRenderers (one of each) cover both without per-draw pipeline switches.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "engine/trail.h"
#include "rhi/rhi.h"

namespace ukiyo {

enum class TrailBlend { Alpha, Additive };

class TrailRenderer {
public:
    // Room for this many triangles before the buffers are grown (they double, and are re-created, up to a hard cap).
    // 4096 vertices is 384 KB of vertex buffer and draws 2047 quads, which is a dozen full trails.
    static constexpr uint32_t kInitialVertices = 4096;
    // A stop against a runaway frame: 1 M vertices is 96 MB and would be a bug, not a scene.
    static constexpr uint32_t kMaxVertices = 1u << 20;

    // colorFormat: the target the pass draws into (Format::Backbuffer for the presentation target).
    // hasDepth: whether that pass has a D32F depth attachment. With one, trails depth-test but do not write depth.
    // softEdge: 0 draws the ribbon with clean edges; 1 fades the last third of the width on both sides, which is what
    // makes a trail read as light rather than as a strip of paper.
    TrailRenderer(rhi::Device& dev, rhi::Format colorFormat, bool hasDepth,
                  TrailBlend blend = TrailBlend::Alpha, float softEdge = 0.35f);

    // Draws every trail in `trails` inside an open pass, in one indexed call. `eye` is the camera position used to
    // orient billboarded trails, and must be the same eye the frame's view-projection was built from. Trails with no
    // geometry are skipped. Returns the number of triangles drawn; a trail that would overflow the buffer cap is
    // skipped whole rather than drawn half.
    uint32_t draw(rhi::Device& dev, rhi::DescriptorSet& frameSet, const glm::vec3& eye,
                  const std::vector<const Trail*>& trails);

    // Changes the edge softness without rebuilding the pipeline.
    void setSoftEdge(float softEdge) { softEdge_ = softEdge < 0.0f ? 0.0f : (softEdge > 1.0f ? 1.0f : softEdge); }
    float softEdge() const { return softEdge_; }
    // Multiplies every trail's alpha this draw. 1 is unchanged; useful for a global "effects intensity" slider.
    void setOpacity(float opacity) { opacity_ = opacity < 0.0f ? 0.0f : opacity; }
    float opacity() const { return opacity_; }
    // Vertices the buffers can hold without being re-created.
    uint32_t vertexCapacity() const { return vertexCapacity_; }

private:
    bool ensureCapacity(rhi::Device& dev, uint32_t vertices, uint32_t indices);

    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> vertexBuffer_;
    std::unique_ptr<rhi::Buffer> indexBuffer_;
    uint32_t vertexCapacity_ = 0;

    // One interleaved copy of every trail this frame, plus per-trail scratch reused between frames so a steady frame
    // does not allocate.
    std::vector<rhi::Vertex> vertices_;
    std::vector<uint32_t> indices_;
    std::vector<TrailVertex> trailVertices_;
    std::vector<uint32_t> trailIndices_;
    float softEdge_ = 0.35f;
    float opacity_ = 1.0f;
};

} // namespace ukiyo
