// Audio on miniaudio: one engine, three mixer buses, polyphonic voices, optional 3D placement and a listener.
//
// The shape is the one every game engine's mixer has, kept deliberately small:
//
//     voices ──> Music   ─┐
//     voices ──> Sfx     ─┼──> Master (the engine's own volume) ──> device
//     voices ──> Ambient ─┘
//
// Each bus is one custom node in miniaudio's graph that sums its voices, applies the bus volume, and runs an
// optional reverb insert (wet = 0 is an exact gain-only pass-through, which is how the mixer sounds with no
// effect engaged). Music is streamed from disk, never counts against the voice limit, and fades are handed to
// miniaudio's stop-time scheduling so they complete on the audio thread even when nothing ticks.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

class AudioEngine {
public:
    using SoundId = uint32_t;   // 0 is "no sound"

    // The mixer buses. Every voice plays on exactly one; all three feed Master.
    enum class Bus : uint8_t { Music = 0, Sfx = 1, Ambient = 2 };

    // With noDevice the engine runs without an output device. Tests use it, and audio is pulled with pump().
    explicit AudioEngine(bool noDevice = false);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool ok() const { return ok_; }
    std::string lastError() const { return error_; }

    // Registers a sound file (WAV, FLAC, MP3, OGG) for playback. The file is opened once to prove it is readable;
    // the data itself is opened per voice. Returns 0 on failure.
    SoundId load(const std::string& path);

    // Plays a registered sound as a new voice on the Sfx bus. Voices overlap: the same sound can be playing three
    // times at once, which is what a game expects from "each call starts a new voice".
    bool play(SoundId id);
    // The same, on a chosen bus.
    bool playOn(SoundId id, Bus bus);
    // Stops every voice of this sound.
    void stop(SoundId id);
    bool isPlaying(SoundId id) const;
    // The voices currently sounding, music excepted (the mixer counts them; the limit steals the oldest).
    uint32_t activeVoices() const;
    void setVoiceLimit(uint32_t voices);
    uint32_t voiceLimit() const { return voiceLimit_; }

    // Per-sound defaults. Set before play() to shape the next voice; they also reach live voices of that id.
    void setVolume(SoundId id, float volume);
    void setLooping(SoundId id, bool loop);
    // Spatial placement. A sound with a position is attenuated and panned relative to the listener.
    void setPosition(SoundId id, const glm::vec3& p);
    void setSpatial(SoundId id, bool spatial);
    void setListener(const glm::vec3& position, const glm::vec3& forward);

    // Buses. Volume is the bus's gain after all voices sum; wet is the reverb insert, 0 = dry, 1 = fully wet-ish
    // (the reverb is added, not crossfaded, so 0 changes nothing about the dry signal).
    void setBusVolume(Bus bus, float volume);
    float busVolume(Bus bus) const;
    void setBusReverb(Bus bus, float wet);
    float busReverb(Bus bus) const;

    // Streaming music: one voice, on the Music bus, opened without decoding the whole file, immune to the voice
    // limit. fadeInSeconds ramps it up from silence; stopMusic fades it out and stops it on the audio thread.
    bool playMusic(const std::string& path, bool loop, float fadeInSeconds = 0.0f);
    void stopMusic(float fadeOutSeconds = 0.0f);
    bool musicPlaying() const;

    void setMasterVolume(float v);
    float masterVolume() const { return master_; }

    // Housekeeping: reaps voices that have finished. Call it once a frame; pump() does it too.
    void tick();

    // Pulls frames without a device, returning how many were produced. Only valid with noDevice.
    uint64_t pump(float* interleavedStereo, uint32_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = false;
    float master_ = 1.0f;
    uint32_t voiceLimit_ = 48;
    std::string error_;
};

} // namespace ukiyo
