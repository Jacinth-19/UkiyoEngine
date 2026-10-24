#include "systems/audio.h"

#include "miniaudio.h"

#include <algorithm>
#include <cstring>

namespace ukiyo {

namespace {

constexpr ma_uint32 kChannels = 2;

// Freeverb's comb lengths in samples at 44.1 kHz, scaled to this engine's fixed 48 kHz.
constexpr int kCombLen[4] = {1214, 1292, 1390, 1475};
constexpr int kAllpassLen[2] = {605, 480};
constexpr float kCombFeedback = 0.84f;
constexpr float kCombDamp = 0.22f;
constexpr float kAllpassG = 0.5f;
constexpr int kMaxCombLen = 1475;
constexpr int kMaxAllpassLen = 700;

// One node per mixer bus: it sums every voice attached to its input, runs the reverb insert, and applies the bus
// volume. It is written here rather than assembled from stock nodes because miniaudio ships filters and a splitter
// but no gain node and no reverb — and because a mixer whose three buses are one class with one processor is easier
// to audit than three graphs. wet = 0 is an exact dry pass-through (the reverb is added, not crossfaded), which is
// what keeps every pre-mixer sound byte-identical to how it mixed before.
//
// MA_NODE_FLAG_CONTINUOUS_PROCESSING keeps the callback firing after the voices have ended, so a reverb tail keeps
// ringing into silence instead of being cut at the last source frame.
struct BusNode {
    ma_node_base base;   // must be first: the struct is handed to miniaudio as a ma_node*.
    ma_uint32 channels = kChannels;
    float volume = 1.0f;
    float wet = 0.0f;
    float combBuf[2][4][kMaxCombLen] = {};
    float combStore[2][4] = {};
    int combPos[2][4] = {};
    float apBuf[2][2][kMaxAllpassLen] = {};
    int apPos[2][2] = {};
};

void busProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut,
                ma_uint32* pFrameCountOut) {
    BusNode* b = reinterpret_cast<BusNode*>(pNode);
    const float* in = ppFramesIn[0];
    float* out = ppFramesOut[0];
    ma_uint32 frames = *pFrameCountOut;
    if (in && *pFrameCountIn < frames) frames = *pFrameCountIn;

    const ma_uint32 ch = b->channels;
    const float vol = b->volume;
    const float wet = b->wet;
    for (ma_uint32 f = 0; f < frames; ++f) {
        for (ma_uint32 c = 0; c < ch; ++c) {
            const float x = in ? in[f * ch + c] : 0.0f;
            float y = x;
            if (wet > 1.0e-4f) {
                float rev = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    int& pos = b->combPos[c][i];
                    float o = b->combBuf[c][i][pos];
                    b->combStore[c][i] = o * (1.0f - kCombDamp) + b->combStore[c][i] * kCombDamp;
                    b->combBuf[c][i][pos] = x + b->combStore[c][i] * kCombFeedback;
                    rev += o;
                    if (++pos == kCombLen[i]) pos = 0;
                }
                rev *= 0.25f;
                for (int i = 0; i < 2; ++i) {
                    int& pos = b->apPos[c][i];
                    const float o = b->apBuf[c][i][pos];
                    const float outAp = -kAllpassG * rev + o;
                    b->apBuf[c][i][pos] = rev + kAllpassG * outAp;
                    rev = outAp;
                    if (++pos == kAllpassLen[i]) pos = 0;
                }
                y = x + wet * rev;
            }
            out[f * ch + c] = y * vol;
        }
    }
    *pFrameCountIn = frames;
    *pFrameCountOut = frames;
}

// One input bus (the graph pre-mixes every attached voice into it), one output (to the endpoint), continuous.
ma_node_vtable g_busVtable = {busProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};

} // namespace

struct AudioEngine::Impl {
    ma_engine engine{};
    bool engineReady = false;
    bool noDevice = false;
    SoundId nextId = 1;

    // What load() registered, and the per-sound defaults every new voice of that sound starts from. The player
    // sets these before play() (component volume, loop, spatial), so they must outlive the load call.
    struct SoundDefaults {
        float volume = 1.0f;
        bool loop = false;
        bool spatial = false;
        glm::vec3 position{0.0f};
    };
    std::map<SoundId, std::string> files;
    std::map<SoundId, SoundDefaults> defaults;

