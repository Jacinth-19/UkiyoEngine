// Lip-sync checker.
//   voiceface_sync --gauntlet                      run the built-in test phrases through the pipeline
//   voiceface_sync --audio env.csv --mouth m.csv   check two recorded series (time, value)
// Exit status 0 when every check passes, 1 otherwise, 2 for bad arguments or files.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "voiceface/rig_controller.h"
#include "voiceface/sync_analyzer.h"
#include "voiceface/tone_voice_cloner.h"
#include "voiceface/viseme_mapper.h"
#include "voiceface/voice_cloner.h"

namespace {

// The built-in phrases. The fixed list keeps the CI run deterministic.
const char* const kPhrases[] = {"one two three four five", "bump map flip", "oh ee ah"};

int runGauntlet() {
    voiceface::ToneVoiceCloner voice(0.12);
    voiceface::RuleVisemeMapper mapper;
    voiceface::SyncConfig cfg;
    int failures = 0;
    for (const char* phrase : kPhrases) {
        voiceface::Audio audio;
        std::string err;
        if (!voice.synthesize(phrase, &audio, &err)) {
            std::printf("FAIL  \"%s\": %s\n", phrase, err.c_str());
            ++failures;
            continue;
        }
        const double duration = double(audio.samples.size()) / audio.sampleRate;
        const std::vector<voiceface::VisemeKey> keys = mapper.keys(phrase, voice.charSeconds());
        // The rig takes the weights of each key. Check them the way the renderer will read them.
        std::vector<float> weights;
        for (const voiceface::VisemeKey& k : keys) weights.push_back(k.weight);
        const std::string weightErr = voiceface::checkWeights(weights);
        if (!weightErr.empty()) {
            std::printf("FAIL  \"%s\": %s\n", phrase, weightErr.c_str());
            ++failures;
            continue;
        }
        const auto env = voiceface::rmsEnvelope(audio, 0.02, 0.01);
        const auto mouth = voiceface::openCurve(keys, 0.01, duration);
        const voiceface::SyncResult r = voiceface::analyzeSync(env, mouth, cfg);
        std::printf("%s  \"%s\": %s\n", r.pass ? "PASS" : "FAIL", phrase, r.message.c_str());
        if (!r.pass) ++failures;
    }
    std::printf("gauntlet: %d of %d phrases passed\n", int(sizeof kPhrases / sizeof kPhrases[0]) - failures,
                int(sizeof kPhrases / sizeof kPhrases[0]));
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--gauntlet") == 0) return runGauntlet();

    std::string audioPath, mouthPath;
    double maxLate = 150.0, maxEarly = 50.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--audio" && i + 1 < argc) audioPath = argv[++i];
        else if (a == "--mouth" && i + 1 < argc) mouthPath = argv[++i];
        else if (a == "--max-late-ms" && i + 1 < argc) maxLate = std::atof(argv[++i]);
        else if (a == "--max-early-ms" && i + 1 < argc) maxEarly = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "usage: voiceface_sync --gauntlet | --audio env.csv --mouth mouth.csv [--max-late-ms N] [--max-early-ms N]\n");
            return 2;
        }
    }
    if (audioPath.empty() || mouthPath.empty()) {
        std::fprintf(stderr, "usage: voiceface_sync --gauntlet | --audio env.csv --mouth mouth.csv\n");
        return 2;
    }
    std::vector<voiceface::Sample> audio, mouth;
    std::string err;
    if (!voiceface::readSeriesCsv(audioPath, &audio, &err) || !voiceface::readSeriesCsv(mouthPath, &mouth, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    voiceface::SyncConfig cfg;
    cfg.maxLateMs = maxLate;
    cfg.maxEarlyMs = maxEarly;
    const voiceface::SyncResult r = voiceface::analyzeSync(audio, mouth, cfg);
    std::printf("%s: %s\n", r.pass ? "PASS" : "FAIL", r.message.c_str());
    return r.pass ? 0 : 1;
}
