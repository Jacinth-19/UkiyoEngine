#pragma once
#include <string>
#include <vector>

#include "voiceface/sync_analyzer.h"

namespace voiceface {

// Mono float PCM, -1..1.
struct Audio {
    int sampleRate = 16000;
    std::vector<float> samples;
};

// A voice source. Real implementations (a local model, or a cloud API behind an adapter) go here.
// Cloned voices need the speaker's consent and must be marked as synthetic (see README.md).
class IVoiceCloner {
public:
    virtual ~IVoiceCloner() = default;
    // Returns false and sets *error when the text cannot be spoken.
    virtual bool synthesize(const std::string& text, Audio* out, std::string* error) = 0;
};

// RMS level over the `windowS` seconds that end at each step, one sample every `stepS` seconds.
std::vector<Sample> rmsEnvelope(const Audio& a, double windowS, double stepS);

} // namespace voiceface
