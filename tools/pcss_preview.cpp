// Offline PCSS preview (docs/PCSS.md): one scene under a hard shadow map, a fixed-radius PCF and percentage-closer soft
// shadows, all measured against a 64-ray ground truth.
//
// The scene is a slab, a crate, a thin plank and a low block on a floor, lit by a sun 3.4 degrees wide. At that angle the
// shadow edge is a hand's width of penumbra by the time it reaches the floor, and narrower where the caster touches it --
// the gradient a fixed filter cannot produce. Every pixel is shaded three ways from the same 1024-texel shadow map
// (9.8 mm per texel, finer than the effect needs, so the difference in the picture is the filter and not the map), and a
// fourth time by 64 rays over the sun's disc.
//
// Build (from the repository root):
//     c++ -O2 -std=c++17 -I src -I third_party -I third_party/glm tools/pcss_preview.cpp -o build/pcss_preview
// Run:
//     ./build/pcss_preview docs/pcss.png 1600 1000
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/pcss.h"
#include "figure_kit.h"

using namespace ukiyo;

namespace {

// ---- the scene ----------------------------------------------------------------------------------------------

struct Box {
    glm::vec3 lo, hi;
    glm::vec3 albedo;
};

// Spread along z so that each object's shadow falls on open floor: a shadow map cannot show a penumbra that another
// shadow is sitting on.
const Box kBoxes[] = {
    {{-0.60f, 0.00f, -3.20f}, {0.60f, 2.20f, -2.00f}, {0.62f, 0.60f, 0.56f}},   // the slab: a 2.2 m drop
    {{-0.40f, 0.00f, -0.60f}, {0.40f, 0.60f, 0.20f}, {0.72f, 0.47f, 0.34f}},    // a crate: 0.6 m
    {{1.20f, 0.00f, 1.40f}, {1.34f, 1.60f, 2.40f}, {0.42f, 0.52f, 0.60f}},      // a plank thinner than its penumbra
    {{-2.40f, 0.00f, 1.60f}, {-1.90f, 0.35f, 2.10f}, {0.52f, 0.58f, 0.40f}},    // a low block
};

const glm::vec3 kSun = glm::normalize(glm::vec3(-0.62f, 0.52f, 0.24f));   // towards the sun
// Distance from the light along the light, which is what a shadow map stores: it must grow away from the light, so it is
// a constant minus the projection onto the direction towards it. Without the constant the floor is "nearer" than the
// boxes standing on it and nothing casts anything.
constexpr float kLightDepthOffset = 16.0f;
inline float lightDepth(const glm::vec3& p) { return kLightDepthOffset - glm::dot(p, kSun); }
constexpr float kSunTanRadius = 0.06f;                                    // ~3.4 degrees: a stylised sun
constexpr int kMapSize = 1024;

struct Hit {
    bool hit = false;
    float t = 1e30f;
    glm::vec3 p{0.0f};
    glm::vec3 n{0.0f};
    glm::vec3 albedo{0.5f};
};

bool slabTest(const Box& b, const glm::vec3& o, const glm::vec3& d, float* tNear, float* tFar) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < b.lo[i] || o[i] > b.hi[i]) return false;
            continue;
        }
        float a = (b.lo[i] - o[i]) / d[i], c = (b.hi[i] - o[i]) / d[i];
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a);
        t1 = std::min(t1, c);
        if (t0 > t1) return false;
    }
    *tNear = t0;
    *tFar = t1;
    return true;
}

glm::vec3 boxNormal(const Box& b, const glm::vec3& p) {
    const float e = 1e-3f;
    if (std::fabs(p.x - b.hi.x) < e) return {1, 0, 0};
    if (std::fabs(p.x - b.lo.x) < e) return {-1, 0, 0};
    if (std::fabs(p.z - b.hi.z) < e) return {0, 0, 1};
    if (std::fabs(p.z - b.lo.z) < e) return {0, 0, -1};
    if (std::fabs(p.y - b.hi.y) < e) return {0, 1, 0};
    return {0, -1, 0};
}

