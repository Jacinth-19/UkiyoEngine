#pragma once
#include <string>
#include <vector>

namespace voiceface {

// One point of a time series: time in seconds and a value.
struct Sample {
    double t = 0.0;
    double v = 0.0;
};

struct SyncConfig {
    double audioThreshold = 0.1;   // fraction of the audio peak that counts as speech onset
    double mouthThreshold = 0.2;   // mouth openness (0..1) that counts as movement onset
    double maxLateMs = 150.0;      // the mouth may trail the audio by up to this much
    double maxEarlyMs = 50.0;      // the mouth may lead the audio by up to this much
};

struct SyncResult {
    bool hasAudioOnset = false;
    bool hasMouthOnset = false;
    bool pass = false;
    double audioOnsetS = 0.0;
    double mouthOnsetS = 0.0;
    double lagMs = 0.0;           // mouth onset minus audio onset; positive means the mouth is late
    std::string message;
};

// First time the series reaches `threshold`. The series must be sorted by time.
bool firstOnset(const std::vector<Sample>& series, double threshold, double* t);

// Compares speech onset in the audio with the first mouth movement.
SyncResult analyzeSync(const std::vector<Sample>& audioEnvelope, const std::vector<Sample>& mouthOpen,
                       const SyncConfig& cfg);

// Blendshape weights must be finite and in [0, 1]. Returns an empty string when they are valid,
// otherwise a message naming the first bad weight.
std::string checkWeights(const std::vector<float>& weights);

// Reads "t,v" or "t v" lines. A non-numeric first line is treated as a header.
bool readSeriesCsv(const std::string& path, std::vector<Sample>* out, std::string* error);

} // namespace voiceface
