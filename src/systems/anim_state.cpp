#include "systems/anim_state.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ukiyo {

namespace {
constexpr float kTwoPi = 6.28318530718f;

// Maps an angle into [0, 2*pi).
float wrapPositive(float a) {
    a = std::fmod(a, kTwoPi);
    return a < 0.0f ? a + kTwoPi : a;
}
} // namespace

const char* blendKindName(BlendKind kind) {
    switch (kind) {
    case BlendKind::None: return "clip";
    case BlendKind::Linear1D: return "linear1d";
    case BlendKind::SimpleDirectional2D: return "simple2d";
    case BlendKind::FreeformCartesian2D: return "freeform2d";
    case BlendKind::FreeformDirectional2D: return "freeformdir2d";
    case BlendKind::Direct: return "direct";
    }
    return "clip";
}

BlendKind blendKindFromName(const std::string& name) {
    if (name == "linear1d") return BlendKind::Linear1D;
    if (name == "simple2d") return BlendKind::SimpleDirectional2D;
    if (name == "freeform2d") return BlendKind::FreeformCartesian2D;
    if (name == "freeformdir2d") return BlendKind::FreeformDirectional2D;
    if (name == "direct") return BlendKind::Direct;
    return BlendKind::None;
}

std::vector<float> directWeights(const std::vector<float>& values) {
    std::vector<float> w(values.size(), 0.0f);
    float sum = 0.0f;
    for (size_t i = 0; i < values.size(); ++i) {
        w[i] = std::max(values[i], 0.0f);
        sum += w[i];
    }
    if (sum > 0.0f)
        for (float& v : w) v /= sum;
    else
        std::fill(w.begin(), w.end(), 0.0f);
    return w;
}

std::vector<BlendMotion> spreadThresholds(std::vector<BlendMotion> motions) {
    const size_t n = motions.size();
    if (n < 2) return motions;
    float lo = motions[0].x, hi = motions[0].x;
    for (const BlendMotion& m : motions) {
        lo = std::min(lo, m.x);
        hi = std::max(hi, m.x);
    }
    // Keep the current order of the thresholds: the lowest existing X gets the first slot, and so on.
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return motions[a].x < motions[b].x; });
    for (size_t rank = 0; rank < n; ++rank)
        motions[order[rank]].x = lo + (hi - lo) * static_cast<float>(rank) / static_cast<float>(n - 1);
    return motions;
}

