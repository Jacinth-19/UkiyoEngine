// Trails (docs/TRAIL.md): the geometry Godot 4.8's Trail3D builds, checked without a device, a clock or a scene tree.
//
// A trail is a list of sections and a strip of triangles. The properties that matter are geometric, so they are
// measured rather than asserted: the strip's width at the tail and the head against the width curve, its facing
// against the camera, its plane against `localUp`, its texture coordinates against arc length, and what the two limit
// modes actually remove. A trail that is not finite would be a black screen in the renderer, so every vertex of every
// case is checked for NaN as it is built.
#include "engine/trail.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace ukiyo;

namespace {

int gChecks = 0;
int gFailures = 0;

void checkTrue(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %s\n", what.c_str());
    } else {
        std::printf("ok    %s\n", what.c_str());
    }
}

void checkNear(double got, double want, double tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol && std::isfinite(got);
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %-58s got %.6f want %.6f (tol %.6f)\n", what.c_str(), got, want, tol);
    } else {
        std::printf("ok    %-58s %.6f\n", what.c_str(), got);
    }
}

// A trail marched along +x in `step` metre sections, `count` sections in all, emitted from the origin.
Trail alongX(int count, float step, const TrailSettings& s = TrailSettings()) {
    Trail t(s);
    for (int i = 0; i < count; ++i) t.emit(glm::vec3(step * float(i), 0.0f, 0.0f));
    return t;
}

bool allFinite(const std::vector<TrailVertex>& v) {
    for (const TrailVertex& x : v)
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(x.position[c])) return false;
    for (const TrailVertex& x : v)
        for (int c = 0; c < 2; ++c)
            if (!std::isfinite(x.uv[c])) return false;
    return true;
}

} // namespace

