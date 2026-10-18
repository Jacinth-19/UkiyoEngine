#include "voiceface/rig_controller.h"

#include <algorithm>
#include <cmath>

namespace voiceface {

bool RigController::setWeight(const std::string& name, float w) {
    if (!std::isfinite(w)) return false;
    weights_[name] = std::clamp(w, 0.0f, 1.0f);
    return true;
}

float RigController::weight(const std::string& name) const {
    auto it = weights_.find(name);
    return it == weights_.end() ? 0.0f : it->second;
}

bool RigController::setBoneRotation(const std::string& bone, const std::array<float, 4>& q) {
    for (float v : q) {
        if (!std::isfinite(v)) return false;
    }
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (std::fabs(len - 1.0f) > 1e-3f) return false;
    bones_[bone] = q;
    return true;
}

} // namespace voiceface
