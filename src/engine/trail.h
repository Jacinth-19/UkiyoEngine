// Trails: the ribbon of quads a moving object leaves behind it (Godot 4.8's Line3D / Trail3D, in the engine's idiom).
//
// Godot's node owns a list of sections, adds one when the object has moved `min_section_length`, and on demand builds a
// strip mesh whose vertices are offset sideways from the path. Everything about the look is a property: a width, a
// width curve along the trail, a colour gradient along it, UV tiling, and whether the strip faces the camera or a fixed
// "up". This file is the same idea without a scene tree, because a trail is geometry: a list of points and a function
// that turns them into triangles.
//
// The split is deliberate. `Trail` is pure CPU and deterministic (no device, no clock of its own: `update(dt)` takes
// the time, so a test or a replay can drive it), which is what lets tests/trail_test.cpp check the geometry without a
// GPU. `TrailRenderer` (trail_renderer.h) uploads the triangles and draws them; `tools/trail_preview.cpp` rasterises
// the same triangles on the CPU for the figure in docs/TRAIL.md.
//
// Units are metres and seconds. A trail point is one section boundary, oldest first, and the newest point follows the
// emitter: between section additions the head moves, so the ribbon is continuous to the object rather than lagging a
// section behind it (Godot's Trail3D does the same with its last point).
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

// What ends a trail: its age (Godot's LIMIT_MODE_LIFETIME) or its length (LIMIT_MODE_MAX_LENGTH).
enum class TrailLimit { Lifetime, MaxLength };

// How the strip is oriented. Billboard turns the strip about its tangent to face the camera; Local keeps it in the
// plane whose normal is `TrailSettings::localUp`, which is what a ribbon lying on the ground (a skid mark, a trail in
// the snow) wants: its width should not change as the camera orbits.
enum class TrailAlign { Billboard, Local };

// How the texture is mapped along the strip. Stretch maps the whole trail to 0..1, so the texture squashes as the trail
// grows; Tile maps arc length, so the texture slides along the strip at a fixed scale.
enum class TrailTiling { Stretch, Tile };

// One stop of a colour gradient along the trail, t = 0 at the oldest point and 1 at the emitter.
struct TrailColorStop {
    float t = 0.0f;
    glm::vec4 color{1.0f};
};

// A colour along the trail, linearly interpolated between stops (Godot's Gradient, without the colour-space modes).
// The alpha of the stops is the trail's fade: a gradient that ends at alpha 0 fades the head out.
class TrailGradient {
public:
    TrailGradient();
    explicit TrailGradient(std::vector<TrailColorStop> stops);
    // t is clamped to [0, 1]; a gradient with no stops is opaque white.
    glm::vec4 sample(float t) const;
    void setStops(std::vector<TrailColorStop> stops);
    const std::vector<TrailColorStop>& stops() const { return stops_; }

private:
    std::vector<TrailColorStop> stops_;
};

// Width along the trail as a multiplier of TrailSettings::width: a piecewise-linear curve through points (t, value),
// t = 0 at the oldest point. This is Godot's Curve sampled at one point; the engine has no curve resource, and a
// piecewise-linear one is what a width preset needs.
class TrailWidthCurve {
public:
    TrailWidthCurve();
    explicit TrailWidthCurve(std::vector<glm::vec2> points);
    // t is clamped to [0, 1]; a curve with no points is a constant 1.
    float sample(float t) const;
    void setPoints(std::vector<glm::vec2> points);
    const std::vector<glm::vec2>& points() const { return points_; }

private:
    std::vector<glm::vec2> points_;
};