    struct Voice {
        SoundId asset = 0;
        ma_sound* snd = nullptr;
        uint64_t seq = 0;
    };
    std::vector<Voice> voices;
    uint64_t nextSeq = 1;

    ma_sound* music = nullptr;
    std::string musicPath;   // the last music file, kept for pFilePath's lifetime

    BusNode buses[3] = {};   // index = int(Bus)
    int nodesBuilt = 0;      // how many of them ma_node_init accepted (the destructor uninitialises this many)
    float busVol[3] = {1.0f, 1.0f, 1.0f};
    float busWet[3] = {0.0f, 0.0f, 0.0f};

    ma_node* busNode(Bus bus) {
        return reinterpret_cast<ma_node*>(&buses[static_cast<int>(bus)]);
    }

    void destroyVoice(Voice& v) {
        ma_sound_stop(v.snd);
        ma_sound_uninit(v.snd);
        delete v.snd;
        v.snd = nullptr;
    }

    // Finished voices leave the mix. A stopped one leaves immediately; one that simply reached its end leaves on
    // the next reap, after miniaudio has taken it out of the graph.
    void reap() {
        for (size_t i = voices.size(); i-- > 0;) {
            if (ma_sound_at_end(voices[i].snd)) {
                ma_sound_uninit(voices[i].snd);
                delete voices[i].snd;
                voices[i] = voices.back();
                voices.pop_back();
            }
        }
    }

    void stealOldest() {
        if (voices.empty()) return;
        auto oldest = voices.begin();
        for (auto it = voices.begin(); it != voices.end(); ++it)
            if (it->seq < oldest->seq) oldest = it;
        ma_sound_stop(oldest->snd);
        ma_sound_uninit(oldest->snd);
        delete oldest->snd;
        *oldest = voices.back();
        voices.pop_back();
    }
};

AudioEngine::AudioEngine(bool noDevice) : impl_(new Impl) {
    impl_->noDevice = noDevice;
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = noDevice ? MA_TRUE : MA_FALSE;
    config.channels = kChannels;
    config.sampleRate = 48000;
    const ma_result r = ma_engine_init(&config, &impl_->engine);
    if (r != MA_SUCCESS) {
        error_ = std::string("could not start audio: ") + ma_result_description(r);
        return;
    }
    impl_->engineReady = true;
    ok_ = true;

    // The three buses: value-initialized (so the comb and allpass state is zero), initialised into the graph, and
    // attached straight to the endpoint. Sound nodes attach to the input side of the bus they play on.
    ma_node_graph* graph = ma_engine_get_node_graph(&impl_->engine);
    ma_uint32 channels[1] = {kChannels};
    for (BusNode& b : impl_->buses) {
        b = BusNode{};
        ma_node_config nc = ma_node_config_init();
        nc.vtable = &g_busVtable;
        nc.pInputChannels = channels;
        nc.pOutputChannels = channels;
        if (ma_node_init(graph, &nc, nullptr, reinterpret_cast<ma_node*>(&b)) != MA_SUCCESS) {
            error_ = "could not create a mixer bus";
            ok_ = false;
            return;
        }
        ++impl_->nodesBuilt;
        ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&b), 0, ma_node_graph_get_endpoint(graph), 0);
    }
}

AudioEngine::~AudioEngine() {
    if (!impl_) return;
    for (auto& v : impl_->voices) {
        ma_sound_uninit(v.snd);
        delete v.snd;
    }
    impl_->voices.clear();
    if (impl_->music) {
        ma_sound_uninit(impl_->music);
        delete impl_->music;
        impl_->music = nullptr;
    }
    if (impl_->engineReady) {
        for (int i = 0; i < impl_->nodesBuilt; ++i)
            ma_node_uninit(reinterpret_cast<ma_node*>(&impl_->buses[i]), nullptr);
        ma_engine_uninit(&impl_->engine);
    }
}

AudioEngine::SoundId AudioEngine::load(const std::string& path) {
    if (!ok_) return 0;
    // Prove the file opens and decodes before handing back an id: callers (and a project's sound components) treat
    // a non-zero id as "this file exists", which was load()'s contract when it kept the sound object alive. The
    // data itself is opened per voice instead, so N overlapping voices need no N cached decodes.
    ma_sound probe{};
    const ma_result r = ma_sound_init_from_file(&impl_->engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr,
                                                &probe);
    if (r != MA_SUCCESS) {
        error_ = "could not load " + path + ": " + ma_result_description(r);
        return 0;
    }
    ma_sound_uninit(&probe);
    const SoundId id = impl_->nextId++;
    impl_->files[id] = path;
    impl_->defaults[id] = Impl::SoundDefaults{};
    return id;
}

