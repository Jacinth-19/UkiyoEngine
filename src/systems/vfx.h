// Particle effects. The CPU simulates particles; each emitter exposes its live particles as a flat
// instance list (position, size, colour) that a GPU instanced-quad pass can draw directly.
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

struct EmitterSettings {
    float rate = 20.0f;                       // particles per second while looping
    bool loop = true;
    int maxParticles = 512;
    float lifetime = 1.0f;
    glm::vec3 velocityMin{-0.5f, 1.0f, -0.5f};
    glm::vec3 velocityMax{0.5f, 2.0f, 0.5f};
    glm::vec3 gravity{0.0f, -2.0f, 0.0f};
    float sizeStart = 0.2f;
    float sizeEnd = 0.02f;
    glm::vec4 colorStart{1.0f, 0.8f, 0.3f, 1.0f};
    glm::vec4 colorEnd{1.0f, 0.2f, 0.0f, 0.0f};
};

// One entry per live particle, laid out for an instance buffer: xyz position and size, then rgba colour.
struct ParticleInstance {
    float position[4];   // xyz, size
    float color[4];      // rgba
};

class Emitter {
public:
    explicit Emitter(const EmitterSettings& s, const glm::vec3& position, uint32_t seed = 1);
    void update(float dt);
    void spawnBurst(int count);         // a burst, independent of the rate. Not named emit: Qt defines emit as a macro.
    void setPosition(const glm::vec3& p) { position_ = p; }
    glm::vec3 position() const { return position_; }
    int liveCount() const { return int(particles_.size()); }
    bool finished() const { return !settings_.loop && particles_.empty() && spawnedAll_; }
    void instances(std::vector<ParticleInstance>* out) const;
    void setSpawnedAll() { spawnedAll_ = true; }

private:
    struct Particle {
        glm::vec3 pos, vel;
        float age = 0.0f;
        float life = 1.0f;
    };
    float random01();
    void spawnOne();

    EmitterSettings settings_;
    glm::vec3 position_;
    std::vector<Particle> particles_;
    float accumulator_ = 0.0f;
    uint32_t rng_;
    bool spawnedAll_ = false;
};

// Owns emitters by handle. Handles are never reused while a session runs.
class VfxSystem {
public:
    using Handle = uint32_t;
    Handle spawn(const EmitterSettings& s, const glm::vec3& position);
    void setPosition(Handle h, const glm::vec3& p);
    void burst(Handle h, int count);
    void remove(Handle h);
    void update(float dt);
    Emitter* find(Handle h);
    size_t emitterCount() const { return emitters_.size(); }
    int liveParticles() const;
    // Instances from every emitter, in one list.
    void instances(std::vector<ParticleInstance>* out) const;

private:
    std::vector<std::pair<Handle, Emitter>> emitters_;
    Handle next_ = 1;
};

} // namespace ukiyo
