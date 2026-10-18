// Audio on miniaudio: one engine, sounds loaded from files, optional 3D placement and a listener.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <glm/glm.hpp>

namespace ukiyo {

class AudioEngine {
public:
    using SoundId = uint32_t;   // 0 is "no sound"

    // With noDevice the engine runs without an output device. Tests use it, and audio is pulled with pump().
    explicit AudioEngine(bool noDevice = false);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool ok() const { return ok_; }
    std::string lastError() const { return error_; }

    // Loads a sound file (WAV, FLAC, MP3, or OGG). Returns 0 on failure.
    SoundId load(const std::string& path);
    // Plays a sound. Each call starts a new voice, so one sound can overlap itself.
    bool play(SoundId id);
    void stop(SoundId id);
    bool isPlaying(SoundId id) const;
    void setVolume(SoundId id, float volume);
    void setLooping(SoundId id, bool loop);
    // Spatial placement. A sound with a position is attenuated and panned relative to the listener.
    void setPosition(SoundId id, const glm::vec3& p);
    void setSpatial(SoundId id, bool spatial);
    void setListener(const glm::vec3& position, const glm::vec3& forward);
    void setMasterVolume(float v);
    float masterVolume() const { return master_; }

    // Pulls frames without a device, returning how many were produced. Only valid with noDevice.
    uint64_t pump(float* interleavedStereo, uint32_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = false;
    float master_ = 1.0f;
    std::string error_;
};

} // namespace ukiyo
