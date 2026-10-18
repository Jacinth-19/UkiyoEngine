#include "voiceface/tone_voice_cloner.h"

#include <cctype>
#include <cmath>

namespace voiceface {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

bool ToneVoiceCloner::synthesize(const std::string& text, Audio* out, std::string* error) {
    if (text.empty()) {
        *error = "no text to speak";
        return false;
    }
    const int rate = 16000;
    const double burstS = 0.08;
    const double total = charSeconds_ * double(text.size()) + 0.1;
    out->sampleRate = rate;
    out->samples.assign(static_cast<size_t>(total * rate), 0.0f);
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isalpha(static_cast<unsigned char>(text[i]))) continue;
        const size_t start = static_cast<size_t>(double(i) * charSeconds_ * rate);
        const size_t len = static_cast<size_t>(burstS * rate);
        for (size_t n = 0; n < len && start + n < out->samples.size(); ++n) {
            const double t = double(n) / rate;
            const double env = std::sin(kPi * t / burstS);
            out->samples[start + n] = static_cast<float>(0.5 * env * std::sin(2.0 * kPi * 220.0 * t));
        }
    }
    return true;
}

std::vector<Sample> rmsEnvelope(const Audio& a, double windowS, double stepS) {
    std::vector<Sample> out;
    const double rate = a.sampleRate;
    const size_t win = std::max<size_t>(1, static_cast<size_t>(windowS * rate));
    const double duration = double(a.samples.size()) / rate;
    for (double t = stepS; t <= duration + 1e-9; t += stepS) {
        const size_t end = std::min(a.samples.size(), static_cast<size_t>(t * rate));
        const size_t begin = end > win ? end - win : 0;
        double sum = 0.0;
        for (size_t i = begin; i < end; ++i) sum += double(a.samples[i]) * a.samples[i];
        const double n = double(end - begin);
        out.push_back({t, n > 0.0 ? std::sqrt(sum / n) : 0.0});
    }
    return out;
}

} // namespace voiceface
