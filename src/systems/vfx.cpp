#include "systems/vfx.h"

#include <algorithm>

namespace ukiyo {

Emitter::Emitter(const EmitterSettings& s, const glm::vec3& position, uint32_t seed)
    : settings_(s), position_(position), rng_(seed ? seed : 1u) {}

float Emitter::random01() {
    // xorshift32: small, deterministic per seed, which keeps the tests repeatable.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return float(rng_ & 0xFFFFFF) / float(0x1000000);
}

void Emitter::spawnOne() {
    if (int(particles_.size()) >= settings_.maxParticles) return;
    Particle p;
    p.pos = position_;
    const glm::vec3 t(random01(), random01(), random01());
    p.vel = settings_.velocityMin + (settings_.velocityMax - settings_.velocityMin) * t;
    p.life = std::max(0.01f, settings_.lifetime);
    particles_.push_back(p);
}

void Emitter::spawnBurst(int count) {
    for (int i = 0; i < count; ++i) spawnOne();
    // A one-shot emitter spawns only what it is given, so it is done once the burst is emitted.
    if (!settings_.loop) spawnedAll_ = true;
}

void Emitter::update(float dt) {
    if (settings_.loop && !spawnedAll_) {
        accumulator_ += dt * settings_.rate;
        while (accumulator_ >= 1.0f) {
            accumulator_ -= 1.0f;
            spawnOne();
        }
    }
    for (Particle& p : particles_) {
        p.age += dt;
        p.vel += settings_.gravity * dt;
        p.pos += p.vel * dt;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.age >= p.life; }),
                     particles_.end());
}

void Emitter::instances(std::vector<ParticleInstance>* out) const {
    for (const Particle& p : particles_) {
        const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        const float size = settings_.sizeStart + (settings_.sizeEnd - settings_.sizeStart) * t;
        const glm::vec4 c = glm::mix(settings_.colorStart, settings_.colorEnd, t);
        ParticleInstance inst;
        inst.position[0] = p.pos.x;
        inst.position[1] = p.pos.y;
        inst.position[2] = p.pos.z;
        inst.position[3] = size;
        inst.color[0] = c.r;
        inst.color[1] = c.g;
        inst.color[2] = c.b;
        inst.color[3] = c.a;
        out->push_back(inst);
    }
}

VfxSystem::Handle VfxSystem::spawn(const EmitterSettings& s, const glm::vec3& position) {
    const Handle h = next_++;
    emitters_.emplace_back(h, Emitter(s, position, uint32_t(h) * 2654435761u + 1u));
    return h;
}

Emitter* VfxSystem::find(Handle h) {
    for (auto& [id, e] : emitters_)
        if (id == h) return &e;
    return nullptr;
}

void VfxSystem::setPosition(Handle h, const glm::vec3& p) {
    if (Emitter* e = find(h)) e->setPosition(p);
}

void VfxSystem::burst(Handle h, int count) {
    if (Emitter* e = find(h)) e->spawnBurst(count);
}

void VfxSystem::remove(Handle h) {
    emitters_.erase(std::remove_if(emitters_.begin(), emitters_.end(),
                                   [h](const auto& pair) { return pair.first == h; }),
                    emitters_.end());
}

void VfxSystem::update(float dt) {
    for (auto& [id, e] : emitters_) e.update(dt);
    // Finished one-shot emitters are dropped on their own; they need no explicit remove.
    emitters_.erase(std::remove_if(emitters_.begin(), emitters_.end(),
                                   [](const auto& pair) { return pair.second.finished(); }),
                    emitters_.end());
}

int VfxSystem::liveParticles() const {
    int n = 0;
    for (const auto& [id, e] : emitters_) n += e.liveCount();
    return n;
}

void VfxSystem::instances(std::vector<ParticleInstance>* out) const {
    for (const auto& [id, e] : emitters_) e.instances(out);
}

} // namespace ukiyo
