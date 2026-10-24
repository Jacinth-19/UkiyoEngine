// Percentage-closer soft shadows (docs/PCSS.md): the blocker search and the variable-width filter in src/engine/pcss.h,
// measured against the exact shadow of a disc light.
//
// The scene is one straight occluder edge and a receiver plane behind it, expressed as a shadow map and a distance to
// the light. That combination has a closed-form answer — the fraction of a disc whose ray clears the edge — so the test
// can ask how far PCSS is from the right picture, not merely whether it changed one. What is checked:
//
//   * the penumbra follows the analytic curve, and the error is small enough to be invisible;
//   * a fixed-radius PCF, on the same map, is much further from the answer: the feature earns its cost;
//   * contact hardening: the penumbra is proportional to the gap, and zero at the contact;
//   * a lit point stays lit and a deep shadow stays dark, with no energy leaking from the filter;
//   * the local-light formula, the clamp and the off switch behave as documented;
//   * every answer is finite and inside [0, 1].

#include "engine/pcss.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ukiyo;

namespace {

int gChecks = 0;
int gFailures = 0;

void checkTrue(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL %s\n", what);
    }
}

void checkNear(float got, float want, float tol, const char* what) {
    ++gChecks;
    if (!(std::fabs(got - want) <= tol)) {
        ++gFailures;
        std::printf("FAIL %s -- got %.4f, want %.4f +- %.4f\n", what, got, want, tol);
    }
}

void checkLess(float a, float b, const char* what) {
    ++gChecks;
    if (!(a < b)) {
        ++gFailures;
        std::printf("FAIL %s -- %.4f is not less than %.4f\n", what, a, b);
    }
}

// A shadow map of the edge scene. The light looks down -Z. Everything at x < edge is an occluder at distance
// `blockerDistance`; everywhere else the map holds the receiver plane itself at `receiverDistance`, which is what a
// floor in the map would put there and is not a blocker (the bias takes care of it).
struct EdgeMap {
    int size = 512;
    float halfWidth = 4.0f;   // metres from the centre to the map's edge
    float edge = 0.0f;        // world x of the occluder's edge
    float blockerDistance = 1.0f;
    float receiverDistance = 2.0f;
    std::vector<float> depth;

    void build() {
        depth.assign(size_t(size) * size_t(size), 0.0f);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float wx = (float(x) + 0.5f) / float(size) * 2.0f * halfWidth - halfWidth;
                depth[size_t(y) * size_t(size) + size_t(x)] = wx < edge ? blockerDistance : receiverDistance;
            }
        }
    }

    float texelMetres() const { return 2.0f * halfWidth / float(size); }
    float uvPerMetre() const { return 1.0f / (2.0f * halfWidth); }
    // World x -> map uv.
    float uvForX(float x) const { return (x + halfWidth) / (2.0f * halfWidth); }
};

float mapDepth(const void* map, glm::vec2 uv) {
    const auto* m = static_cast<const EdgeMap*>(map);
    if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) return 0.0f;
    const int x = std::min(int(uv.x * float(m->size)), m->size - 1);
    const int y = std::min(int(uv.y * float(m->size)), m->size - 1);
    return m->depth[size_t(y) * size_t(m->size) + size_t(x)];
}

float visibilityAt(const EdgeMap& map, float x, const PcssParams& p) {
    return pcssVisibility(glm::vec2(map.uvForX(x), 0.5f), map.receiverDistance, &map, mapDepth, map.uvPerMetre(), p);
}

// Mean absolute error of a mode against the analytic curve, over the penumbra and a margin either side.
float meanError(const EdgeMap& map, PcssParams p, float halfRange) {
    double sum = 0.0;
    int n = 0;
    for (float x = -halfRange; x <= halfRange; x += map.texelMetres()) {
        const float want = analyticEdgeVisibility(x, map.blockerDistance, map.receiverDistance, p.sunTanRadius);
        sum += std::fabs(double(visibilityAt(map, x, p)) - double(want));
        ++n;
    }
    return n > 0 ? float(sum / double(n)) : 0.0f;
}

// The x where the visibility crosses `level`, found by bisection on the monotone part of the profile.
float crossing(const EdgeMap& map, const PcssParams& p, float level, float lo, float hi) {
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (visibilityAt(map, mid, p) < level) lo = mid;
        else hi = mid;
    }
    return 0.5f * (lo + hi);
}

} // namespace

