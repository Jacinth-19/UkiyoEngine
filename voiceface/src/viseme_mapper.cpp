#include "voiceface/viseme_mapper.h"

#include <cctype>

namespace voiceface {

const char* visemeName(Viseme v) {
    switch (v) {
        case Viseme::Rest: return "rest";
        case Viseme::Open: return "open";
        case Viseme::Wide: return "wide";
        case Viseme::Round: return "round";
        case Viseme::Closed: return "closed";
        case Viseme::LipTeeth: return "lip-teeth";
        case Viseme::Neutral: return "neutral";
    }
    return "rest";
}

double opennessOf(Viseme v) {
    switch (v) {
        case Viseme::Open: return 1.0;
        case Viseme::Wide: return 0.6;
        case Viseme::Round: return 0.5;
        case Viseme::Neutral: return 0.25;
        case Viseme::LipTeeth: return 0.1;
        case Viseme::Closed:
        case Viseme::Rest: return 0.0;
    }
    return 0.0;
}

std::vector<VisemeKey> RuleVisemeMapper::keys(const std::string& text, double charSeconds) const {
    std::vector<VisemeKey> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const double t = double(i) * charSeconds;
        const unsigned char c = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(text[i])));
        Viseme v = Viseme::Neutral;
        float w = 1.0f;
        if (c == 'a') {
            v = Viseme::Open;
        } else if (c == 'e' || c == 'i') {
            v = Viseme::Wide;
        } else if (c == 'o' || c == 'u') {
            v = Viseme::Round;
        } else if (c == 'b' || c == 'm' || c == 'p') {
            v = Viseme::Closed;
        } else if (c == 'f' || c == 'v') {
            v = Viseme::LipTeeth;
        } else if (!std::isalpha(c)) {
            v = Viseme::Rest;   // spaces and punctuation
            w = 0.0f;
        }
        out.push_back({t, v, w});
    }
    return out;
}

std::vector<Sample> openCurve(const std::vector<VisemeKey>& keys, double stepS, double endS) {
    std::vector<Sample> out;
    size_t k = 0;
    double value = 0.0;
    for (double t = 0.0; t <= endS + 1e-9; t += stepS) {
        while (k < keys.size() && keys[k].t <= t + 1e-9) {
            value = opennessOf(keys[k].viseme) * keys[k].weight;
            ++k;
        }
        out.push_back({t, value});
    }
    return out;
}

} // namespace voiceface
