// Unit and wiring tests. No framework: each check prints PASS or FAIL and the run exits 1 on any failure.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "voiceface/rig_controller.h"
#include "voiceface/sync_analyzer.h"
#include "voiceface/tone_voice_cloner.h"
#include "voiceface/viseme_mapper.h"
#include "voiceface/voice_cloner.h"

using namespace voiceface;

static int passed = 0;
static int failed = 0;

static void check(bool ok, const std::string& name) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
    ok ? ++passed : ++failed;
}

// A ramp that rises at `onset` seconds, sampled every 10 ms for 2 s.
static std::vector<Sample> step(double onset, double level) {
    std::vector<Sample> s;
    for (int i = 0; i <= 200; ++i) {
        const double t = i * 0.01;
        s.push_back({t, t >= onset ? level : 0.0});
    }
    return s;
}

static void testOnsets() {
    double t = -1.0;
    check(firstOnset(step(0.5, 1.0), 0.5, &t) && std::fabs(t - 0.5) < 1e-9, "onset: the first sample above the threshold is found");
    check(!firstOnset(step(0.5, 0.1), 0.5, &t), "onset: a series that never reaches the threshold has no onset");
}

static void testSync() {
    SyncConfig cfg;
    SyncResult ok = analyzeSync(step(0.5, 1.0), step(0.58, 1.0), cfg);
    check(ok.pass && std::fabs(ok.lagMs - 80.0) < 1e-6, "sync: mouth 80 ms late passes (" + ok.message + ")");

    SyncResult late = analyzeSync(step(0.5, 1.0), step(0.8, 1.0), cfg);
    check(!late.pass, "sync: mouth 300 ms late fails (" + late.message + ")");

    SyncResult early = analyzeSync(step(0.5, 1.0), step(0.4, 1.0), cfg);
    check(!early.pass, "sync: mouth 100 ms early fails (" + early.message + ")");

    SyncResult silent = analyzeSync(step(0.5, 0.0), step(0.5, 1.0), cfg);
    check(!silent.pass && !silent.hasAudioOnset, "sync: silent audio gives no onset and fails");

    SyncResult still = analyzeSync(step(0.5, 1.0), step(0.5, 0.0), cfg);
    check(!still.pass && !still.hasMouthOnset, "sync: a mouth that never moves fails");
}

static void testWeights() {
    check(checkWeights({0.0f, 0.5f, 1.0f}).empty(), "weights: values in [0, 1] are valid");
    check(!checkWeights({0.2f, 1.5f}).empty(), "weights: a value above 1 is rejected");
    check(!checkWeights({std::nanf("")}).empty(), "weights: NaN is rejected");

    RigController rig;
    check(rig.setWeight("jawOpen", 2.0f) && rig.weight("jawOpen") == 1.0f, "rig: weights above 1 are clamped to 1");
    check(!rig.setWeight("jawOpen", std::nanf("")) && rig.weight("jawOpen") == 1.0f, "rig: NaN is refused and the old value kept");
    check(rig.setBoneRotation("head", {1.0f, 0.0f, 0.0f, 0.0f}), "rig: an identity bone rotation is accepted");
    check(!rig.setBoneRotation("jaw", {2.0f, 0.0f, 0.0f, 0.0f}) && !rig.hasBone("jaw"), "rig: a non-unit rotation is refused");
}

static void testMapper() {
    RuleVisemeMapper m;
    const auto keys = m.keys("ab o ", 0.1);
    check(keys.size() == 5, "mapper: one key per character");
    check(keys[0].viseme == Viseme::Open, "mapper: a vowel opens the mouth");
    check(keys[1].viseme == Viseme::Closed, "mapper: b closes the mouth");
    check(keys[3].viseme == Viseme::Round && std::fabs(keys[3].t - 0.3) < 1e-9, "mapper: o maps to round at its time");
    check(keys[2].viseme == Viseme::Rest && keys[2].weight == 0.0f, "mapper: a space is a rest with no weight");

    const auto curve = openCurve(keys, 0.05, 0.6);
    check(!curve.empty() && curve[0].v == 1.0, "curve: the open vowel is at full openness at the start");
    check(curve[1].v == 1.0, "curve: the vowel is held until the next key (0.05 s)");
    check(curve[2].v == 0.0, "curve: the closed consonant at 0.1 s gives zero openness");
}

static void testWiring() {
    // The full path, with the tone voice: synthesis, envelope, mouth curve and sync check.
    ToneVoiceCloner voice(0.12);
    RuleVisemeMapper mapper;
    Audio audio;
    std::string err;
    check(voice.synthesize("one two three", &audio, &err), "wiring: the tone voice synthesises a phrase");
    const double duration = double(audio.samples.size()) / audio.sampleRate;
    const auto env = rmsEnvelope(audio, 0.02, 0.01);
    const auto mouth = openCurve(mapper.keys("one two three", voice.charSeconds()), 0.01, duration);
    const SyncResult r = analyzeSync(env, mouth, SyncConfig{});
    check(r.pass, "wiring: the phrase passes the sync check (" + r.message + ")");

    Audio empty;
    check(!voice.synthesize("", &empty, &err), "wiring: empty text is refused");
}

int main() {
    testOnsets();
    testSync();
    testWeights();
    testMapper();
    testWiring();
    std::printf("voiceface: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
