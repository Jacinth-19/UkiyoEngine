// Motion-matching demo (item 20, docs/MOTION.md): drives the controller through a scripted route and writes a
// figure of what came out — the path it walked, its footprints, and four poses along the way.
//
// The clips here are synthetic locomotion cycles rather than mocap, so the tool needs no capture data and runs in CI.
// The database, the KD-tree, the matcher and the controller are the engine's own; swap `makeClip` for
// `loadMotionBvhFile` over a LAFAN1 folder and the rest is unchanged.
//
//     motion_demo docs/motion-demo.png
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <glm/gtc/quaternion.hpp>

#include "engine/motion_match.h"

using namespace ukiyo;

namespace {

constexpr float kPi = 3.14159265f;

// A locomotion cycle: root travels along `dir` at `speed`, feet swing past each other, hands counter-swing.
// `stride` is the cycle rate in Hz. The pose is deliberately simple — six joints — because what is being tested is
// the matcher, not the artistry.
MotionClip makeClip(const char* name, float speed, glm::vec2 dir, float stride, int frames = 180, float fps = 30.0f,
                    bool idle = false) {
    MotionClip clip;
    clip.name = name;
    clip.fps = fps;
    clip.joints = {{"Hips", -1, glm::vec3(0.0f)},
                   {"LeftFoot", 0, glm::vec3(0.12f, -0.95f, 0.0f)},
                   {"RightFoot", 0, glm::vec3(-0.12f, -0.95f, 0.0f)},
                   {"LeftHand", 0, glm::vec3(0.22f, 0.10f, 0.0f)},
                   {"RightHand", 0, glm::vec3(-0.22f, 0.10f, 0.0f)},
                   {"Head", 0, glm::vec3(0.0f, 0.62f, 0.0f)}};
    const glm::vec2 d = glm::length(dir) > 1e-5f ? glm::normalize(dir) : glm::vec2(0.0f, 1.0f);
    const glm::vec2 side(-d.y, d.x);
    clip.frames.resize(size_t(frames));
    for (int i = 0; i < frames; ++i) {
        const float t = float(i) / fps;
        MotionFrame& f = clip.frames[size_t(i)];
        f.rootRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        f.rootPos = glm::vec3(d.x * speed * t, 0.0f, d.y * speed * t);
        const float phase = t * 2.0f * kPi * stride;
        const float swingA = std::sin(phase), swingB = std::sin(phase + kPi);
        const float liftA = std::max(0.0f, std::sin(phase + kPi * 0.5f));
        const float liftB = std::max(0.0f, std::sin(phase + kPi * 1.5f));
        const float amp = idle ? 0.02f : 0.34f;
        const float lift = idle ? 0.01f : 0.10f;
        const glm::vec3 s(d.x, 0.0f, d.y);          // forward
        const glm::vec3 c(side.x, 0.0f, side.y);    // lateral
        f.world = {
            f.rootPos,
            f.rootPos + c * 0.13f + s * (amp * swingA) + glm::vec3(0.0f, 0.06f + lift * liftA, 0.0f),
            f.rootPos - c * 0.13f + s * (amp * swingB) + glm::vec3(0.0f, 0.06f + lift * liftB, 0.0f),
            f.rootPos + c * 0.24f - s * (amp * 0.8f * swingA) + glm::vec3(0.0f, 1.02f, 0.0f),
            f.rootPos - c * 0.24f - s * (amp * 0.8f * swingB) + glm::vec3(0.0f, 1.02f, 0.0f),
            f.rootPos + glm::vec3(0.0f, 1.62f, 0.0f),
        };
    }
    return clip;
}

// --- A very small software rasteriser, enough for a figure: lines, discs, and a 2x supersample. -------------------

struct Canvas {
    int w = 0, h = 0;
    std::vector<unsigned char> px;   // RGB
    Canvas(int width, int height) : w(width), h(height), px(size_t(width) * size_t(height) * 3, 0) {}
    void clear(glm::vec3 c) {
        for (size_t i = 0; i < px.size(); i += 3) {
            px[i + 0] = (unsigned char)(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            px[i + 1] = (unsigned char)(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            px[i + 2] = (unsigned char)(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        }
    }
    void put(int x, int y, glm::vec3 c, float a = 1.0f) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        unsigned char* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 3];
        for (int k = 0; k < 3; ++k) {
            const float v = (&c.r)[k] * 255.0f;
            p[k] = (unsigned char)(p[k] * (1.0f - a) + v * a);
        }
    }
    void disc(float cx, float cy, float r, glm::vec3 c) {
        const int x0 = int(std::floor(cx - r)), x1 = int(std::ceil(cx + r));
        const int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float a = glm::clamp(r - d + 0.5f, 0.0f, 1.0f);
                if (a > 0.0f) put(x, y, c, a);
            }
    }
    void line(glm::vec2 a, glm::vec2 b, float width, glm::vec3 c) {
        const float len = glm::length(b - a);
        const int steps = std::max(1, int(len * 2.0f));
        for (int i = 0; i <= steps; ++i) {
            const glm::vec2 p = glm::mix(a, b, float(i) / float(steps));
            disc(p.x, p.y, width, c);
        }
    }
};

void downsample(const Canvas& src, Canvas* dst) {
    for (int y = 0; y < dst->h; ++y)
        for (int x = 0; x < dst->w; ++x) {
            int acc[3] = {0, 0, 0};
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    const unsigned char* p = &src.px[(size_t(y * 2 + sy) * size_t(src.w) + size_t(x * 2 + sx)) * 3];
                    for (int k = 0; k < 3; ++k) acc[k] += p[k];
                }
            unsigned char* o = &dst->px[(size_t(y) * size_t(dst->w) + size_t(x)) * 3];
            for (int k = 0; k < 3; ++k) o[k] = (unsigned char)(acc[k] / 4);
        }
}

