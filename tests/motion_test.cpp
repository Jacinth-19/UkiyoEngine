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
MotionClip makeWalk(const char* name, float speed, float turnRate, int frames = 180, float fps = 30.0f,
                    float phaseOffset = 0.0f) {
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
        const float phase = phaseOffset + t * 2.0f * 3.14159265f * 1.7f;
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

    // A capture in centimetres, in the MotionBuilder layout: the root's OFFSET and its position channels are the
    // same number, so the parser must use the channel as the absolute position rather than adding the two.
    const char* kCentimetreBvh = R"(HIERARCHY
ROOT Hips
{
  OFFSET 173.4 91.95 -518.3
  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
  JOINT LeftLeg
  {
    OFFSET 43.5 0.0 0.0
    CHANNELS 3 Zrotation Yrotation Xrotation
    End Site
    {
      OFFSET 17.3 0.0 0.0
    }
  }
}
MOTION
Frames: 2
Frame Time: 0.0333333
173.4 91.95 -518.3 0 0 0 0 0 0
200.0 95.00 -518.3 0 0 0 0 0 0
)";
    {
        MotionClip cm;
        std::string why;
        check(loadMotionBvh(kCentimetreBvh, &cm, &why), "a centimetre capture parses");
        check(std::fabs(cm.unitScale - 0.01f) < 1e-6f, "a centimetre capture is recognised by its hip height");
        check(std::fabs(cm.frames[1].rootPos.y - 0.95f) < 1e-3f,
              "the root's offset is not added to its position channel");
        check(std::fabs(cm.frames[1].rootPos.x - 2.0f) < 1e-3f, "the capture is scaled to metres");
        check(std::fabs(cm.joints[1].offset.x - 0.435f) < 1e-4f, "bone lengths are scaled too");

        // Recentring: the feet land on 0 and the first frame starts at the origin.
        recentreMotionClip(&cm);
        float lowest = 1e9f;
        for (const MotionFrame& f : cm.frames)
            for (const glm::vec3& p : f.world) lowest = std::min(lowest, p.y);
        check(std::fabs(lowest) < 1e-4f, "recentring puts the lowest joint on the ground");
        check(std::fabs(cm.frames[0].rootPos.x) < 1e-4f && std::fabs(cm.frames[0].rootPos.z) < 1e-4f,
              "recentring starts the clip at the origin");
    }

    // ---- Rest-pose lead-in --------------------------------------------------------------------------------
    // A capture export opens with the performer standing in a T-pose while the suit calibrates. It is not motion and
    // the matcher must not be able to match into it, or a player started at frame 0 stands with its arms out.
    {
        MotionClip hold;
        hold.name = "hold";
        hold.fps = 30.0f;
        hold.joints = {{"Hips", -1, glm::vec3(0.0f)},
                       {"LeftHand", 0, glm::vec3(0.0f)},
                       {"RightHand", 0, glm::vec3(0.0f)}};
        hold.frames.resize(90);
        for (int i = 0; i < 90; ++i) {
            MotionFrame& f = hold.frames[size_t(i)];
            f.rootPos = glm::vec3(0.0f);
            f.rootRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            if (i < 60) {
                f.world = {f.rootPos, f.rootPos + glm::vec3(0.75f, 0.55f, 0.0f),
                           f.rootPos - glm::vec3(0.75f, -0.55f, 0.0f)};
            } else {
                // The arms come down over a second of real motion, a couple of centimetres a frame: a capture does
                // not jump out of its calibration pose, and the trimmer must not read the transition as more hold.
                const float drop = std::min(1.0f, float(i - 59) / 30.0f);
                f.world = {f.rootPos, f.rootPos + glm::vec3(0.75f - 0.55f * drop, 0.55f - 0.40f * drop, 0.0f),
                           f.rootPos - glm::vec3(0.75f - 0.55f * drop, -0.55f + 0.40f * drop, 0.0f)};
            }
        }
        const int removed = trimRestLeadIn(&hold);
        check(removed >= 55 && removed <= 61, "the T-pose hold at the head of a capture is trimmed");
        check(hold.frameCount() == 90 - removed, "trimming keeps the motion that follows the hold");
        check(glm::length(hold.frames.back().world[1] - hold.frames.back().rootPos) < 0.65f,
              "the motion that follows the hold survives the trim");

        MotionClip plain = makeWalk("plain", 1.4f, 0.0f, 60);
        check(trimRestLeadIn(&plain) == 0 && plain.frameCount() == 60, "a clip without a hold is left alone");
    }

    // ---- Facing -------------------------------------------------------------------------------------------
    // The hips' local +Z is not the direction the character faces: on LAFAN1 it is a quarter turn off, so a yaw read
    // off the hips draws the character walking sideways and makes every forward query match backwards motion. The
    // loader takes forward from the toes instead.
    {
        const char* kSidewaysBvh = R"(HIERARCHY
ROOT Hips
{
  OFFSET 0.0 0.0 0.0
  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
  JOINT LeftFoot
  {
    OFFSET 0.0 -0.9 0.0
    CHANNELS 3 Zrotation Xrotation Yrotation
    End Site
    {
      OFFSET 0.2 -0.2 0.0
    }
  }
}
MOTION
Frames: 4
Frame Time: 0.0333333
0 0 0 0 0 0 0 0 0
0.05 0 0 0 0 0 0 0 0
0.10 0 0 0 0 0 0 0 0
0.15 0 0 0 0 0 0 0 0
)";
        MotionClip crab;
        std::string why;
        check(loadMotionBvh(kSidewaysBvh, &crab, &why), "a capture whose toes are not along the hips' +Z parses");
        // The toes point along +x and the character travels along +x, so forward must come out as the local +Z.
        MotionDatabase db2;
        std::vector<MotionClip> one;
        one.push_back(crab);
        db2.build(std::move(one), MotionSettings{}, &why);
        const auto f = db2.entry(db2.entryAt(0, 1)).feature;
        check(f[1] > 0.05f, "travel is forward in the character's own frame, not sideways");
        check(std::fabs(f[0]) < 0.02f, "the trajectory has no lateral component when the toes define forward");
    }

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
            // The tree searches the kIndexFeatures subspace, so that is the metric to validate it against. The
            // remaining dimensions belong to the re-rank, which is a scan over the returned candidates and is
            // checked by the pose test below.
            float brute[4] = {1e30f, 1e30f, 1e30f, 1e30f};
            for (int e = 0; e < database.size(); ++e) {
                const float d = database.indexDistance(query, database.entry(e).feature);
                for (int i = 0; i < 4; ++i)
                    if (d < brute[i]) {
                        for (int j = 3; j > i; --j) brute[j] = brute[j - 1];
                        brute[i] = d;
                        break;
                    }
            }
            for (int i = 0; i < 4; ++i) {
                const float got = database.indexDistance(query, database.entry(hit[i]).feature);
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

    // The root-velocity features belong to the trajectory: they must say where the character is going, not what the
    // pose it is standing in happened to be doing. Copying them from the pose source made a 1.6 m/s query look like a
    // standing query with a long future trajectory, and the search preferred slow frames.
    {
        const auto runQuery = database.query(glm::vec3(0.0f, 0.0f, 4.2f), 0.0f, database.entryAt(0, 10));
        check(std::fabs(runQuery[22]) < 1e-4f && std::fabs(runQuery[23] - 4.2f) < 1e-3f,
              "the query's root velocity is the requested velocity, not the pose's");
    }

    // Same trajectory, opposite foot phase. Without a pose term the two are a tie; with one, the frame the body is
    // already in wins, which is what keeps the blend small.
    {
        std::vector<MotionClip> phases;
        phases.push_back(makeWalk("phaseA", 1.4f, 0.0f, 120));
        phases.push_back(makeWalk("phaseB", 1.4f, 0.0f, 120, 30.0f, 3.14159265f));
        MotionSettings settings;
        settings.poseWeight = 1.0f;
        MotionDatabase phaseDb;
        std::string phaseError;
        phaseDb.build(std::move(phases), settings, &phaseError);
        const int from = phaseDb.entryAt(0, 40);
        const auto q = phaseDb.query(glm::vec3(0.0f, 0.0f, 1.4f), 0.0f, from);
        int pick[8];
        phaseDb.nearest(q, 8, pick);
        int bestEntry = -1;
        float best = 1e30f;
        for (int i = 0; i < 8; ++i) {
            const float c = phaseDb.cost(q, pick[i], from);
            if (c < best) { best = c; bestEntry = pick[i]; }
        }
        check(bestEntry >= 0 && phaseDb.entry(bestEntry).clip == 0,
              "the pose term keeps the match in the frame the body is already in");
        check(phaseDb.poseDistance(phaseDb.entryAt(0, 40), phaseDb.entryAt(1, 40)) > 0.5f,
              "the two phases really are far apart in body pose");
    }

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

    // ---- Controller ----------------------------------------------------------------------------------------
    {
        MotionController controller;
        controller.setDatabase(&database);
        controller.reset(glm::vec3(0.0f), 0.0f);
        const glm::vec3 start = controller.position();
        for (int i = 0; i < 90; ++i) controller.update(glm::vec3(0.0f, 0.0f, 1.0f), 1.4f, 1.0f / 30.0f, 0.0f);
        check(glm::length(controller.position() - start) > 2.5f, "the controller walks the character forward");
        check(controller.pose().size() == 3, "the controller publishes one position per joint");

        glm::vec3 mean(0.0f);
        for (const glm::vec3& p : controller.pose()) mean += p;
        mean /= float(controller.pose().size());
        check(glm::length(glm::vec2(mean.x - controller.position().x, mean.z - controller.position().z)) < 1.0f,
              "the pose is re-centred on the character, not left in clip space");

        for (int i = 0; i < 90; ++i) controller.update(glm::vec3(0.0f), 0.0f, 1.0f / 30.0f, 0.0f);
        check(controller.speed() < 0.2f, "releasing the stick decelerates to a stop");

        controller.reset(glm::vec3(0.0f), 0.0f);
        for (int i = 0; i < 60; ++i) controller.update(glm::vec3(1.0f, 0.0f, 0.0f), 1.4f, 1.0f / 30.0f, 1.5708f);
        check(std::fabs(controller.yaw() - 1.5708f) < 0.05f, "the controller turns the body to the requested facing");
    }

    // ---- Inertialization ------------------------------------------------------------------------------------
    {
        const glm::vec3 p(0.4f, -0.1f, 0.25f);
        const glm::vec3 v(1.3f, 0.2f, -0.7f);
        check(glm::length(inertialize(p, v, 0.0f) - p) < 1e-5f, "the offset starts at the outgoing difference");
        const float eps = 1e-3f;
        const glm::vec3 vStart = (inertialize(p, v, eps) - inertialize(p, v, 0.0f)) / eps;
        check(glm::length(vStart - v) < 0.02f, "the offset starts with the outgoing velocity");
        check(glm::length(inertialize(p, v, 1.0f)) < 1e-5f, "the offset is gone at the end of the blend");
        const glm::vec3 vEnd = (inertialize(p, v, 1.0f) - inertialize(p, v, 1.0f - eps)) / eps;
        check(glm::length(vEnd) < 0.02f, "the offset ends at rest, so the blend has no pop");

        // And through the player: a jump must not stop the character for a frame, which is what a crossfade did.
        MotionPlayer player2;
        player2.setDatabase(&database);
        player2.start(database.entryAt(0, 0));
        player2.setTarget(glm::vec3(0.0f, 0.0f, 1.4f), 0.0f);
        std::vector<glm::vec3> a, b;
        glm::vec3 ra(0.0f), rb(0.0f);
        glm::quat qa(1.0f, 0.0f, 0.0f, 0.0f), qb(1.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 120; ++i) {
            player2.update(1.0f / 30.0f);
            if (player2.matchCount() > 0 && i > 60) break;
        }
        // Sample either side of the seam by 1/30 s and compare the speed to the clip's own.
        player2.sample(&a, &ra, &qa);
        player2.update(1.0f / 30.0f);
        player2.sample(&b, &rb, &qb);
        check(a.size() == b.size() && !a.empty(), "the inertialized pose keeps its joint count");
        if (a.size() == b.size() && !a.empty()) {
            const float speed = glm::length(b[0] - a[0]) * 30.0f;
            check(speed > 1.0f, "the pose keeps moving through a match instead of stopping dead");
        }
    }

    std::printf("MOTION: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