std::vector<float> blendWeights(BlendKind kind, const std::vector<BlendMotion>& m, float x, float y) {
    const size_t n = m.size();
    std::vector<float> w(n, 0.0f);
    if (n == 0) return w;
    if (kind == BlendKind::Direct) return w;   // Direct weights come from directWeights(values)

    if (kind == BlendKind::Linear1D) {
        std::vector<size_t> order(n);
        std::iota(order.begin(), order.end(), size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return m[a].x < m[b].x; });
        if (x <= m[order.front()].x) { w[order.front()] = 1.0f; return w; }
        if (x >= m[order.back()].x) { w[order.back()] = 1.0f; return w; }
        for (size_t k = 0; k + 1 < n; ++k) {
            const size_t a = order[k], b = order[k + 1];
            const float x0 = m[a].x, x1 = m[b].x;
            if (x >= x0 && x <= x1) {
                const float t = x1 > x0 ? (x - x0) / (x1 - x0) : 0.0f;
                w[a] = 1.0f - t;
                w[b] = t;
                return w;
            }
        }
        w[order.back()] = 1.0f;
        return w;
    }

    if (kind == BlendKind::SimpleDirectional2D) {
        // The motion at (0, 0) is the idle pose. Every other motion is a direction.
        size_t idle = n;
        std::vector<size_t> dirs;
        for (size_t i = 0; i < n; ++i) {
            if (m[i].x == 0.0f && m[i].y == 0.0f) {
                if (idle == n) idle = i;
            } else {
                dirs.push_back(i);
            }
        }
        if (dirs.empty()) {
            w[idle < n ? idle : 0] = 1.0f;
            return w;
        }
        const float len = std::sqrt(x * x + y * y);
        const float mag = std::min(len, 1.0f);
        if (dirs.size() == 1 || len < 1e-6f) {
            // One direction, or no direction: the single direction is used at full weight.
            if (len < 1e-6f && idle < n) w[idle] = 1.0f;
            else w[dirs[0]] = 1.0f;
        } else {
            std::vector<float> ang(n, 0.0f);
            for (size_t i : dirs) ang[i] = std::atan2(m[i].y, m[i].x);
            std::stable_sort(dirs.begin(), dirs.end(), [&](size_t a, size_t b) { return ang[a] < ang[b]; });
            const float a = std::atan2(y, x);
            const size_t d = dirs.size();
            bool placed = false;
            for (size_t k = 0; k < d && !placed; ++k) {
                const size_t p = dirs[k], q = dirs[(k + 1) % d];
                const float a0 = ang[p];
                float span = ang[q] - a0;
                if (k + 1 == d) span += kTwoPi;      // the last sector wraps past the first direction
                if (span <= 1e-6f) continue;         // two motions on the same direction: no sector
                const float rel = wrapPositive(a - a0);
                if (rel <= span) {
                    const float t = rel / span;
                    w[p] = 1.0f - t;
                    w[q] = t;
                    placed = true;
                }
            }
            if (!placed) w[dirs[0]] = 1.0f;
        }
        if (idle < n) {
            // Within the unit circle the directional mix is scaled by the length, and the rest is idle.
            for (size_t i : dirs) w[i] *= mag;
            w[idle] = 1.0f - mag;
        }
        return w;
    }

    if (kind == BlendKind::FreeformCartesian2D) {
        float sum = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const float dx = x - m[i].x, dy = y - m[i].y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < 1e-10f) {
                std::fill(w.begin(), w.end(), 0.0f);
                w[i] = 1.0f;
                return w;
            }
            w[i] = 1.0f / d2;
            sum += w[i];
        }
        for (float& v : w) v /= sum;
        return w;
    }

    if (kind == BlendKind::FreeformDirectional2D) {
        const float len = std::sqrt(x * x + y * y);
        const float a = std::atan2(y, x);
        float sum = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const float mx = m[i].x, my = m[i].y;
            const float r = std::sqrt(mx * mx + my * my);
            float dAng = 0.0f;
            if (r > 1e-6f) {
                // The smallest angle between the two directions, in [0, pi].
                dAng = std::fabs(wrapPositive(a - std::atan2(my, mx)));
                if (dAng > kTwoPi * 0.5f) dAng = kTwoPi - dAng;
            }
            const float dAngScaled = dAng * len;   // angle matters only as far as the input has length
            const float dLen = len - r;
            const float d2 = dAngScaled * dAngScaled + dLen * dLen;
            if (d2 < 1e-10f) {
                std::fill(w.begin(), w.end(), 0.0f);
                w[i] = 1.0f;
                return w;
            }
            w[i] = 1.0f / d2;
            sum += w[i];
        }
        for (float& v : w) v /= sum;
        return w;
    }

    w[0] = 1.0f;   // BlendKind::None has no motions to mix. The first clip is used as a plain clip.
    return w;
}

void AnimStateMachine::addState(const std::string& name, const std::string& clip) {
    states_[name] = State{clip, BlendKind::None, {}, {}, {}};
    if (default_.empty()) default_ = name;
}

void AnimStateMachine::addBlendState(const std::string& name, BlendKind kind, const std::string& paramX,
                                     const std::string& paramY, std::vector<BlendMotion> motions) {
    State s;
    s.kind = kind;
    s.paramX = paramX;
    s.paramY = paramY;
    s.motions = std::move(motions);
    states_[name] = std::move(s);
    if (default_.empty()) default_ = name;
}

void AnimStateMachine::addTransition(const std::string& from, const std::string& to, std::vector<Condition> when,
                                     float fadeSeconds, float minDwellSeconds, TransitionOptions opts) {
    transitions_list_.push_back(Transition{from, to, std::move(when), fadeSeconds, minDwellSeconds, opts});
}

float AnimStateMachine::durationOf(const std::string& clip) const {
    auto it = clipDur_.find(clip);
    return (it != clipDur_.end() && it->second > 0.0f) ? it->second : 1.0f;
}

float AnimStateMachine::stateDuration(const State& s) const {
    if (s.kind == BlendKind::None) return durationOf(s.clip);
    if (s.motions.empty()) return 1.0f;
    float sum = 0.0f;
    // A motion at speed k lasts 1/k of its clip length.
    for (const BlendMotion& m : s.motions) sum += durationOf(m.clip) / (m.timeScale > 0.0f ? m.timeScale : 1.0f);
    return sum / static_cast<float>(s.motions.size());
}

float AnimStateMachine::currentDuration() const {
    auto it = states_.find(current_);
    return it != states_.end() ? stateDuration(it->second) : 1.0f;
}

float AnimStateMachine::floatOr(const std::string& name, float fallback) const {
    if (name.empty()) return fallback;
    auto it = floats_.find(name);
    return it != floats_.end() ? it->second : fallback;
}

