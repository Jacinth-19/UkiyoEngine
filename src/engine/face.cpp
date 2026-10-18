#include "engine/face.h"

#include "engine/model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ukiyo {

bool FaceTrack::load(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (const std::exception& e) {
        err = std::string("bad JSON in ") + path + ": " + e.what();
        return false;
    }
    fps_ = j.value("fps", 100.0f);
    duration_ = j.value("duration", 0.0f);
    channels_.clear();
    if (!j.contains("channels") || !j["channels"].is_object()) {
        err = "no channels in " + path;
        return false;
    }
    for (auto it = j["channels"].begin(); it != j["channels"].end(); ++it)
        channels_[it.key()] = it.value().get<std::vector<float>>();
    if (channels_.empty() || fps_ <= 0.0f) {
        err = "empty face track " + path;
        return false;
    }
    return true;
}

std::map<std::string, float> FaceTrack::sample(float t) const {
    std::map<std::string, float> out;
    for (const auto& [name, v] : channels_) {
        if (v.empty()) {
            out[name] = 0.0f;
            continue;
        }
        const float x = std::clamp(t * fps_, 0.0f, float(v.size() - 1));
        const size_t i0 = size_t(std::floor(x));
        const size_t i1 = std::min(i0 + 1, v.size() - 1);
        const float f = x - float(i0);
        out[name] = v[i0] + (v[i1] - v[i0]) * f;
    }
    return out;
}

void applyFaceChannels(Model& model, const std::map<std::string, float>& values) {
    const bool hasSurprised = model.expressionBinds.count("surprised") > 0;
    for (const auto& [name, v] : values) {
        if (name == "nod") {
            model.faceNod = v * 0.09f;
        } else if (name == "tilt") {
            model.faceTilt = v * 0.09f;
        } else if (name == "happy") {
            // Seed-san's "happy" preset squints the eyes shut at full weight, so it is capped.
            model.expressionValues["happy"] = 0.6f * v;
        } else if (name == "surprised") {
            if (hasSurprised) {
                model.expressionValues["surprised"] = v;
            } else {
                // Fallback: no surprise preset and no brow or eye-widen morphs (Alicia's VRM 0.x
                // file has neither), so the surprise is carried by the mouth and the head. The
                // mouth opens into an "oh" and the head leans back about 5 degrees at full weight.
                model.expressionValues["oh"] = std::max(model.expressionValues["oh"], 0.9f * v);
                model.faceNod -= 0.08f * v;
            }
        } else {
            model.expressionValues[name] = v;
        }
    }
}

} // namespace ukiyo