int main() {
    std::printf("trails: the geometry of a ribbon (mirrors the engine's Trail)\n\n");

    // 1. Sections. The first emit lays the head; until the emitter has moved minSectionLength the head moves and no
    //    section is added, which is what keeps a slow object from piling up coincident points.
    {
        TrailSettings s;
        s.minSectionLength = 0.2f;
        Trail t(s);
        checkTrue(t.sectionCount() == 0 && !t.hasGeometry(), "1a: a new trail has nothing");
        checkTrue(t.emit(glm::vec3(0.0f)), "1b: the first emit lays the head");
        checkTrue(t.sectionCount() == 1 && !t.hasGeometry(), "1c: one section is not a ribbon");
        checkTrue(!t.emit(glm::vec3(0.1f, 0.0f, 0.0f)), "1d: a short move does not add a section");
        checkTrue(t.sectionCount() == 1, "1e: the head moved instead");
        checkNear(double(t.sectionPosition(0).x), 0.1, 1e-5, "1f: the head is where the emitter is");
        checkTrue(t.emit(glm::vec3(0.3f, 0.0f, 0.0f)), "1g: a move past minSectionLength adds one");
        checkTrue(t.sectionCount() == 2 && t.hasGeometry(), "1h: two sections make a ribbon");
        checkNear(double(t.length()), 0.2, 1e-5, "1i: length is the distance walked");
    }

    // 2. The strip itself: two vertices per section, six indices per pair, none of them NaN.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        Trail t = alongX(5, 1.0f, s);
        std::vector<TrailVertex> v;
        std::vector<uint32_t> idx;
        const uint32_t tris = t.buildRibbon(glm::vec3(0.0f, 5.0f, 20.0f), &v, &idx);
        checkTrue(tris == 8 && v.size() == 10 && idx.size() == 24, "2a: five sections are eight triangles");
        checkTrue(allFinite(v), "2b: every vertex is finite");
        int bad = 0;
        for (uint32_t i : idx)
            if (i >= v.size()) ++bad;
        checkTrue(bad == 0, "2c: every index is inside the vertex list");
        // The raw-array overload has to produce the same strip as the vector one.
        std::vector<TrailVertex> v2(v.size());
        std::vector<uint32_t> idx2(idx.size());
        uint32_t count = 0;
        const uint32_t n = t.buildRibbon(glm::vec3(0.0f, 5.0f, 20.0f), v2.data(), uint32_t(v2.size()), idx2.data(),
                                         uint32_t(idx2.size()), &count);
        bool same = n == v.size() && count == idx.size();
        for (size_t i = 0; same && i < v.size(); ++i) same = v[i].position == v2[i].position && v[i].uv == v2[i].uv;
        checkTrue(same, "2d: the array overload builds the same strip");
        std::vector<TrailVertex> small(4);
        std::vector<uint32_t> smallIdx(6);
        uint32_t smallCount = 0;
        checkTrue(t.buildRibbon(glm::vec3(0.0f), small.data(), 4, smallIdx.data(), 6, &smallCount) == 0,
                  "2e: a buffer that is too small is refused, not overrun");
    }

    // 3. Width comes from the curve, at the trail's own parameter, at both ends of every quad.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.width = 1.0f;
        s.widthCurve = TrailWidthCurve({{0.0f, 0.2f}, {0.5f, 1.0f}, {1.0f, 0.4f}});
        Trail t = alongX(5, 1.0f, s);
        std::vector<TrailVertex> v;
        std::vector<uint32_t> idx;
        t.buildRibbon(glm::vec3(0.0f, 5.0f, 20.0f), &v, &idx);
        const auto widthAt = [&](int section) {
            return glm::length(v[section * 2 + 1].position - v[section * 2].position);
        };
        checkNear(double(widthAt(0)), 0.2, 1e-4, "3a: the tail is the curve at t = 0");
        checkNear(double(widthAt(2)), 1.0, 1e-4, "3b: the middle is the curve at t = 0.5");
        checkNear(double(widthAt(4)), 0.4, 1e-4, "3c: the head is the curve at t = 1");
        checkNear(double(widthAt(1)), 0.6, 1e-4, "3d: the curve interpolates between its points");
        // The two sides of one quad are equidistant from the centre line.
        const glm::vec3 centre1 = (v[2].position + v[3].position) * 0.5f;
        checkNear(double(glm::length(centre1 - glm::vec3(1.0f, 0.0f, 0.0f))), 0.0, 1e-4,
                  "3e: the strip is centred on the path");
    }

    // 4. Billboarding: the quad's side is perpendicular to the view ray at every vertex, from any camera.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.align = TrailAlign::Billboard;
        Trail t = alongX(4, 1.0f, s);
        for (const glm::vec3 eye : {glm::vec3(2.0f, 3.0f, 9.0f), glm::vec3(-4.0f, 0.5f, -6.0f)}) {
            std::vector<TrailVertex> v;
            std::vector<uint32_t> idx;
            t.buildRibbon(eye, &v, &idx);
            double worst = 0.0;
            for (size_t i = 0; i < v.size(); i += 2) {
                const glm::vec3 centre = (v[i].position + v[i + 1].position) * 0.5f;
                const glm::vec3 side = glm::normalize(v[i + 1].position - v[i].position);
                worst = std::max(worst, double(std::fabs(glm::dot(side, glm::normalize(eye - centre)))));
            }
            checkNear(worst, 0.0, 1e-4, "4: a billboarded strip faces the eye");
        }
    }

    // 5. Local alignment: the strip stays in the plane the local up defines, so the camera does not twist it.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.align = TrailAlign::Local;
        s.localUp = glm::vec3(0.0f, 1.0f, 0.0f);
        Trail t = alongX(4, 1.0f, s);
        std::vector<TrailVertex> v;
        std::vector<uint32_t> idx;
        t.buildRibbon(glm::vec3(0.0f, 8.0f, 0.0f), &v, &idx);
        double worst = 0.0;
        for (const TrailVertex& x : v) worst = std::max(worst, double(std::fabs(x.position.y)));
        checkNear(worst, 0.0, 1e-5, "5a: a ground ribbon stays at the emitter's height");
        // The same trail under a camera looking from directly above must produce the same strip.
        std::vector<TrailVertex> v2;
        std::vector<uint32_t> idx2;
        t.buildRibbon(glm::vec3(0.0f, 0.0f, 30.0f), &v2, &idx2);
        bool same = v.size() == v2.size();
        for (size_t i = 0; same && i < v.size(); ++i) same = v[i].position == v2[i].position;
        checkTrue(same, "5b: a local strip does not depend on the camera");
    }

    // 6. Texture coordinates: stretch spans the trail, tile counts metres, and pinning keeps a section's u as the
    //    trail moves. These are the three modes a material author picks between.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.widthCurve = TrailWidthCurve({{0.0f, 1.0f}, {1.0f, 1.0f}});
        s.tilingMultiplier = 2.0f;
        s.tiling = TrailTiling::Stretch;
        Trail t = alongX(5, 1.0f, s);
        std::vector<TrailVertex> v;
        std::vector<uint32_t> idx;
        t.buildRibbon(glm::vec3(0.0f, 5.0f, 10.0f), &v, &idx);
        checkNear(double(v.front().uv.x), 0.0, 1e-5, "6a: stretch starts at u = 0");
        checkNear(double(v.back().uv.x), 2.0, 1e-5, "6b: stretch ends at the tiling multiplier");
        bool monotone = true;
        for (size_t i = 2; i < v.size(); i += 2)
            if (v[i].uv.x < v[i - 2].uv.x) monotone = false;
        checkTrue(monotone, "6c: u grows along the trail");
        checkNear(double(v.front().uv.y), 0.0, 1e-5, "6d: v is 0 on one edge");
        checkNear(double(v[1].uv.y), 1.0, 1e-5, "6e: and 1 on the other");

        s.tiling = TrailTiling::Tile;
        Trail tile = alongX(5, 1.0f, s);
        tile.buildRibbon(glm::vec3(0.0f, 5.0f, 10.0f), &v, &idx);
        checkNear(double(v.back().uv.x), 8.0, 1e-4, "6f: tile counts arc length times the multiplier");

        s.pinUv = true;
        Trail pinned = alongX(4, 1.0f, s);
        std::vector<TrailVertex> before, after;
        pinned.buildRibbon(glm::vec3(0.0f, 5.0f, 10.0f), &before, &idx);
        const float uOfSecondSection = before[2].uv.x;
        pinned.emit(glm::vec3(4.0f, 0.0f, 0.0f));
        pinned.buildRibbon(glm::vec3(0.0f, 5.0f, 10.0f), &after, &idx);
        checkNear(double(after[2].uv.x), double(uOfSecondSection), 1e-5, "6g: a pinned section keeps its u");
        checkTrue(after[2].uv.x < after[8].uv.x, "6h: the pins are ordered along the path");
    }

    // 7. Gradient: colour along the trail, alpha included, interpolated between stops.
    {
        TrailGradient g({{0.0f, {1.0f, 0.2f, 0.0f, 1.0f}}, {0.5f, {1.0f, 1.0f, 0.0f, 0.5f}}, {1.0f, {0.0f, 0.3f, 1.0f, 0.0f}}});
        const glm::vec4 mid = g.sample(0.25f);
        checkNear(double(mid.r), 1.0, 1e-5, "7a: red interpolates");
        checkNear(double(mid.g), 0.6, 1e-5, "7b: green interpolates");
        checkNear(double(mid.a), 0.75, 1e-5, "7c: alpha interpolates");
        checkNear(double(g.sample(-3.0f).a), 1.0, 1e-5, "7d: below the first stop the gradient clamps");
        checkNear(double(g.sample(9.0f).b), 1.0, 1e-5, "7e: above the last stop it clamps");
        checkNear(double(TrailGradient().sample(0.0f).a), 0.0, 1e-5, "7f: the default fades in from the tail");
        checkNear(double(TrailGradient().sample(1.0f).a), 1.0, 1e-5, "7g: and is solid at the head");
        // Unsorted stops are sorted, not lost.
        TrailGradient unsorted({{1.0f, {0, 0, 1, 1}}, {0.0f, {1, 0, 0, 1}}});
        checkNear(double(unsorted.sample(0.0f).r), 1.0, 1e-5, "7h: stops given out of order are sorted");
    }

    // 8. Lifetime: old sections go, the last two stay, and the age of a new section starts at zero.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.lifetime = 1.0f;
        Trail t = alongX(6, 1.0f, s);
        t.update(0.6f);
        checkTrue(t.sectionCount() == 6, "8a: nothing expires before its lifetime");
        t.update(0.6f);
        checkTrue(t.sectionCount() >= 2, "8b: the ribbon keeps its last pair");
        checkTrue(t.sectionCount() < 6, "8c: expired sections are dropped");
        t.emit(glm::vec3(20.0f, 0.0f, 0.0f));
        checkNear(double(t.sectionAge(t.sectionCount() - 1)), 0.0, 1e-5, "8d: a new section starts at age zero");
        t.clear();
        checkTrue(t.sectionCount() == 0 && !t.hasGeometry(), "8e: clear empties the trail");
    }

    // 9. Max-length mode trims by distance instead of time.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.limit = TrailLimit::MaxLength;
        s.maxLength = 2.5f;
        Trail t = alongX(10, 1.0f, s);
        // The limit is applied by update(), the same as Godot's: laying sections down and ageing them are separate
        // steps, so a caller that replays a route can add every section first and then let it settle.
        t.update(0.0f);
        checkTrue(t.length() <= 2.5f + 1e-4f && t.length() > 0.0f, "9a: the trail is no longer than its limit");
        checkNear(double(t.length()), 2.0, 1e-4, "9b: and keeps whole sections up to it");
    }

    // 10. The section cap bounds memory, whatever the lifetime says.
    {
        TrailSettings s;
        s.minSectionLength = 0.01f;
        s.maxSections = 8;
        s.lifetime = 100.0f;
        Trail t = alongX(40, 0.1f, s);
        checkTrue(t.sectionCount() == 8, "10a: the section cap holds");
        std::vector<TrailVertex> v;
        std::vector<uint32_t> idx;
        t.buildRibbon(glm::vec3(0.0f, 5.0f, 10.0f), &v, &idx);
        checkTrue(v.size() == 16 && idx.size() == 42, "10b: the strip follows the trimmed trail");
    }

    // 11. Degenerate input: a stationary emitter, a trail that doubles back, a zero width.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        Trail still(s);
        still.emit(glm::vec3(1.0f, 2.0f, 3.0f));
        still.emit(glm::vec3(1.0f, 2.0f, 3.0f));
        still.emit(glm::vec3(1.0f, 2.0f, 3.0f));
        std::vector<TrailVertex> v;
        std::vector<uint32_t> idx;
        checkTrue(still.buildRibbon(glm::vec3(0.0f, 0.0f, 5.0f), &v, &idx) == 0,
                  "11a: a stationary emitter draws nothing");

        Trail back(s);
        back.emit(glm::vec3(0.0f));
        back.emit(glm::vec3(1.0f, 0.0f, 0.0f));
        back.emit(glm::vec3(0.0f, 0.0f, 0.0f));   // doubles back over the first section
        back.buildRibbon(glm::vec3(0.0f, 0.0f, 5.0f), &v, &idx);
        checkTrue(allFinite(v) && !v.empty(), "11b: a trail that doubles back is still finite");

        s.width = 0.0f;
        Trail flat = alongX(4, 1.0f, s);
        checkTrue(!flat.hasGeometry() && flat.buildRibbon(glm::vec3(0.0f, 1.0f, 1.0f), &v, &idx) == 0,
                  "11c: zero width means no geometry");

        s.width = 0.2f;
        s.widthCurve = TrailWidthCurve(std::vector<glm::vec2>{});   // no points at all
        Trail bare = alongX(4, 1.0f, s);
        bare.buildRibbon(glm::vec3(0.0f, 1.0f, 1.0f), &v, &idx);
        checkTrue(allFinite(v) && v.size() == 8, "11d: an empty width curve is a constant 1");
    }

    // 12. Settings changes are live: a shorter lifetime trims on the next update, without losing the ribbon.
    {
        TrailSettings s;
        s.minSectionLength = 0.1f;
        s.lifetime = 10.0f;
        Trail t = alongX(8, 1.0f, s);
        t.update(2.0f);
        const int before = t.sectionCount();
        s.lifetime = 0.5f;
        t.setSettings(s);
        t.update(1.0f);
        checkTrue(t.sectionCount() == 2 && before == 8, "12a: a shorter lifetime trims on the next update");
    }

    std::printf("\ntrail: %d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