Hit trace(const glm::vec3& o, const glm::vec3& d) {
    Hit h;
    if (std::fabs(d.y) > 1e-6f) {
        const float t = -o.y / d.y;
        if (t > 1e-4f && t < h.t) {
            h.hit = true;
            h.t = t;
            h.p = o + d * t;
            h.n = {0, 1, 0};
            h.albedo = {0.55f, 0.55f, 0.58f};
        }
    }
    for (const Box& b : kBoxes) {
        float t0 = 0.0f, t1 = 0.0f;
        if (!slabTest(b, o, d, &t0, &t1)) continue;
        const float t = t0 > 1e-4f ? t0 : (t1 > 1e-4f ? t1 : -1.0f);
        if (t > 1e-4f && t < h.t) {
            h.hit = true;
            h.t = t;
            h.p = o + d * t;
            h.n = boxNormal(b, h.p);
            h.albedo = b.albedo;
        }
    }
    return h;
}

bool occluded(const glm::vec3& o, const glm::vec3& d, float maxT) {
    if (d.y < -1e-6f) {
        const float t = -o.y / d.y;
        if (t > 1e-4f && t < maxT) return true;
    }
    for (const Box& b : kBoxes) {
        float t0 = 0.0f, t1 = 0.0f;
        if (slabTest(b, o, d, &t0, &t1) && t1 > 1e-4f && t0 < maxT) return true;
    }
    return false;
}

// ---- the shadow map -----------------------------------------------------------------------------------------

struct LightFrame {
    glm::vec3 right, up;
    float minU = 0.0f, minV = 0.0f, sizeU = 1.0f, sizeV = 1.0f;

    glm::vec2 uvOf(const glm::vec3& p, float* depth) const {
        *depth = lightDepth(p);
        return glm::vec2((glm::dot(p, right) - minU) / sizeU, (glm::dot(p, up) - minV) / sizeV);
    }
};

struct ShadowMap {
    LightFrame frame;
    std::vector<float> depth;

    void build() {
        frame.right = glm::normalize(glm::cross(kSun, glm::vec3(0.0f, 1.0f, 0.0f)));
        frame.up = glm::normalize(glm::cross(frame.right, kSun));

        const glm::vec3 lo(-3.6f, 0.0f, -4.4f), hi(3.4f, 2.4f, 3.4f);
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            minU = std::min(minU, glm::dot(c, frame.right));
            maxU = std::max(maxU, glm::dot(c, frame.right));
            minV = std::min(minV, glm::dot(c, frame.up));
            maxV = std::max(maxV, glm::dot(c, frame.up));
        }
        const float side = std::max(maxU - minU, maxV - minV) * 1.02f;
        const float cu = 0.5f * (minU + maxU), cv = 0.5f * (minV + maxV);
        frame.minU = cu - 0.5f * side;
        frame.minV = cv - 0.5f * side;
        frame.sizeU = frame.sizeV = side;

        depth.assign(size_t(kMapSize) * size_t(kMapSize), 0.0f);
        const float step = side / float(kMapSize);
        for (int y = 0; y < kMapSize; ++y) {
            for (int x = 0; x < kMapSize; ++x) {
                const float u = frame.minU + (float(x) + 0.5f) * step;
                const float v = frame.minV + (float(y) + 0.5f) * step;
                const glm::vec3 origin = frame.right * u + frame.up * v + kSun * 12.0f;
                const Hit h = trace(origin, -kSun);
                depth[size_t(y) * size_t(kMapSize) + size_t(x)] = h.hit ? lightDepth(h.p) : 0.0f;
            }
        }
    }

    float texelMetres() const { return frame.sizeU / float(kMapSize); }
    float uvPerMetre() const { return 1.0f / frame.sizeU; }

    float at(glm::vec2 uv) const {
        if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) return 0.0f;
        const int x = std::min(int(uv.x * float(kMapSize)), kMapSize - 1);
        const int y = std::min(int(uv.y * float(kMapSize)), kMapSize - 1);
        return depth[size_t(y) * size_t(kMapSize) + size_t(x)];
    }
};

float mapDepth(const void* map, glm::vec2 uv) { return static_cast<const ShadowMap*>(map)->at(uv); }

float groundTruth(const glm::vec3& p, const glm::vec3& n, const ShadowMap& map, int samples = 64) {
    int lit = 0;
    for (int i = 0; i < samples; ++i) {
        const glm::vec2 t = vogelTap(i, samples);
        const glm::vec3 dir = glm::normalize(kSun + (map.frame.right * t.x + map.frame.up * t.y) * kSunTanRadius);
        if (glm::dot(dir, n) <= 0.0f) continue;
        if (!occluded(p + n * 1e-3f, dir, 40.0f)) ++lit;
    }
    return float(lit) / float(samples);
}