int main() {
    // 1. The penumbra itself. A gap of 1 m and a sun radius of 0.05 (2.9 degrees) put the full transition inside 10 cm,
    //    which is 10 texels of a 512 map over 8 m: a hard case, and the one the map cannot resolve at all.
    {
        EdgeMap map;
        map.size = 512;
        map.halfWidth = 4.0f;
        map.blockerDistance = 1.0f;
        map.receiverDistance = 2.0f;
        map.build();

        PcssParams pcss;
        pcss.mode = 2;
        pcss.sunTanRadius = 0.05f;
        pcss.searchRadius = 0.6f;

        PcssParams pcf = pcss;
        pcf.mode = 1;
        pcf.fixedRadius = 0.12f;

        PcssParams hard = pcss;
        hard.mode = 0;

        const float errPcss = meanError(map, pcss, 0.2f);
        const float errHard = meanError(map, hard, 0.2f);
        checkLess(errPcss, 0.06f, "1a: PCSS tracks the analytic disc shadow within 0.06");
        checkLess(errPcss, errHard, "1b: and closer than the hard shadow");

        // A fixed filter radius is one number for every distance. Measured across three gaps, the adaptive width has to
        // win clearly: that is the whole argument for the extra taps.
        float sumPcss = 0.0f, sumPcf = 0.0f;
        const float gaps[3] = {0.25f, 1.0f, 2.0f};
        for (float gap : gaps) {
            EdgeMap g = map;
            g.receiverDistance = g.blockerDistance + gap;
            g.build();
            sumPcss += meanError(g, pcss, 0.5f * gap + 0.05f);
            sumPcf += meanError(g, pcf, 0.5f * gap + 0.05f);
        }
        checkLess(sumPcss * 1.8f, sumPcf, "1c: across three gaps PCSS is far closer than one fixed-radius PCF");
        std::printf("  pcss %.4f  pcf %.4f  hard %.4f (single gap); across gaps pcss %.4f pcf %.4f\n",
                    double(errPcss), double(meanError(map, pcf, 0.2f)), double(errHard), double(sumPcss),
                    double(sumPcf));

        // 2. Contact hardening. The 10%-90% width of the penumbra is the analytic width for the gap, and it doubles when
        //    the gap doubles.
        // The analytic 10%-90% width, by bisection on the closed form: the same measurement the filter is put through.
        auto analyticCrossing = [&](float level, float gap) {
            float lo = -2.0f * 0.05f * gap, hi = 2.0f * 0.05f * gap;
            for (int i = 0; i < 50; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (analyticEdgeVisibility(mid, 1.0f, 1.0f + gap, 0.05f) < level) lo = mid;
                else hi = mid;
            }
            return 0.5f * (lo + hi);
        };
        float widths[3];
        const float gapSizes[3] = {0.25f, 0.5f, 1.0f};
        for (int i = 0; i < 3; ++i) {
            const float gaps_i = gapSizes[i];
            EdgeMap g = map;
            g.blockerDistance = 1.0f;
            g.receiverDistance = 1.0f + gaps_i;
            g.build();
            const float w = crossing(g, pcss, 0.9f, -0.5f, 0.5f) - crossing(g, pcss, 0.1f, -0.5f, 0.5f);
            widths[i] = w;
            const float want = analyticCrossing(0.9f, gaps_i) - analyticCrossing(0.1f, gaps_i);
            // The map is 512 texels over 8 m, so 1.6 cm is the resolution the answer can be located to; allow for that
            // plus the filter's own noise.
            const float tol = std::max(0.3f * want, 4.0f * g.texelMetres());
            checkNear(w, want, tol, "2a: the penumbra width matches the disc light's geometry");
        }
        checkLess(widths[0], widths[1], "2b: a bigger gap widens the penumbra");
        checkLess(widths[1], widths[2], "2c: and keeps widening it");

        // The contact: at a 5 cm gap the penumbra has to be crisp, and crisper than a wider gap's. This is what stops
        // an object on the floor from looking like it floats over a blob.
        EdgeMap tight = map;
        tight.receiverDistance = tight.blockerDistance + 0.05f;
        tight.build();
        const float tightWidth = crossing(tight, pcss, 0.9f, -0.5f, 0.5f) - crossing(tight, pcss, 0.1f, -0.5f, 0.5f);
        checkLess(tightWidth, 0.06f, "2d: at a 5 cm gap the penumbra is crisp");
        checkLess(tightWidth, widths[0], "2e: and crisper than at a 25 cm gap");

        // 3. No leaking. A point far from the edge is fully lit, and one deep inside the shadow is fully dark: the
        //    filter widens the edge, it does not brighten what is behind it.
        checkNear(visibilityAt(map, 1.5f, pcss), 1.0f, 1e-4f, "3a: far into the light is exactly lit");
        checkNear(visibilityAt(map, -1.5f, pcss), 0.0f, 1e-4f, "3b: far into the shadow is exactly dark");

        // 4. Bounds and determinism, over a sweep that includes the edge and the map's border.
        bool inRange = true;
        bool stable = true;
        for (float x = -3.9f; x <= 3.9f; x += 0.013f) {
            const float v = visibilityAt(map, x, pcss);
            inRange = inRange && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
            stable = stable && v == visibilityAt(map, x, pcss);
        }
        checkTrue(inRange, "4a: every visibility is finite and inside [0, 1]");
        checkTrue(stable, "4b: the same query gives the same answer");
    }

    // 5. The width formula, for both kinds of light, and the clamp.
    {
        PcssParams p;
        p.directional = true;
        p.sunTanRadius = 0.01f;
        p.maxPenumbra = 10.0f;
        checkNear(penumbraWidth(p, 2.0f, 3.0f), 0.02f, 1e-6f, "5a: a directional light's penumbra is the sun's angle times the gap");
        checkNear(penumbraWidth(p, 2.0f, 2.0f), 0.0f, 1e-9f, "5b: and zero at contact");
        checkNear(penumbraWidth(p, 2.0f, 1.0f), 0.0f, 1e-9f, "5c: a receiver in front of the blocker casts nothing");

        p.directional = false;
        p.lightRadius = 0.5f;
        checkNear(penumbraWidth(p, 2.0f, 4.0f), 0.5f, 1e-6f, "5d: a local light's penumbra is the light's radius times the relative gap");

        p.maxPenumbra = 0.1f;
        checkNear(penumbraWidth(p, 1.0f, 100.0f), 0.1f, 1e-6f, "5e: the clamp holds a penumbra the size of a room");
    }

    // 6. The analytic reference itself: 0.5 at the edge, 1 well lit, 0 well dark, monotone between.
    {
        bool monotone = true;
        float previous = 0.0f;
        for (float u = -1.0f; u <= 1.0f; u += 0.01f) {
            const float v = analyticEdgeVisibility(u * 0.05f, 1.0f, 2.0f, 0.05f);
            monotone = monotone && v >= previous - 1e-6f;
            previous = v;
        }
        checkTrue(monotone, "6a: the disc shadow rises across the edge");
        checkNear(analyticEdgeVisibility(0.0f, 1.0f, 2.0f, 0.05f), 0.5f, 1e-4f, "6b: half lit at the edge");
        checkNear(analyticEdgeVisibility(0.3f, 1.0f, 2.0f, 0.05f), 1.0f, 1e-4f, "6c: fully lit past the penumbra");
        checkNear(analyticEdgeVisibility(-0.3f, 1.0f, 2.0f, 0.05f), 0.0f, 1e-4f, "6d: fully dark behind it");
    }

    // 7. The tap pattern: deterministic, inside the unit disc, and distinct.
    {
        bool inside = true, distinct = true;
        std::vector<glm::vec2> taps;
        for (int i = 0; i < 24; ++i) {
            const glm::vec2 t = vogelTap(i, 24);
            inside = inside && glm::length(t) <= 1.0f + 1e-5f;
            taps.push_back(t);
        }
        for (size_t i = 0; i < taps.size(); ++i)
            for (size_t j = i + 1; j < taps.size(); ++j)
                distinct = distinct && glm::length(taps[i] - taps[j]) > 1e-3f;
        checkTrue(inside, "7a: every tap is inside the disc");
        checkTrue(distinct, "7b: no two taps coincide");
        checkTrue(vogelTap(5, 24) == vogelTap(5, 24), "7c: the taps are a fixed pattern");
    }

    // 8. The shader's algebra. shaders/shadow_sample.glsl works in the shadow matrix's own units -- uv and z -- and
    //    derives both of its scales from the cascade's matrix rows, because a cascade's box changes size with the
    //    camera. A wrong factor there would widen or narrow every penumbra on the GPU, and no test of the header would
    //    notice: this checks the derivation against a matrix built the way the renderer builds one, then runs the
    //    shader's arithmetic against the header's on the same map.
    {
        // fitCascade's box: an orthographic projection over a 2R square, near 0.01, far 4R, with the light this far
        // back from the centre.
        const float radius = 4.0f;
        const glm::vec3 lightDir = glm::normalize(glm::vec3(0.0f, 0.0f, -1.0f));   // the direction light travels
        const glm::vec3 towards = -lightDir;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::mat4 view = glm::lookAtRH(towards * radius * 2.0f, glm::vec3(0.0f), up);
        const glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.01f, radius * 4.0f);
        const glm::mat4 M = proj * view;

        // The scale the shader reads, and the one the matrix actually has: uv spans 0.5 of the atlas across W metres,
        // and clip x spans [-1, 1] across the same W, so uv per metre is |row 0| / 4.
        const float uvPerMetre = 0.25f * glm::length(glm::vec3(M[0]));
        const float zPerMetre = std::fabs(M[2][2]);
        const glm::vec3 right = glm::normalize(glm::cross(towards, up));
        const glm::vec3 p0(0.37f, -0.21f, 0.0f);
        auto clip = [&](const glm::vec3& p) { const glm::vec4 c = M * glm::vec4(p, 1.0f); return glm::vec3(c) / c.w; };
        const glm::vec3 a = clip(p0), b = clip(p0 + right), c = clip(p0 + towards);
        checkNear(std::fabs(b.x - a.x) * 0.25f, uvPerMetre, 1e-5f,
                  "8a: a metre across the light's plane is |row 0| / 4 of atlas uv");
        checkNear(std::fabs(c.z - a.z), zPerMetre, 1e-6f, "8b: a metre towards the light is |row 2| of shadow z");
        checkNear(uvPerMetre * (2.0f * radius) * 2.0f, 1.0f, 1e-4f,
                  "8c: and the whole cascade lands on half the atlas");

        // The shader's arithmetic, in z units, on the map section 1 already measured: the same blocker, receiver and
        // sun, the same 16 and 32 taps, the same Vogel spiral.
        constexpr float kNear = 0.01f, kFar = 16.0f;
        const float zPerMetreScene = 1.0f / (kFar - kNear);
        auto toZ = [&](float metres) { return (metres - kNear) * zPerMetreScene; };

        EdgeMap map;
        map.size = 512;
        map.halfWidth = 4.0f;
        map.blockerDistance = 1.0f;
        map.receiverDistance = 2.0f;
        map.build();

        PcssParams p;
        p.mode = 2;
        p.sunTanRadius = 0.05f;
        p.searchRadius = 0.3f;
        p.blockerSamples = 16;
        p.pcfSamples = 32;
        p.receiverSlope = 0.0f;

        // `facing` is the shader's sqrt(1 - (N.L)^2): 1 for a surface turned towards the sun, 0 edge-on. The header
        // carries the complement, receiverSlope, so a facing receiver is receiverSlope 0 and facing 1.
        auto shaderVisibility = [&](float x, float facing) {
            const float scale = map.uvPerMetre() * std::max(facing, 1e-3f);
            const float zReceiver = toZ(map.receiverDistance);
            const float search = p.searchRadius * scale;
            float blocker = 0.0f;
            int found = 0;
            for (int i = 0; i < 16; ++i) {
                const glm::vec2 off = vogelTap(i, 16) * search;
                const float d = mapDepth(&map, glm::vec2(map.uvForX(x) + off.x, 0.5f + off.y));
                const float slack = 0.0008f + facing * (glm::length(off) / scale) * zPerMetreScene;
                if (d > 0.0f && toZ(d) < zReceiver - slack) {
                    blocker += toZ(d);
                    ++found;
                }
            }
            if (found == 0) return 1.0f;
            const float gap = std::max(zReceiver - blocker / float(found), 0.0f) / zPerMetreScene;
            const float width = std::min(2.0f * p.sunTanRadius * gap, p.maxPenumbra) * scale;
            int lit = 0;
            for (int i = 0; i < 32; ++i) {
                const glm::vec2 off = vogelTap(i, 32) * width;
                const float d = mapDepth(&map, glm::vec2(map.uvForX(x) + off.x, 0.5f + off.y));
                const float slack = 0.0008f + facing * (glm::length(off) / scale) * zPerMetreScene;
                if (d <= 0.0f || toZ(d) >= zReceiver - slack) ++lit;
            }
            return float(lit) / 32.0f;
        };
        float worst = 0.0f;
        for (float x = -0.5f; x <= 0.5f; x += 0.05f)
            worst = std::max(worst, std::fabs(shaderVisibility(x, 1.0f) - visibilityAt(map, x, p)));
        checkLess(worst, 1e-4f, "8d: the shader's z-space arithmetic gives the header's answer");
    }

    std::printf("pcss: %d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
