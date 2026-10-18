#pragma once
#include <string>
#include <vector>

#include "voiceface/sync_analyzer.h"

namespace voiceface {

// Mouth shapes. Rest is the closed neutral mouth with no pose.
enum class Viseme { Rest, Open, Wide, Round, Closed, LipTeeth, Neutral };

struct VisemeKey {
    double t = 0.0;      // seconds from the start of the utterance
    Viseme viseme = Viseme::Rest;
    float weight = 0.0f; // 0..1
};

const char* visemeName(Viseme v);

// How open the mouth looks for a shape, 0 (closed) to 1 (wide open).
double opennessOf(Viseme v);

// Maps speech to mouth shapes. The real implementation will take phoneme timing from the voice engine.
class IVisemeMapper {
public:
    virtual ~IVisemeMapper() = default;
    virtual std::vector<VisemeKey> keys(const std::string& text, double charSeconds) const = 0;
};

// Placeholder mapper: one shape per letter, spaced `charSeconds` apart. Vowels open the mouth,
// B, M and P close it, F and V show the lip-teeth shape, other letters use a neutral shape.
class RuleVisemeMapper final : public IVisemeMapper {
public:
    std::vector<VisemeKey> keys(const std::string& text, double charSeconds) const override;
};

// Openness over time, sampled every `stepS` seconds up to `endS`. Holds each key until the next one.
std::vector<Sample> openCurve(const std::vector<VisemeKey>& keys, double stepS, double endS);

} // namespace voiceface