// ---- rendering ----------------------------------------------------------------------------------------------

struct Frame {
    int w = 0, h = 0;
    std::vector<glm::vec3> color;
    std::vector<float> vis;
};

glm::vec3 shade(const glm::vec3& albedo, const glm::vec3& n, float vis) {
    const float ndl = std::max(glm::dot(n, kSun), 0.0f);
    const glm::vec3 sun(1.05f, 0.98f, 0.90f), sky(0.28f, 0.33f, 0.42f), ground(0.16f, 0.14f, 0.12f);
    const glm::vec3 ambient = glm::mix(ground, sky, n.y * 0.5f + 0.5f) * 0.55f;
    return albedo * (ambient + sun * (ndl * vis));
}

// mode 0 = ground truth, 1 = hard, 2 = pcf, 3 = pcss.
Frame render(const ShadowMap& map, int mode, const glm::mat4& viewProj, int w, int h, PcssParams params) {
    Frame out;
    out.w = w;
    out.h = h;
    out.color.assign(size_t(w) * size_t(h), glm::vec3(0.0f));
    out.vis.assign(size_t(w) * size_t(h), 1.0f);
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const glm::vec2 ndc((float(x) + 0.5f) / float(w) * 2.0f - 1.0f,
                                1.0f - (float(y) + 0.5f) / float(h) * 2.0f);
            const glm::vec4 nearP = invViewProj * glm::vec4(ndc, 0.0f, 1.0f);
            const glm::vec4 farP = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);
            const glm::vec3 o = glm::vec3(nearP) / nearP.w;
            const glm::vec3 d = glm::normalize(glm::vec3(farP) / farP.w - o);
            const Hit hit = trace(o, d);
            const size_t at = size_t(y) * size_t(w) + size_t(x);
            if (!hit.hit) {
                out.color[at] = glm::vec3(0.08f, 0.10f, 0.14f);
                continue;
            }
            float depth = 0.0f;
            const glm::vec2 uv = map.frame.uvOf(hit.p, &depth);
            PcssParams local = params;
            // The receiver's own surface, as the light sees it: 0 facing the sun, 1 edge-on.
            local.receiverSlope = glm::length(kSun - glm::dot(kSun, hit.n) * hit.n);
            const float vis = mode == 0 ? groundTruth(hit.p, hit.n, map)
                                        : pcssVisibility(uv, depth, &map, mapDepth, map.uvPerMetre(), local);
            out.vis[at] = vis;
            out.color[at] = shade(hit.albedo, hit.n, vis);
        }
    }
    return out;
}

// ---- the profile plot ---------------------------------------------------------------------------------------

struct Curve {
    glm::vec3 color;
    std::vector<glm::vec2> points;   // xy in [0,1] plot space
};

