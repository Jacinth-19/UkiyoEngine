#pragma once
#include <array>
#include <map>
#include <string>

namespace voiceface {

// CPU-side state of a face rig: blendshape weights and bone rotations. The renderer copies these
// into GPU buffers each frame.
class RigController {
public:
    // Clamps the weight to [0, 1]. Returns false, and changes nothing, when the weight is NaN or infinite.
    bool setWeight(const std::string& name, float w);
    float weight(const std::string& name) const;
    const std::map<std::string, float>& weights() const { return weights_; }

    // A bone rotation as a unit quaternion (w, x, y, z). Returns false for non-finite or non-unit input.
    bool setBoneRotation(const std::string& bone, const std::array<float, 4>& q);
    bool hasBone(const std::string& bone) const { return bones_.count(bone) != 0; }

private:
    std::map<std::string, float> weights_;
    std::map<std::string, std::array<float, 4>> bones_;
};

} // namespace voiceface
