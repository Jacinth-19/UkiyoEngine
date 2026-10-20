#include "input/input_users.h"

#include <algorithm>
#include <cmath>

namespace ukiyo::input {

void copyDeviceValues(const Device& from, Device& to) {
    for (const auto& entry : from.controls()) {
        const Control& src = entry.second;
        Control* dst = to.find(entry.first);
        if (!dst) {
            // A control the mirror has not seen yet — a backend that added an axis after the player joined. Create it
            // with the source's type so the value is readable, and let the binding resolve on the next rebuild.
            dst = &to.control(entry.first, src.type);
        }
        dst->x = src.x;
        dst->xy = src.xy;
    }
}

// --------------------------------------------------------------------------------------------- PlayerInput

PlayerInput::PlayerInput(InputSystem* owner, int index) : owner_(owner), index_(index) {
    actions_ = std::make_unique<State>();
}

bool PlayerInput::owns(const Device* device) const {
    return std::find(devices_.begin(), devices_.end(), device) != devices_.end();
}

bool PlayerInput::pairDevice(Device* device) {
    if (!device || owns(device)) return device != nullptr;
    if (owner_ && owner_->ownerOf(device) && owner_->ownerOf(device) != this) return false;
    devices_.push_back(device);
    rebuild();
    if (onDevicePaired) onDevicePaired(*this, device);
    return true;
}

void PlayerInput::unpairDevice(Device* device) {
    const auto it = std::find(devices_.begin(), devices_.end(), device);
    if (it == devices_.end()) return;
    devices_.erase(it);
    rebuild();
    if (onDeviceUnpaired) onDeviceUnpaired(*this, device);
}

void PlayerInput::unpairAll() {
    const std::vector<Device*> had = devices_;
    devices_.clear();
    rebuild();
    for (Device* d : had)
        if (onDeviceUnpaired) onDeviceUnpaired(*this, d);
}

void PlayerInput::rebuild() {
    // The maps and their bindings are re-created from the master every time the device set changes, because the
    // resolved `Control*` pointers belong to the mirrors and the mirrors are new. What the player keeps is the action
    // map it was driving and any callbacks the game installed on the actions it holds — those the game reinstalls,
    // and `switchActionMap` below restores the map.
    std::string wanted = currentMap_;
    auto fresh = std::make_unique<State>();
    if (owner_) fresh->copyMapsFrom(owner_->master());
    for (Device* d : devices_) fresh->addDevice(d->kind(), d->index());
    if (!scheme_.empty()) fresh->setControlScheme(scheme_);
    fresh->setPoller(nullptr);
    actions_ = std::move(fresh);
    currentMap_.clear();
    if (!wanted.empty()) switchActionMap(wanted);
}

void PlayerInput::setControlScheme(const std::string& scheme) {
    scheme_ = scheme;
    if (!scheme_.empty()) actions_->setControlScheme(scheme_);
}

void PlayerInput::switchActionMap(const std::string& mapName) {
    if (!actions_->map(mapName)) return;
    for (ActionMap& m : actions_->maps()) {
        if (m.name == mapName) actions_->enableMap(m.name);
        else actions_->disableMap(m.name);
    }
    currentMap_ = mapName;
}

Action* PlayerInput::action(const std::string& mapName, const std::string& actionName) {
    if (mapName.empty()) {
        if (!currentMap_.empty())
            if (Action* a = actions_->action(currentMap_, actionName)) return a;
        for (ActionMap& m : actions_->maps())
            if (m.enabled)
                if (Action* a = m.find(actionName)) return a;
        return nullptr;
    }
    return actions_->action(mapName, actionName);
}

// --------------------------------------------------------------------------------------------- InputSystem

InputSystem::InputSystem(State* master) {
    if (master) {
        master_ = master;
        owned_ = nullptr;
    } else {
        owned_ = new State;
        master_ = owned_;
    }
}

InputSystem::~InputSystem() {
    players_.clear();
    delete owned_;   // players are gone first: their actions hold pointers into the master's devices
}

PlayerInput* InputSystem::ownerOf(const Device* device) const {
    for (const auto& p : players_)
        if (p->owns(device)) return p.get();
    return nullptr;
}

PlayerInput* InputSystem::player(int index) {
    for (const auto& p : players_)
        if (p->index() == index) return p.get();
    return nullptr;
}

PlayerInput* InputSystem::pickByScheme(const std::string& scheme, bool allowDuplicates, Device** pressed) {
    // 1. A device that was just pressed decides the scheme, when the caller did not pin one: press a gamepad button
    //    and you joined with the gamepad, whatever else is plugged in. This is Unity's "join with the device that
    //    was used" and it is why auto-join does not need the game to know which controller it was.
    std::vector<std::string> candidates;
    if (!scheme.empty()) candidates.push_back(scheme);
    else for (const ControlScheme& s : master_->controlSchemes) candidates.push_back(s.name);

    const auto freeFor = [&](Device* d) { return allowDuplicates || ownerOf(d) == nullptr; };
    const auto findFree = [&](DeviceKind kind) -> Device* {
        for (Device* d : master_->devices(kind))
            if (d->connected && d->enabled && freeFor(d)) return d;
        return nullptr;
    };

    for (const std::string& name : candidates) {
        if (pressed && *pressed) {
            // Only a scheme that this device is part of.
            bool fits = false;
            for (const ControlScheme& s : master_->controlSchemes)
                if (s.name == name &&
                    std::find(s.devices.begin(), s.devices.end(), (*pressed)->kind()) != s.devices.end())
                    fits = true;
            if (!fits) continue;
        }
        // Prefer the scheme that has a device of every kind it names. A scheme with a device already taken is
        // skipped unless duplicates are allowed, so four players fill four gamepads instead of all landing on one.
        std::vector<Device*> picks;
        bool ok = true;
        for (const DeviceKind kind : [&] {
                 std::vector<DeviceKind> kinds;
                 for (const ControlScheme& s : master_->controlSchemes)
                     if (s.name == name) kinds = s.devices;
                 return kinds;
             }()) {
            Device* d = nullptr;
            if (pressed && *pressed && (*pressed)->kind() == kind && freeFor(*pressed)) d = *pressed;
            if (!d) d = findFree(kind);
            if (!d) { ok = false; break; }
            if (std::find(picks.begin(), picks.end(), d) == picks.end()) picks.push_back(d);
        }
        if (!ok || picks.empty()) continue;
        for (Device* d : picks) {
            if (ownerOf(d) && !allowDuplicates) { ok = false; break; }
        }
        if (!ok) continue;
        PlayerInput* p = join([&] {
            JoinOptions o;
            o.allowDuplicates = allowDuplicates;
            o.devices = picks;
            return o;
        }());
        if (p) p->setControlScheme(name);
        return p;
    }
    return nullptr;
}

PlayerInput* InputSystem::join(const JoinOptions& options, std::string* error) {
    // An explicit device list is taken as given: the caller knows what it is doing, and a refusal here would be a
    // surprise in the middle of a console command.
    if (!options.devices.empty()) {
        for (Device* d : options.devices) {
            if (!d) continue;
            if (!options.allowDuplicates) {
                if (PlayerInput* other = ownerOf(d)) {
                    if (error) *error = "device already paired with player " + std::to_string(other->index());
                    return nullptr;
                }
            }
        }
        auto player = std::make_unique<PlayerInput>(this, nextIndex_++);
        PlayerInput* raw = player.get();
        players_.push_back(std::move(player));
        for (Device* d : options.devices)
            if (d) raw->devices_.push_back(d);
        if (!options.controlScheme.empty()) raw->scheme_ = options.controlScheme;
        raw->rebuild();
        if (raw->currentMap_.empty() && !defaultActionMap_.empty()) raw->switchActionMap(defaultActionMap_);
        if (raw->onJoined) raw->onJoined(*raw);
        if (onPlayerJoined) onPlayerJoined(*raw);
        return raw;
    }

    // No device named: the scheme decides. `pickByScheme` calls back into join with the devices it picked.
    if (!options.allowDuplicates) {
        PlayerInput* p = pickByScheme(options.controlScheme, false, nullptr);
        if (p) return p;
    } else {
        PlayerInput* p = pickByScheme(options.controlScheme, true, nullptr);
        if (p) return p;
    }

    // Last resort, Unity's "join with any device": a scheme may be absent from a small asset, and a gamepad plugged
    // in with no scheme authored for it should still be able to start a game.
    if (options.controlScheme.empty()) {
        for (Device* d : master_->all()) {
            if (!d->connected || !d->enabled) continue;
            if (!options.allowDuplicates && ownerOf(d)) continue;
            auto player = std::make_unique<PlayerInput>(this, nextIndex_++);
            PlayerInput* raw = player.get();
            players_.push_back(std::move(player));
            raw->devices_.push_back(d);
            raw->rebuild();
            if (raw->currentMap_.empty() && !defaultActionMap_.empty()) raw->switchActionMap(defaultActionMap_);
            if (raw->onJoined) raw->onJoined(*raw);
            if (onPlayerJoined) onPlayerJoined(*raw);
            return raw;
        }
    }
    if (error) *error = "no free device for the requested scheme";
    return nullptr;
}

void InputSystem::leave(PlayerInput* player) {
    if (!player) return;
    for (auto it = players_.begin(); it != players_.end(); ++it) {
        if (it->get() != player) continue;
        PlayerInput* raw = it->get();
        if (raw->onLeft) raw->onLeft(*raw);
        players_.erase(it);
        return;
    }
}

void InputSystem::leaveIndex(int index) { leave(player(index)); }

void InputSystem::autoJoin(bool enabled, JoinOptions options, int maxPlayers) {
    autoJoin_ = enabled;
    autoJoinOptions_ = std::move(options);
    autoJoinMax_ = std::max(1, maxPlayers);
    pressedSnapshot_.clear();
    pressedBefore_.clear();
}

void InputSystem::refreshMaps() {
    for (auto& p : players_) p->rebuild();
}

void InputSystem::mirrorInto(PlayerInput& player) {
    for (Device* source : player.devices_) {
        Device* mirror = player.actions_->device(source->kind(), source->index());
        if (!mirror) mirror = &player.actions_->addDevice(source->kind(), source->index());
        copyDeviceValues(*source, *mirror);
    }
}

void InputSystem::update(float dt) {
    // 1. The master latches and polls: the backend writes the physical devices exactly once, whatever the player
    //    count. This is the frame's only device read, unless the owner has already made it.
    if (updateMaster_) master_->update(dt);

    // 2. Auto-join looks for a press edge on a device nobody owns. It runs before the mirrors so a player that joins
    //    this frame already sees the button that joined it.
    if (autoJoin_ && playerCount() < autoJoinMax_) {
        if (pressedSnapshot_.empty()) {
            for (Device* d : master_->all())
                for (const auto& entry : d->controls()) pressedSnapshot_.emplace_back(d, entry.first);
            for (const auto& entry : pressedSnapshot_) {
                const Control* c = entry.first->find(entry.second);
                pressedBefore_.push_back(c ? c->x : 0.0f);
            }
        }
        Device* pressed = nullptr;
        for (size_t i = 0; i < pressedSnapshot_.size(); ++i) {
            Device* d = pressedSnapshot_[i].first;
            const Control* c = d->find(pressedSnapshot_[i].second);
            if (!c) continue;
            const float now = c->x;
            const float before = i < pressedBefore_.size() ? pressedBefore_[i] : 0.0f;
            if (now >= 0.5f && before < 0.5f && !ownerOf(d)) { pressed = d; break; }
        }
        for (size_t i = 0; i < pressedSnapshot_.size(); ++i) {
            const Control* c = pressedSnapshot_[i].first->find(pressedSnapshot_[i].second);
            if (i < pressedBefore_.size()) pressedBefore_[i] = c ? c->x : 0.0f;
        }
        if (pressed) {
            JoinOptions options = autoJoinOptions_;
            options.devices.clear();
            if (options.controlScheme.empty())
                pickByScheme(std::string(), options.allowDuplicates, &pressed);
            else
                pickByScheme(options.controlScheme, options.allowDuplicates, &pressed);
        }
    }

    // 3. Every player: copy its own devices in, then evaluate. A player whose device is absent keeps a zeroed mirror,
    //    which is exactly what "that player's controller is unplugged" should look like.
    for (auto& p : players_) {
        if (!p->paired()) continue;
        mirrorInto(*p);
        p->actions_->update(dt);
    }
}

} // namespace ukiyo::input
