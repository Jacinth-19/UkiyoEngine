#include "systems/audio.h"

#include "miniaudio.h"

namespace ukiyo {

struct AudioEngine::Impl {
    ma_engine engine{};
    bool engineReady = false;
    bool noDevice = false;
    SoundId nextId = 1;
    std::map<SoundId, ma_sound*> sounds;
};

AudioEngine::AudioEngine(bool noDevice) : impl_(new Impl) {
    impl_->noDevice = noDevice;
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = noDevice ? MA_TRUE : MA_FALSE;
    config.channels = 2;
    config.sampleRate = 48000;
    const ma_result r = ma_engine_init(&config, &impl_->engine);
    if (r != MA_SUCCESS) {
        error_ = std::string("could not start audio: ") + ma_result_description(r);
        return;
    }
    impl_->engineReady = true;
    ok_ = true;
}

AudioEngine::~AudioEngine() {
    if (!impl_) return;
    for (auto& [id, snd] : impl_->sounds) {
        ma_sound_uninit(snd);
        delete snd;
    }
    impl_->sounds.clear();
    if (impl_->engineReady) ma_engine_uninit(&impl_->engine);
}

AudioEngine::SoundId AudioEngine::load(const std::string& path) {
    if (!ok_) return 0;
    ma_sound* snd = new ma_sound{};
    const ma_result r = ma_sound_init_from_file(&impl_->engine, path.c_str(),
                                                MA_SOUND_FLAG_DECODE, nullptr, nullptr, snd);
    if (r != MA_SUCCESS) {
        delete snd;
        error_ = "could not load " + path + ": " + ma_result_description(r);
        return 0;
    }
    const SoundId id = impl_->nextId++;
    impl_->sounds[id] = snd;
    return id;
}

namespace {
ma_sound* find(const std::map<AudioEngine::SoundId, ma_sound*>& m, AudioEngine::SoundId id) {
    auto it = m.find(id);
    return it == m.end() ? nullptr : it->second;
}
} // namespace

bool AudioEngine::play(SoundId id) {
    ma_sound* snd = find(impl_->sounds, id);
    if (!snd) return false;
    ma_sound_seek_to_pcm_frame(snd, 0);
    return ma_sound_start(snd) == MA_SUCCESS;
}

void AudioEngine::stop(SoundId id) {
    if (ma_sound* snd = find(impl_->sounds, id)) ma_sound_stop(snd);
}

bool AudioEngine::isPlaying(SoundId id) const {
    ma_sound* snd = find(impl_->sounds, id);
    return snd && ma_sound_is_playing(snd);
}

void AudioEngine::setVolume(SoundId id, float volume) {
    if (ma_sound* snd = find(impl_->sounds, id)) ma_sound_set_volume(snd, volume);
}

void AudioEngine::setLooping(SoundId id, bool loop) {
    if (ma_sound* snd = find(impl_->sounds, id)) ma_sound_set_looping(snd, loop ? MA_TRUE : MA_FALSE);
}

void AudioEngine::setPosition(SoundId id, const glm::vec3& p) {
    if (ma_sound* snd = find(impl_->sounds, id)) ma_sound_set_position(snd, p.x, p.y, p.z);
}

void AudioEngine::setSpatial(SoundId id, bool spatial) {
    if (ma_sound* snd = find(impl_->sounds, id)) ma_sound_set_spatialization_enabled(snd, spatial ? MA_TRUE : MA_FALSE);
}

void AudioEngine::setListener(const glm::vec3& position, const glm::vec3& forward) {
    if (!ok_) return;
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 1.0f, 0.0f);
}

void AudioEngine::setMasterVolume(float v) {
    master_ = v;
    if (ok_) ma_engine_set_volume(&impl_->engine, v);
}

uint64_t AudioEngine::pump(float* out, uint32_t frames) {
    if (!ok_ || !impl_->noDevice) return 0;
    ma_uint64 done = 0;
    ma_engine_read_pcm_frames(&impl_->engine, out, frames, &done);
    return done;
}

} // namespace ukiyo
