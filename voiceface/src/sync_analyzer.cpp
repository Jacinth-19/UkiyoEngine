#include "voiceface/sync_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace voiceface {

bool firstOnset(const std::vector<Sample>& series, double threshold, double* t) {
    for (const Sample& p : series) {
        if (p.v >= threshold) {
            *t = p.t;
            return true;
        }
    }
    return false;
}

SyncResult analyzeSync(const std::vector<Sample>& audio, const std::vector<Sample>& mouth, const SyncConfig& cfg) {
    SyncResult r;
    double peak = 0.0;
    for (const Sample& p : audio) peak = std::max(peak, p.v);
    if (peak > 0.0) r.hasAudioOnset = firstOnset(audio, cfg.audioThreshold * peak, &r.audioOnsetS);
    r.hasMouthOnset = firstOnset(mouth, cfg.mouthThreshold, &r.mouthOnsetS);
    if (!r.hasAudioOnset) {
        r.message = "no speech onset in the audio";
        return r;
    }
    if (!r.hasMouthOnset) {
        r.message = "the mouth never moves";
        return r;
    }
    r.lagMs = (r.mouthOnsetS - r.audioOnsetS) * 1000.0;
    r.pass = r.lagMs <= cfg.maxLateMs && r.lagMs >= -cfg.maxEarlyMs;
    char buf[160];
    std::snprintf(buf, sizeof buf, "lag %.1f ms (allowed -%.0f to +%.0f ms)", r.lagMs, cfg.maxEarlyMs, cfg.maxLateMs);
    r.message = buf;
    return r;
}

std::string checkWeights(const std::vector<float>& weights) {
    for (size_t i = 0; i < weights.size(); ++i) {
        if (!std::isfinite(weights[i])) return "weight " + std::to_string(i) + " is not a finite number";
        if (weights[i] < 0.0f || weights[i] > 1.0f) return "weight " + std::to_string(i) + " is outside [0, 1]";
    }
    return {};
}

bool readSeriesCsv(const std::string& path, std::vector<Sample>* out, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        *error = "cannot open " + path;
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream ss(line);
        Sample p;
        if (!(ss >> p.t >> p.v)) {
            if (lineNo == 1) continue;   // header
            *error = path + ":" + std::to_string(lineNo) + ": expected two numbers";
            return false;
        }
        if (!out->empty() && p.t < out->back().t) {
            *error = path + ":" + std::to_string(lineNo) + ": times must not go backwards";
            return false;
        }
        out->push_back(p);
    }
    return true;
}

} // namespace voiceface
