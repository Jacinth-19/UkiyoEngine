// Face timeline produced by tools/face_align.py: per-channel expression curves sampled
// at a fixed rate. Channel names are the canonical expression names (see model.h).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ukiyo {

struct Model;

// Applies one face sample to a model: expression weights, head nod/tilt, and the
// fallback for models without a "surprised" preset (VRM 0.x Alicia): surprise opens
// the mouth into an "oh" shape instead.
void applyFaceChannels(Model& model, const std::map<std::string, float>& values);

class FaceTrack {
public:
    // Loads a face_align.py JSON file. Returns false and sets err on failure.
    bool load(const std::string& path, std::string& err);

    // All channels at time t (seconds), linearly interpolated and clamped at the ends.
    std::map<std::string, float> sample(float t) const;

    float duration() const { return duration_; }
    // Channel names as they appear in the JSON (canonical expression names, plus nod and tilt).
    bool empty() const { return channels_.empty(); }

private:
    float fps_ = 100.0f;
    float duration_ = 0.0f;
    std::map<std::string, std::vector<float>> channels_;
};

} // namespace ukiyo
