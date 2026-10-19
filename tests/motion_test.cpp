// Motion matching tests: BVH parsing, the feature vector, the KD-tree against a brute-force scan, and a short
// playback that has to walk forward. No device, no capture files.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/motion_match.h"

using namespace ukiyo;

namespace {

int gChecks = 0, gFailures = 0;

void check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("[FAIL] %s\n", what);
    } else {
        std::printf("[PASS] %s\n", what);
    }
}

// A synthetic walk: the hips advance along +z at `speed`, with the two feet stepping past each other. Enough for
// the matcher to have to choose by velocity and foot phase.
MotionClip makeWalk(const char* name, float speed, float turnRate, int frames = 180, float fps = 30.0f) {
    MotionClip clip;
    clip.name = name;
    clip.fps = fps;
    clip.joints = {{"Hips", -1, glm::vec3(0.0f)}, {"LeftFoot", 0, glm::vec3(0.0f)},
                   {"RightFoot", 0, glm::vec3(0.0f)}};
    clip.frames.resize(size_t(frames));
    glm::vec3 pos(0.0f);
    float yaw = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float t = float(i) / fps;
        yaw += turnRate / fps;
        const glm::quat rot = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        MotionFrame& f = clip.frames[size_t(i)];
        f.rootRot = rot;
        f.rootPos = pos;
        const float phase = t * 2.0f * 3.14159265f * 1.7f;
        const glm::vec3 side = rot * glm::vec3(0.13f, 0.0f, 0.0f);
        const glm::vec3 forward = rot * glm::vec3(0.0f, 0.0f, 0.42f * std::sin(phase));
        const glm::vec3 up = glm::vec3(0.0f, 0.08f + 0.06f * std::max(0.0f, std::sin(phase)), 0.0f);
        f.world = {pos,
                   pos + side + forward + up,
                   pos - side - forward + up};
        pos += rot * glm::vec3(0.0f, 0.0f, speed / fps);
    }
    return clip;
}

const char* kTinyBvh = R"(HIERARCHY
ROOT Hips
{
  OFFSET 0.0 0.0 0.0
  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
  JOINT LeftFoot
  {
    OFFSET 0.2 -1.0 0.0
    CHANNELS 3 Zrotation Xrotation Yrotation
    End Site
    {
      OFFSET 0.0 -0.2 0.0
    }
  }
}
MOTION
Frames: 3
Frame Time: 0.0333333
0 0 0 0 0 0 0 0 0
0 0 0.5 0 0 0 10 0 0
0 0 1.0 0 0 0 20 0 0
)";

} // namespace

int main() {
    // ---- BVH ----------------------------------------------------------------------------------------------
    MotionClip bvh;
    std::string error;
    const bool parsed = loadMotionBvh(kTinyBvh, &bvh, &error);
    check(parsed, "a small BVH parses");
    if (!parsed) std::printf("  %s\n", error.c_str());
    check(bvh.joints.size() == 3, "the BVH has three joints (including End Site)");
    check(bvh.frameCount() == 3, "the BVH has three frames");
    check(std::fabs(bvh.fps - 30.0f) < 0.01f, "the frame time becomes an fps");
    check(std::fabs(bvh.frames[2].rootPos.z - 1.0f) < 1e-4f, "root translation is read");
    check(std::fabs(bvh.frames[2].world[1].z - 1.0f) < 1e-4f, "the child follows the root");

    // ---- Database -----------------------------------------------------------------------------------------
    std::vector<MotionClip> clips;
    clips.push_back(makeWalk("walk", 1.4f, 0.0f));
    clips.push_back(makeWalk("run", 4.2f, 0.0f));
    clips.push_back(makeWalk("turn", 1.4f, 0.9f));

    MotionDatabase database;
    const bool built = database.build(std::move(clips), MotionSettings{}, &error);
    check(built, "the database builds");
    check(database.size() == 540, "every frame becomes an entry");

    MotionClip broken;
    std::string bvhError;
    check(!loadMotionBvh("not a bvh", &broken, &bvhError) && !bvhError.empty(), "a malformed BVH is rejected");

    // ---- Features -----------------------------------------------------------------------------------------
    const auto walkFeature = database.entry(database.entryAt(0, 10)).feature;
    check(std::fabs(walkFeature[0]) < 1e-4f, "a straight walk has no lateral trajectory");
    check(std::fabs(walkFeature[1] - 1.4f * 0.15f) < 0.03f, "the first trajectory sample is one step ahead");
    check(std::fabs(walkFeature[23] - 1.4f) < 0.05f, "the root velocity feature matches the clip speed");

    // ---- KD-tree vs brute force ---------------------------------------------------------------------------
    {
        bool same = true;
        uint32_t seed = 12345u;
        auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return float((seed >> 8) & 0xffff) / 65535.0f;
        };
        for (int q = 0; q < 40 && same; ++q) {
            std::array<float, kMotionDims> query{};
            for (int d = 0; d < kMotionDims; ++d) query[size_t(d)] = (rnd() * 2.0f - 1.0f) * 2.0f;
            int hit[4] = {-1, -1, -1, -1};
            const int n = database.nearest(query, 4, hit);
            if (n != 4) {
                same = false;
                break;
            }
            float brute[4] = {1e30f, 1e30f, 1e30f, 1e30f};
            for (int e = 0; e < database.size(); ++e) {
                const float d = database.weightedDistance(query, database.entry(e).feature);
                for (int i = 0; i < 4; ++i)
                    if (d < brute[i]) {
                        for (int j = 3; j > i; --j) brute[j] = brute[j - 1];
                        brute[i] = d;
                        break;
                    }
            }
            for (int i = 0; i < 4; ++i) {
                const float got = database.weightedDistance(query, database.entry(hit[i]).feature);
                if (std::fabs(got - brute[i]) > 1e-3f) same = false;
            }
        }
        check(same, "the KD-tree returns the same four neighbours as a brute-force scan");
    }

    // ---- Query and playback --------------------------------------------------------------------------------
    const auto query = database.query(glm::vec3(0.0f, 0.0f, 1.4f), 0.0f, database.entryAt(0, 0));
    int hits[8];
    const int hitCount = database.nearest(query, 8, hits);
    check(hitCount == 8, "a query returns eight candidates");
    bool walking = true;
    for (int i = 0; i < hitCount; ++i)
        if (database.entry(hits[i]).clip == 1) walking = false;
    check(walking, "a 1.4 m/s query never matches the 4.2 m/s run clip");
    check(database.entry(hits[0]).clip == 0, "the best match is the walk");

    MotionPlayer player;
    player.setDatabase(&database);
    player.start(database.entryAt(0, 0));
    player.setTarget(glm::vec3(0.0f, 0.0f, 1.4f), 0.0f);
    std::vector<glm::vec3> pose;
    glm::vec3 root(0.0f);
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 180; ++i) player.update(1.0f / 30.0f);
    player.sample(&pose, &root, &rot);
    check(player.matchCount() > 0, "the player re-matched during three seconds of walking");
    check(root.z > 3.0f, "the character walked forward");
    check(pose.size() == 3, "the sampled pose has one position per joint");
    check(std::fabs(root.y) < 1e-3f, "the root stayed on the ground plane");

    std::printf("MOTION: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