struct Snapshot {
    std::vector<glm::vec3> pose;
    float yaw = 0.0f;
    float time = 0.0f;
};

} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "motion-demo.png";

    std::vector<MotionClip> clips;
    clips.push_back(makeClip("walk", 1.4f, glm::vec2(0.0f, 1.0f), 1.55f));
    clips.push_back(makeClip("run", 5.0f, glm::vec2(0.0f, 1.0f), 1.30f));
    clips.push_back(makeClip("strafe-left", 1.3f, glm::vec2(-1.0f, 0.0f), 1.45f));
    clips.push_back(makeClip("strafe-right", 1.3f, glm::vec2(1.0f, 0.0f), 1.45f));
    clips.push_back(makeClip("idle", 0.0f, glm::vec2(0.0f, 1.0f), 0.35f, 180, 30.0f, true));

    MotionDatabase database;
    std::string error;
    if (!database.build(clips, MotionSettings{}, &error)) {
        std::fprintf(stderr, "database: %s\n", error.c_str());
        return 1;
    }

    MotionController controller;
    controller.setDatabase(&database);
    controller.reset(glm::vec3(0.0f), 0.0f);

    const float dt = 1.0f / 30.0f;
    std::vector<glm::vec2> path;
    std::vector<glm::vec2> footprints;
    std::vector<Snapshot> snapshots;
    float t = 0.0f;
    float walked = 0.0f;
    float peak = 0.0f;
    int matchesAtStart = 0;

    glm::vec3 last = controller.position();
    for (int step = 0; step < 300; ++step, t += dt) {
        glm::vec3 dir(0.0f);
        float speed = 0.0f;
        if (t < 2.0f) {
            dir = glm::vec3(0.0f, 0.0f, 1.0f);
            speed = 1.4f;
        } else if (t < 4.2f) {
            // A left-hand curve: the wish direction rotates while the body follows it.
            const float a = (t - 2.0f) * 1.9f;
            dir = glm::vec3(-std::sin(a), 0.0f, std::cos(a));
            speed = 1.4f;
        } else if (t < 6.6f) {
            dir = glm::vec3(-0.42f, 0.0f, 1.0f);
            speed = 5.0f;
        } else if (t < 8.4f) {
            dir = glm::vec3(-0.42f, 0.0f, 1.0f);
            speed = 0.0f;
        }
        const float facing = controller.yaw() + 0.0f;
        if (matchesAtStart == 0 && controller.player().matchCount() > 0) matchesAtStart = step;
        controller.update(dir, speed, dt, speed > 0.05f ? std::atan2(dir.x, dir.z) : facing);

        const glm::vec3 p = controller.position();
        walked += glm::length(p - last);
        last = p;
        peak = std::max(peak, controller.speed());
        if (step % 3 == 0) path.emplace_back(p.x, p.z);
        if (step % 2 == 0 && controller.pose().size() >= 3) {
            if (controller.pose()[1].y < p.y + 0.12f) footprints.emplace_back(controller.pose()[1].x, controller.pose()[1].z);
            if (controller.pose()[2].y < p.y + 0.12f) footprints.emplace_back(controller.pose()[2].x, controller.pose()[2].z);
        }
        // Four snapshots spread over the route; the offsets put them on different halves of the stride, so the
        // figure shows the walk and the run at different phases rather than four copies of the same pose.
        if ((step == 30 || step == 96 || step == 168 || step == 246) && controller.pose().size() >= 6)
            snapshots.push_back({controller.pose(), controller.yaw(), t});
    }

    const float finalSpeed = controller.speed();
    const int matches = controller.player().matchCount();

    // --- The figure: a top-down map of the route on the left, four poses on the right. ---------------------------
    Canvas big(1440, 720);
    big.clear(glm::vec3(0.953f, 0.933f, 0.894f));   // paper
    const glm::vec3 ink(0.102f, 0.157f, 0.267f);   // indigo
    const glm::vec3 vermilion(0.800f, 0.282f, 0.196f);
    const glm::vec3 hair(0.760f, 0.722f, 0.651f);

    // Left panel: the route fitted to the panel with a margin, +z up the page.
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const glm::vec2& p : path) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec2 span = glm::max(hi - lo, glm::vec2(0.5f));
    const float scale = std::min(520.0f / span.x, 470.0f / span.y);
    const glm::vec2 origin(360.0f - 0.5f * (lo.x + hi.x) * scale, 380.0f + 0.5f * (lo.y + hi.y) * scale);
    auto map2 = [&](glm::vec2 p) { return origin + glm::vec2(p.x, -p.y) * scale; };
    for (int g = -6; g <= 6; ++g) {
        const float y = 380.0f + g * scale;
        if (y > 90.0f && y < 690.0f) big.line(glm::vec2(50.0f, y), glm::vec2(670.0f, y), 0.6f, hair);
        const float x = 360.0f + g * scale;
        if (x > 50.0f && x < 670.0f) big.line(glm::vec2(x, 90.0f), glm::vec2(x, 690.0f), 0.6f, hair);
    }
    for (size_t i = 1; i < path.size(); ++i) big.line(map2(path[i - 1]), map2(path[i]), 2.4f, ink);
    for (const glm::vec2& f : footprints) {
        const glm::vec2 p = map2(f);
        big.disc(p.x, p.y, 2.8f, vermilion);
    }

    // Right panel: four poses, drawn side on. Joints are 0 hips, 1 left foot, 2 right foot, 3 left hand, 4 right hand,
    // 5 head; the projection is the character's own forward axis, so each pose reads the same whatever its heading.
    const char* labels[4] = {"walk", "curve", "run", "stop"};
    (void)labels;
    for (size_t s = 0; s < snapshots.size() && s < 4; ++s) {
        const std::vector<glm::vec3>& pose = snapshots[s].pose;
        const float cx = 880.0f + float(s % 2) * 250.0f;
        const float cy = 250.0f + float(s / 2) * 330.0f;
        const glm::vec3 root = pose[0];
        // Project along the character's own forward axis at that moment, so the drawing shows the stride; drawing x
        // would show only the width of the stance and every pose would look identical.
        const glm::vec3 forward(std::sin(snapshots[s].yaw), 0.0f, std::cos(snapshots[s].yaw));
        auto toScreen = [&](glm::vec3 p) {
            const glm::vec3 d = p - root;
            return glm::vec2(cx + glm::dot(d, forward) * 110.0f, cy - d.y * 110.0f);
        };
        const glm::vec2 hip = toScreen(pose[0]);
        // A ground line under the figure, so a foot that lifts reads as lifted.
        big.line(glm::vec2(cx - 80.0f, cy + 6.0f), glm::vec2(cx + 80.0f, cy + 6.0f), 1.0f, hair);
        // A torso: hips to a shoulder point, then the head; the arms hang off the shoulder, not off the pelvis.
        const glm::vec2 shoulder = toScreen(pose[0] + glm::vec3(0.0f, 0.46f, 0.0f));
        big.line(hip, shoulder, 4.0f, ink);
        big.line(shoulder, toScreen(pose[5]), 3.4f, ink);
        big.line(hip, toScreen(pose[1]), 3.4f, ink);
        big.line(hip, toScreen(pose[2]), 3.4f, ink);
        big.disc(hip.x, hip.y, 6.0f, vermilion);
        for (size_t j = 1; j < 6; ++j) {
            const glm::vec2 p = toScreen(pose[j]);
            big.disc(p.x, p.y, 4.4f, j <= 2 ? vermilion : ink);
        }
    }

    Canvas small(720, 360);
    downsample(big, &small);
    if (!stbi_write_png(out.c_str(), small.w, small.h, 3, small.px.data(), small.w * 3)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }

    std::printf("clips %zu  entries %d  seconds 10.0\n", clips.size(), database.size());
    std::printf("distance %.2f m  peak %.2f m/s  final %.2f m/s  matches %d (first at %.2f s)\n", double(walked),
                double(peak), double(finalSpeed), matches, double(matchesAtStart) * double(dt));
    std::printf("wrote %s (%dx%d)\n", out.c_str(), small.w, small.h);

    // The figure is only evidence if the run itself is sane: the character has to cover ground, the matcher has to
    // keep choosing, and a released stick has to actually stop.
    if (walked < 12.0f || matches < 10 || finalSpeed > 0.25f) {
        std::fprintf(stderr, "motion demo self-check failed\n");
        return 1;
    }
    return 0;
}