void AnimStateMachine::applyBlend(const State& s, float fade, const BlendFn& blend) const {
    if (!blend) return;
    std::vector<std::string> clips;
    std::vector<MotionOptions> options;
    clips.reserve(s.motions.size());
    options.reserve(s.motions.size());
    for (const BlendMotion& m : s.motions) {
        clips.push_back(m.clip);
        options.push_back(MotionOptions{m.timeScale, m.mirror});
    }
    std::vector<float> weights;
    if (s.kind == BlendKind::Direct) {
        std::vector<float> values;
        values.reserve(s.motions.size());
        for (const BlendMotion& m : s.motions) values.push_back(floatOr(m.param, 0.0f));
        weights = directWeights(values);
    } else {
        weights = blendWeights(s.kind, s.motions, floatOr(s.paramX, 0.0f), floatOr(s.paramY, 0.0f));
    }
    blend(clips, weights, options, fade);
}

bool AnimStateMachine::holds(const Condition& c) const {
    switch (c.op) {
    case Condition::Op::Greater: {
        auto it = floats_.find(c.param);
        return it != floats_.end() && it->second > c.value;
    }
    case Condition::Op::Less: {
        auto it = floats_.find(c.param);
        return it != floats_.end() && it->second < c.value;
    }
    case Condition::Op::Equal: {
        auto it = floats_.find(c.param);
        return it != floats_.end() && it->second == c.value;
    }
    case Condition::Op::IsTrue: {
        auto it = bools_.find(c.param);
        return it != bools_.end() && it->second;
    }
    case Condition::Op::IsFalse: {
        auto it = bools_.find(c.param);
        return it == bools_.end() || !it->second;
    }
    case Condition::Op::Trigger: {
        auto it = triggers_.find(c.param);
        return it != triggers_.end() && it->second;
    }
    }
    return false;
}

bool AnimStateMachine::allHold(const Transition& t) const {
    for (const Condition& c : t.when)
        if (!holds(c)) return false;
    return true;
}

// Exit time is reached on the frame where the playhead crosses it. For a value below 1 that happens once
// per loop, the same as Unity's per-loop evaluation. For 1 or more it happens once, after that many loops.
bool AnimStateMachine::exitTimeReached(const Transition& t, float prevDwell, float dwell, float clipLen) const {
    if (!t.opts.hasExitTime) return true;
    const double e = t.opts.exitTime;
    if (e <= 0.0) return true;
    const double prev = prevDwell / clipLen;
    const double now = dwell / clipLen;
    if (e >= 1.0) return prev < e && now >= e;
    return std::floor(now - e) > std::floor(prev - e);
}

void AnimStateMachine::enter(const std::string& name, float fade, float startFraction, const PlayFn& play,
                             const BlendFn& blend) {
    current_ = name;
    dwell_ = 0.0f;
    prevDwell_ = 0.0f;
    ++transitions_;
    auto it = states_.find(name);
    if (it == states_.end()) return;
    if (it->second.kind != BlendKind::None) {
        // A blend state crossfades in its weighted set. The start fraction applies to plain clips only.
        applyBlend(it->second, fade, blend);
        return;
    }
    if (play) play(it->second.clip, fade, startFraction);
}

void AnimStateMachine::update(float dt, const PlayFn& play, const BlendFn& blend) {
    if (!started_) {
        started_ = true;
        if (!default_.empty()) enter(default_, 0.0f, 0.0f, play, blend);
        return;
    }
    prevDwell_ = dwell_;
    dwell_ += dt;
    const float clipLen = currentDuration();
    for (const Transition& t : transitions_list_) {
        if (t.from != "*" && t.from != current_) continue;
        if (t.to == current_ && !t.opts.toSelf) continue;
        if (dwell_ < t.minDwell) continue;
        if (!exitTimeReached(t, prevDwell_, dwell_, clipLen)) continue;
        if (!allHold(t)) continue;
        // Consume the triggers that fired this transition, so each one acts once.
        for (const Condition& c : t.when)
            if (c.op == Condition::Op::Trigger) triggers_[c.param] = false;
        // A fractional crossfade is a share of the clip being left.
        const float fade = t.opts.fixedDuration ? t.fade : t.fade * clipLen;
        enter(t.to, fade, t.opts.offset, play, blend);
        return;
    }
    // A blend state that stays current follows its parameters, without restarting its clips.
    auto it = states_.find(current_);
    if (it != states_.end() && it->second.kind != BlendKind::None) applyBlend(it->second, -1.0f, blend);
    // A trigger stays set until a transition consumes it, so a request made during a dwell is not lost.
}

} // namespace ukiyo