bool AudioEngine::play(SoundId id) { return playOn(id, Bus::Sfx); }

bool AudioEngine::playOn(SoundId id, Bus bus) {
    if (!ok_) return false;
    auto it = impl_->files.find(id);
    if (it == impl_->files.end()) return false;
    impl_->reap();
    while (impl_->voices.size() >= voiceLimit_ && !impl_->voices.empty()) impl_->stealOldest();

    ma_sound* snd = new ma_sound{};
    // The sound config is the way to put a sound on a custom node: pInitialAttachment is a ma_node*, which the
    // file-based helper's ma_sound_group* parameter is not. pFilePath points into the registered path, whose string
    // lives as long as the engine - nothing here keeps the caller's buffer.
    ma_sound_config cfg = ma_sound_config_init_2(&impl_->engine);
    cfg.pFilePath = it->second.c_str();
    cfg.pInitialAttachment = impl_->busNode(bus);
    cfg.initialAttachmentInputBusIndex = 0;
    const ma_result r = ma_sound_init_ex(&impl_->engine, &cfg, snd);
    if (r != MA_SUCCESS) {
        delete snd;
        error_ = "could not play " + it->second + ": " + ma_result_description(r);
        return false;
    }
    const Impl::SoundDefaults& d = impl_->defaults[id];
    ma_sound_set_volume(snd, d.volume);
    ma_sound_set_looping(snd, d.loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(snd, d.spatial ? MA_TRUE : MA_FALSE);
    if (d.spatial) ma_sound_set_position(snd, d.position.x, d.position.y, d.position.z);
    if (ma_sound_start(snd) != MA_SUCCESS) {
        ma_sound_uninit(snd);
        delete snd;
        return false;
    }
    impl_->voices.push_back(Impl::Voice{id, snd, impl_->nextSeq++});
    return true;
}

void AudioEngine::stop(SoundId id) {
    if (!ok_) return;
    for (size_t i = impl_->voices.size(); i-- > 0;) {
        if (impl_->voices[i].asset != id) continue;
        Impl::Voice v = impl_->voices[i];
        impl_->voices[i] = impl_->voices.back();
        impl_->voices.pop_back();
        impl_->destroyVoice(v);
    }
}

bool AudioEngine::isPlaying(SoundId id) const {
    if (!ok_) return false;
    for (const Impl::Voice& v : impl_->voices)
        if (v.asset == id && ma_sound_is_playing(v.snd)) return true;
    return false;
}

uint32_t AudioEngine::activeVoices() const {
    if (!ok_) return 0;
    return uint32_t(impl_->voices.size());
}

void AudioEngine::setVoiceLimit(uint32_t voices) { voiceLimit_ = std::max<uint32_t>(1, voices); }

void AudioEngine::setVolume(SoundId id, float volume) {
    if (auto it = impl_->defaults.find(id); it != impl_->defaults.end()) it->second.volume = volume;
    for (const Impl::Voice& v : impl_->voices)
        if (v.asset == id) ma_sound_set_volume(v.snd, volume);
}

void AudioEngine::setLooping(SoundId id, bool loop) {
    if (auto it = impl_->defaults.find(id); it != impl_->defaults.end()) it->second.loop = loop;
    for (const Impl::Voice& v : impl_->voices)
        if (v.asset == id) ma_sound_set_looping(v.snd, loop ? MA_TRUE : MA_FALSE);
}

void AudioEngine::setPosition(SoundId id, const glm::vec3& p) {
    if (auto it = impl_->defaults.find(id); it != impl_->defaults.end()) it->second.position = p;
    for (const Impl::Voice& v : impl_->voices) {
        if (v.asset != id) continue;
        ma_sound_set_position(v.snd, p.x, p.y, p.z);
    }
}

void AudioEngine::setSpatial(SoundId id, bool spatial) {
    if (auto it = impl_->defaults.find(id); it != impl_->defaults.end()) it->second.spatial = spatial;
    for (const Impl::Voice& v : impl_->voices) {
        if (v.asset != id) continue;
        ma_sound_set_spatialization_enabled(v.snd, spatial ? MA_TRUE : MA_FALSE);
        if (spatial) {
            const Impl::SoundDefaults& d = impl_->defaults[id];
            ma_sound_set_position(v.snd, d.position.x, d.position.y, d.position.z);
        }
    }
}

void AudioEngine::setListener(const glm::vec3& position, const glm::vec3& forward) {
    if (!ok_) return;
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 1.0f, 0.0f);
}

