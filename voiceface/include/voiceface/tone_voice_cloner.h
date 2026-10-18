#pragma once
#include "voiceface/voice_cloner.h"

namespace voiceface {

// Deterministic test voice, not speech: one short tone burst per letter, silence for other characters.
// It exists so the sync pipeline can run in CI with no model or network.
class ToneVoiceCloner final : public IVoiceCloner {
public:
    explicit ToneVoiceCloner(double charSeconds = 0.12) : charSeconds_(charSeconds) {}
    bool synthesize(const std::string& text, Audio* out, std::string* error) override;
    double charSeconds() const { return charSeconds_; }

private:
    double charSeconds_;
};

} // namespace voiceface