struct TrailSettings {
    TrailLimit limit = TrailLimit::Lifetime;
    float lifetime = 0.35f;          // seconds a section lives, in Lifetime mode
    float maxLength = 6.0f;          // metres of path kept, in MaxLength mode
    float minSectionLength = 0.12f;  // the head must move this far before a section is added
    float width = 0.28f;             // metres at the widest point of the curve
    TrailWidthCurve widthCurve;
    TrailGradient gradient;
    TrailAlign align = TrailAlign::Billboard;
    TrailTiling tiling = TrailTiling::Tile;
    float tilingMultiplier = 0.45f;  // texture repeats per metre of path (u = arc * multiplier)
    bool pinUv = false;              // the texture stays put in space instead of sliding with the trail
    int maxSections = 64;            // hard cap, so a fast object with a long lifetime cannot grow without bound
    glm::vec3 localUp{0.0f, 1.0f, 0.0f};
};

// One vertex of the strip: position, texture coordinate (u along the trail, v across it) and the interpolated colour.
struct TrailVertex {
    glm::vec3 position{0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};
};

class Trail {
public:
    Trail() = default;
    explicit Trail(const TrailSettings& settings) : settings_(settings) {}

    const TrailSettings& settings() const { return settings_; }
    // Changing the settings keeps the points already laid down; a shorter lifetime or length trims them on the next
    // update, which is what makes a settings panel respond to a slider without a visible restart.
    void setSettings(const TrailSettings& settings) { settings_ = settings; }

    // Starts a new trail at `head`: the points are dropped and `head` becomes the only one. The trail then follows.
    void reset(const glm::vec3& head);
    // The emitter's position this frame. Adds a section once the head has moved `minSectionLength` from the last
    // point; short of that it moves the newest point, so the ribbon always reaches the object. Returns true when a
    // section was added (which is what a burst effect keys off).
    //
    // Named `advance` rather than `emit` because Qt defines `emit` as a keyword macro, and this header is included by
    // the editor's render thread. A Qt keyword is a compile error in a class declaration, so the method is named for
    // what it does to the emitter instead of what it does to the scene.
    bool advance(const glm::vec3& head);
    // Ages the sections by dt and drops the ones the limit removed. Points are never trimmed below two: a single
    // point has no ribbon, and an emitter that stops should leave a trail that fades rather than a pop.
    void update(float dt);
    void clear();

    int sectionCount() const { return static_cast<int>(points_.size()); }
    const glm::vec3& sectionPosition(int i) const { return points_[static_cast<size_t>(i)].position; }
    float sectionAge(int i) const { return points_[static_cast<size_t>(i)].age; }
    // Arc length of the whole trail in metres, along the points as they are now.
    float length() const;
    // True when there is a ribbon to draw (at least two sections and a non-zero width).
    bool hasGeometry() const { return points_.size() >= 2 && settings_.width > 0.0f; }

    // Fills `vertices` (two per section) and `indices` (six per section pair) with the strip for this frame.
    // `eye` is the camera position, used only by TrailAlign::Billboard. Returns the number of triangles.
    //
    // The buffers are cleared first, so a caller can keep one vector and reuse it every frame. A trailing point whose
    // tangent is degenerate (the head has not moved yet) borrows its neighbour's, so the first frame of a trail is a
    // strip rather than a zero-area sliver.
    uint32_t buildRibbon(const glm::vec3& eye, std::vector<TrailVertex>* vertices,
                         std::vector<uint32_t>* indices) const;

    // The same, into caller-owned arrays sized for the current section count: 2 * sectionCount() vertices and
    // 6 * (sectionCount() - 1) indices. Returns the vertex count written.
    uint32_t buildRibbon(const glm::vec3& eye, TrailVertex* vertices, uint32_t vertexCapacity,
                         uint32_t* indices, uint32_t indexCapacity, uint32_t* indexCount) const;

private:
    struct Section {
        glm::vec3 position{0.0f};
        glm::vec3 previous{0.0f};   // where the section was last frame, for the tangent at the head
        float age = 0.0f;
        float arc = 0.0f;           // distance from the first point ever laid down, never renumbered
    };
    // Recomputes `arc` from the tail and the length of the path, after a point was added or trimmed.
    void renumber();
    float span() const;             // path length between the first and last point

    TrailSettings settings_;
    std::vector<Section> points_;
    float totalArc_ = 0.0f;         // arc of the newest point, so a new one continues the numbering
};

} // namespace ukiyo