void drawPlot(figure::Image& sheet, int x0, int y0, int w, int h, const std::vector<Curve>& curves,
              const std::string& title, const std::string& note, const std::string& xLabel) {
    figure::fillRect(sheet, x0, y0, w, h, glm::vec3(0.03f, 0.035f, 0.045f));
    figure::panelHeader(sheet, x0 + 10, y0 + 8, title, note);
    const int plotX = x0 + 60, plotY = y0 + 62, plotW = w - 86, plotH = h - 132;
    figure::fillRect(sheet, plotX, plotY, plotW, plotH, glm::vec3(0.06f, 0.07f, 0.09f));
    // Grid: 0, 0.25, 0.5, 0.75, 1 on the visibility axis.
    for (int i = 0; i <= 4; ++i) {
        const int gy = plotY + plotH - (plotH * i) / 4;
        figure::fillRect(sheet, plotX, gy, plotW, 1, glm::vec3(0.16f, 0.18f, 0.22f));
        char label[8];
        std::snprintf(label, sizeof label, "%d", i * 25);
        figure::drawText(sheet, plotX - (i == 4 ? 26 : 20), gy - 3, label, 1, glm::vec3(0.45f, 0.50f, 0.58f));
    }
    for (const Curve& c : curves) {
        for (size_t i = 1; i < c.points.size(); ++i) {
            const glm::vec2 a = c.points[i - 1], b = c.points[i];
            const int xa = plotX + int(a.x * plotW), ya = plotY + plotH - int(a.y * plotH);
            const int xb = plotX + int(b.x * plotW), yb = plotY + plotH - int(b.y * plotH);
            const int steps = std::max(std::abs(xb - xa), std::abs(yb - ya)) + 1;
            for (int s = 0; s <= steps; ++s) {
                const float t = float(s) / float(steps);
                sheet.set(xa + int(t * float(xb - xa)), ya + int(t * float(yb - ya)), c.color);
            }
        }
    }
    if (!xLabel.empty()) figure::drawText(sheet, plotX, plotY + plotH + 14, xLabel, 1, glm::vec3(0.55f, 0.60f, 0.68f));
    // Legend, under the plot: the curve colours are the only way to read it.
    int lx = plotX;
    const char* legend[4] = {"GROUND TRUTH", "PCSS", "FIXED PCF", "HARD"};
    const glm::vec3 colors[4] = {{0.95f, 0.95f, 0.90f}, {0.30f, 0.85f, 0.95f}, {0.95f, 0.80f, 0.30f}, {0.95f, 0.35f, 0.35f}};
    for (int i = 0; i < 4; ++i) {
        figure::fillRect(sheet, lx, plotY + plotH + 30, 16, 4, colors[i]);
        figure::drawText(sheet, lx + 22, plotY + plotH + 28, legend[i], 1, glm::vec3(0.62f, 0.68f, 0.76f));
        lx += 24 + figure::textWidth(legend[i], 1) + 22;
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "docs/pcss.png";
    const int outW = argc > 2 ? std::atoi(argv[2]) : 1600;
    const int outH = argc > 3 ? std::atoi(argv[3]) : 1000;

    ShadowMap map;
    map.build();

    const glm::vec3 eye(3.10f, 2.85f, 5.55f);
    const glm::vec3 target(0.10f, 0.42f, -0.85f);
    const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));

    PcssParams hard;
    hard.mode = 0;
    PcssParams pcf;
    pcf.mode = 1;
    pcf.fixedRadius = 0.15f;   // replaced below by the swept best; the sweep starts here and keeps its winner
    pcf.pcfSamples = 48;       // the same number of taps PCSS uses (16 blockers + 32 filter), so the comparison is fair
    PcssParams pcss;
    pcss.mode = 2;
    pcss.sunTanRadius = kSunTanRadius;
    pcss.blockerSamples = 16;
    pcss.pcfSamples = 32;
    pcss.searchRadius = 0.25f;
    pcss.maxPenumbra = 0.5f;

    // Each of the four cells holds one panel, inset by the same margin, so the panels tile the sheet.
    const int cellW = outW / 2, cellH = outH / 2;
    const int panelW = cellW - 44;
    const int panelH = cellH - 44;
    const float aspect = float(panelW) / float(panelH);
    const glm::mat4 proj = glm::perspective(glm::radians(42.0f), aspect, 0.1f, 60.0f);
    const glm::mat4 viewProj = proj * view;

    const Frame gt = render(map, 0, viewProj, panelW, panelH, pcss);

    // Mean absolute error over the pixels the ground truth puts in shadow or penumbra: the interior of a shadow is 0 for
    // every filter, so the only number that means anything is the one over the penumbra, where the truth is partly lit.
    // That is the surface the whole technique is about.
    auto penumbraError = [&](const Frame& f) {
        double sum = 0.0;
        int n = 0;
        for (size_t i = 0; i < gt.vis.size(); ++i) {
            if (gt.vis[i] > 0.95f || gt.vis[i] < 0.05f) continue;
            sum += std::fabs(double(f.vis[i]) - double(gt.vis[i]));
            ++n;
        }
        return n > 0 ? sum / double(n) : 0.0;
    };

    // The fixed filter's one free parameter, chosen the way a game cannot: by trying all of them and keeping the best.
    // That is the honest bar for PCSS to clear -- it has no such parameter, and it still has to win against the radius a
    // hand-tuned scene would have landed on.
    struct FixedCost {
        float radius = 0.0f;
        double error = 1e9;
    } bestPcf;
    std::printf("fixed-radius PCF, %d taps, swept (the radius a hand-tuned scene picks):\n", pcf.pcfSamples);
    for (float radius : {0.04f, 0.06f, 0.08f, 0.10f, 0.14f, 0.18f, 0.22f, 0.28f, 0.36f}) {
        PcssParams trial = pcf;
        trial.fixedRadius = radius;
        const double e = penumbraError(render(map, 2, viewProj, panelW, panelH, trial));
        std::printf("  radius %.2f m  error %.4f\n", double(radius), e);
        if (e < bestPcf.error) bestPcf = {radius, e};
    }
    pcf.fixedRadius = bestPcf.radius;

    const Frame hardF = render(map, 1, viewProj, panelW, panelH, hard);
    const Frame pcfF = render(map, 2, viewProj, panelW, panelH, pcf);
    const Frame pcssF = render(map, 3, viewProj, panelW, panelH, pcss);

    const double errHard = penumbraError(hardF);
    const double errPcf = penumbraError(pcfF);
    const double errPcss = penumbraError(pcssF);
    int counted = 0;
    for (float v : gt.vis)
        if (v <= 0.95f && v >= 0.05f) ++counted;
    std::printf("penumbra pixels %d of %d\n", counted, int(gt.vis.size()));
    std::printf("mean |error| in the penumbra vs 64-ray truth: hard %.4f  pcf %.4f  pcss %.4f\n", errHard, errPcf, errPcss);
    std::printf("best fixed radius %.2f m; pcss uses %d + %d taps, pcf %d\n", double(bestPcf.radius),
                pcss.blockerSamples, pcss.pcfSamples, pcf.pcfSamples);
    std::printf("shadow map: %d texels over %.2f m = %.1f mm per texel\n", kMapSize, double(map.frame.sizeU),
                double(map.texelMetres() * 1000.0f));

    figure::Image sheet;
    sheet.reset(outW, outH, glm::vec3(0.05f, 0.055f, 0.065f));

    char pcfName[64], pcfNote[96], pcssName[64];
    std::snprintf(pcfName, sizeof pcfName, "PCF AT ITS BEST FIXED RADIUS, %.0f CM", double(pcf.fixedRadius * 100.0f));
    std::snprintf(pcfNote, sizeof pcfNote, "THE RADIUS SWEPT AND HANDED TO IT; %d TAPS", pcf.pcfSamples);
    std::snprintf(pcssName, sizeof pcssName, "PCSS: %d BLOCKERS + %d TAPS", pcss.blockerSamples, pcss.pcfSamples);
    const char* names[3] = {"HARD SHADOW MAP", pcfName, pcssName};
    const char* notes[3] = {"ONE EDGE WIDTH FOR EVERY LIGHT AND EVERY GAP", pcfNote,
                            "WIDTH FROM THE LIGHT, THE GAP AND THE BLOCKER"};
    const Frame* frames[3] = {&hardF, &pcfF, &pcssF};
    for (int i = 0; i < 3; ++i) {
        const int cellX = (i % 2) * cellW + 22;
        const int cellY = (i / 2) * cellH + 22;
        for (int y = 0; y < panelH; ++y)
            for (int x = 0; x < panelW; ++x)
                sheet.set(cellX + x, cellY + y, figure::toSrgb(frames[i]->color[size_t(y) * size_t(panelW) + size_t(x)]));
        figure::panelHeader(sheet, cellX + 12, cellY + 12, names[i], notes[i]);
    }

    // Fourth cell: the ground truth as a thumbnail, the numbers, and the profile across the crate's shadow edge.
    {
        const int cellX = cellW + 22, cellY = cellH + 22;
        const int thumbH = std::min(panelH / 3, 138);
        const int thumbW = int(float(thumbH) * aspect);
        const int thumbY = cellY + 74;
        for (int y = 0; y < thumbH; ++y)
            for (int x = 0; x < thumbW; ++x) {
                const int sx = int((float(x) + 0.5f) / float(thumbW) * float(panelW));
                const int sy = int((float(y) + 0.5f) / float(thumbH) * float(panelH));
                sheet.set(cellX + 10 + x, thumbY + y, figure::toSrgb(gt.color[size_t(sy) * size_t(panelW) + size_t(sx)]));
            }
        figure::panelHeader(sheet, cellX + 12, cellY + 12, "GROUND TRUTH", "64 RAYS OVER THE SUN'S DISC");
        // The strip between the header and the thumbnail carries the caption; the numbers go beside the thumbnail.

        char line[128];
        const int textX = cellX + thumbW + 30;
        int ty = cellY + 76;
        std::snprintf(line, sizeof line, "MEAN ERROR IN THE PENUMBRA");
        figure::drawText(sheet, textX, ty, line, 1, glm::vec3(0.85f, 0.88f, 0.93f));
        ty += 16;
        std::snprintf(line, sizeof line, "OF %d PIXELS", counted);
        figure::drawText(sheet, textX, ty, line, 1, glm::vec3(0.62f, 0.68f, 0.76f));
        ty += 22;
        std::snprintf(line, sizeof line, "HARD  %.3f", errHard);
        figure::drawText(sheet, textX, ty, line, 2, glm::vec3(0.95f, 0.45f, 0.42f));
        ty += 30;
        std::snprintf(line, sizeof line, "PCF   %.3f", errPcf);
        figure::drawText(sheet, textX, ty, line, 2, glm::vec3(0.95f, 0.80f, 0.35f));
        ty += 30;
        std::snprintf(line, sizeof line, "PCSS  %.3f", errPcss);
        figure::drawText(sheet, textX, ty, line, 2, glm::vec3(0.35f, 0.85f, 0.95f));
        // The profile: visibility along a floor line crossing the crate's shadow edge, 0.6 m below the crate's top.
        const float zLine = 0.40f - 0.6f * kSun.z / kSun.y;   // the crate's top edge throws its shadow here
        std::vector<glm::vec2> gtPts, pcssPts, pcfPts, hardPts;
        const float x0 = 0.30f, x1 = 1.45f;
        for (int i = 0; i <= 160; ++i) {
            const float x = x0 + (x1 - x0) * float(i) / 160.0f;
            const glm::vec3 p(x, 0.001f, zLine);
            const glm::vec3 n(0, 1, 0);
            float depth = 0.0f;
            const glm::vec2 uv = map.frame.uvOf(p, &depth);
            const float g = groundTruth(p, n, map, 64);
            PcssParams localHard = hard, localPcf = pcf, localPcss = pcss;
            localHard.receiverSlope = localPcf.receiverSlope = localPcss.receiverSlope =
                glm::length(kSun - glm::dot(kSun, n) * n);
            const float hv = pcssVisibility(uv, depth, &map, mapDepth, map.uvPerMetre(), localHard);
            const float fv = pcssVisibility(uv, depth, &map, mapDepth, map.uvPerMetre(), localPcf);
            const float sv = pcssVisibility(uv, depth, &map, mapDepth, map.uvPerMetre(), localPcss);
            const float t = float(i) / 160.0f;
            gtPts.push_back({t, g});
            pcssPts.push_back({t, sv});
            pcfPts.push_back({t, fv});
            hardPts.push_back({t, hv});
        }
        std::snprintf(line, sizeof line, "MAP %d TEXELS, %.0f MM EACH", kMapSize, double(map.texelMetres() * 1000.0f));
        figure::drawText(sheet, cellX + 10, cellY + cellH - 34, line, 1, glm::vec3(0.50f, 0.55f, 0.63f));

        const int plotX = cellX + 4, plotY = thumbY + thumbH + 20;
        const int plotW = panelW, plotH = cellY + cellH - 12 - plotY;
        // Where the shadow edge is: the first sample the truth calls half lit, to the nearest centimetre.
        float xEdge = x0;
        bool solved = false;
        for (size_t i = 1; i < gtPts.size() && !solved; ++i) {
            if (gtPts[i - 1].y <= 0.5f && gtPts[i].y > 0.5f) {
                const float f = (0.5f - gtPts[i - 1].y) / (gtPts[i].y - gtPts[i - 1].y);
                xEdge = x0 + (x1 - x0) * (float(i - 1) + f) / 160.0f;
                solved = true;
            }
        }
        if (solved)
            std::snprintf(line, sizeof line, "0.30 M .. 1.45 M ALONG X, SHADOW EDGE AT %.2f M", double(xEdge));
        else
            std::snprintf(line, sizeof line, "0.30 M .. 1.45 M ALONG X");
        drawPlot(sheet, plotX, plotY, plotW, plotH,
                 {{glm::vec3(0.95f, 0.95f, 0.90f), gtPts},
                  {glm::vec3(0.30f, 0.85f, 0.95f), pcssPts},
                  {glm::vec3(0.95f, 0.80f, 0.30f), pcfPts},
                  {glm::vec3(0.95f, 0.35f, 0.35f), hardPts}},
                 "VISIBILITY ACROSS THE CRATE'S EDGE", "0.6 M DROP ONTO THE FLOOR, PERCENT LIT",
                 line);
    }

    return figure::savePng(sheet, out) ? 0 : 1;
}