void AudioEngine::setBusVolume(Bus bus, float volume) {
    volume = std::clamp(volume, 0.0f, 4.0f);
    const int i = int(bus);
    impl_->busVol[i] = volume;
    if (ok_) impl_->buses[i].volume = volume;   // plain float: the audio thread reads it without a lock (by design)
}

float AudioEngine::busVolume(Bus bus) const { return impl_->busVol[int(bus)]; }

void AudioEngine::setBusReverb(Bus bus, float wet) {
    wet = std::clamp(wet, 0.0f, 1.0f);
    const int i = int(bus);
    impl_->busWet[i] = wet;
    if (ok_) impl_->buses[i].wet = wet;
}

float AudioEngine::busReverb(Bus bus) const { return impl_->busWet[int(bus)]; }

bool AudioEngine::playMusic(const std::string& path, bool loop, float fadeInSeconds) {
    if (!ok_) return false;
    if (impl_->music) {
        ma_sound_stop(impl_->music);
        ma_sound_uninit(impl_->music);
        delete impl_->music;
        impl_->music = nullptr;
    }
    ma_sound* snd = new ma_sound{};
    impl_->musicPath = path;   // kept, so the config's pFilePath outlives this call on every platform
    ma_sound_config cfg = ma_sound_config_init_2(&impl_->engine);
    cfg.pFilePath = impl_->musicPath.c_str();
    cfg.pInitialAttachment = impl_->busNode(Bus::Music);
    cfg.initialAttachmentInputBusIndex = 0;
    const ma_result r = ma_sound_init_ex(&impl_->engine, &cfg, snd);
    if (r != MA_SUCCESS) {
        delete snd;
        error_ = "could not play music " + path + ": " + ma_result_description(r);
        return false;
    }
    ma_sound_set_looping(snd, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(snd, 1.0f);   // the Music bus is the volume; the voice sits at unity
    if (fadeInSeconds > 0.0f) {
        const ma_uint64 frames = ma_uint64(fadeInSeconds * double(ma_engine_get_sample_rate(&impl_->engine)));
        ma_sound_set_fade_in_pcm_frames(snd, 0.0f, -1.0f, frames);   // -1 = "to the sound's own volume"
    }
    if (ma_sound_start(snd) != MA_SUCCESS) {
        ma_sound_uninit(snd);
        delete snd;
        return false;
    }
    impl_->music = snd;
    return true;
}

void AudioEngine::stopMusic(float fadeOutSeconds) {
    if (!ok_ || !impl_->music) return;
    if (fadeOutSeconds <= 0.0f) {
        ma_sound_stop(impl_->music);
        ma_sound_uninit(impl_->music);
        delete impl_->music;
        impl_->music = nullptr;
        return;
    }
    // Scheduled on the audio thread: the fade completes and the sound stops whether or not anyone ticks.
    const ma_uint64 frames = ma_uint64(fadeOutSeconds * double(ma_engine_get_sample_rate(&impl_->engine)));
    ma_sound_stop_with_fade_in_pcm_frames(impl_->music, frames);
}

bool AudioEngine::musicPlaying() const {
    return ok_ && impl_->music && ma_sound_is_playing(impl_->music);
}

void AudioEngine::setMasterVolume(float v) {
    master_ = v;
    if (ok_) ma_engine_set_volume(&impl_->engine, v);
}

void AudioEngine::tick() { impl_->reap(); }

uint64_t AudioEngine::pump(float* out, uint32_t frames) {
    if (!ok_ || !impl_->noDevice) return 0;
    impl_->reap();
    ma_uint64 done = 0;
    ma_engine_read_pcm_frames(&impl_->engine, out, frames, &done);
    return done;
}

} // namespace ukiyo
