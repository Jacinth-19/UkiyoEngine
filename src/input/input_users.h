// Multi-player input: Unity's InputUser + PlayerInput, for an engine whose actions live in a State.
//
// The single-player model is one State holding devices, maps and actions together (§ input.h). That is the right shape
// for one player and the wrong one for four, because "Jump" has to mean four different things at once — one action
// object cannot be pressed by player 1 and released by player 2.
//
// So the split is Unity's:
//
//   * the *master* State owns the physical devices and the authored maps (the .inputactions asset);
//   * a *PlayerInput* is one player. It owns its own State, holding a copy of the maps and a mirror of only the
//     devices paired to it;
//   * InputSystem polls the master once a frame, copies each paired device's values into the players that own it, and
//     then updates each player's State. Everything downstream — phases, processors, interactions, callbacks — is the
//     single-player code, evaluated once per player.
//
// The mirror is what makes pairing mean something. A player whose devices are a gamepad has no keyboard in its State,
// so `<Keyboard>/w` resolves to nothing for that player: player 1 cannot press player 2's Jump. And it costs a copy of
// a few hundred floats a frame.
//
// What this covers from Unity's model:
//   InputUser          -> PlayerInput::devices(), pairDevice(), unpairDevice()
//   InputUser.index    -> PlayerInput::index()
//   PlayerInput        -> PlayerInput: one object per player, with its own action map state
//   notification modes -> the action callbacks in input.h (onStarted/onPerformed/onCanceled)
//   PlayerInputManager -> InputSystem::join(), leave(), autoJoin()
//   Join behaviour     -> JoinOptions: best fit of an unpaired device set, a named scheme, or an explicit device
//
// What it does not cover is documented in docs/INPUT.md §11, and it is deliberately short: split keyboard (two players
// on one keyboard) needs Unity's device-index control paths, which this asset parser does not speak.
#pragma once

#include "input/input.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ukiyo::input {

// How a player joins. Unity's PlayerInputManager exposes the same three choices as "Join Behaviour".
struct JoinOptions {
    // A control scheme to pair with ("Keyboard&Mouse", "Gamepad"). Empty means: try every scheme in the asset, in
    // order, and take the first set of devices that is free. A scheme whose devices are all taken is skipped.
    std::string controlScheme;
    // Explicit devices. When non-empty this wins over the scheme: the player is paired with exactly these, and a
    // device that is already paired is refused unless `allowDuplicates` is set.
    std::vector<Device*> devices;
    // Unity's InputUserPairingOptions.UnpairCurrentDevicesFromUser, roughly: allow one device to feed two players.
    // Off by default, because the failure it prevents — player 2 pressing player 1's button — is silent.
    bool allowDuplicates = false;
};

class InputSystem;

// One player: its index, what it is holding, and its own copy of the action maps.
class PlayerInput {
public:
    PlayerInput(InputSystem* owner, int index);

    int index() const { return index_; }
    InputSystem* system() const { return owner_; }

    // The devices this player owns, as the master's objects (not the mirrors).
    const std::vector<Device*>& devices() const { return devices_; }
    bool paired() const { return !devices_.empty(); }
    bool owns(const Device* device) const;

    // Pairing. pairDevice refuses a device that belongs to another player unless duplicates are allowed; it returns
    // false rather than silently leaving two players on one stick.
    bool pairDevice(Device* device);
    void unpairDevice(Device* device);
    void unpairAll();
    // Pairing changes rebuild the mirror and re-resolve every binding, so a player that gains a keyboard starts
    // reading `<Keyboard>/…` bindings on the same frame.
    void rebuild();

    const std::string& controlScheme() const { return scheme_; }
    void setControlScheme(const std::string& scheme);

    // This player's actions. Values, phases and callbacks are per player; the maps and their bindings are copies of
    // the master's. A State owns its devices and its bindings point into them, so a rebuild replaces it rather than
    // mutating it — which is why it is held by pointer.
    State& actions() { return *actions_; }
    const State& actions() const { return *actions_; }

    // The action map the player is currently driving. Switching disables the others, which is Unity's
    // PlayerInput.SwitchCurrentActionMap: a menu map and a gameplay map should not both be live on one player.
    void switchActionMap(const std::string& map);
    const std::string& currentActionMap() const { return currentMap_; }

    // Convenience: find an action in the current map, then in any enabled map.
    Action* action(const std::string& mapName, const std::string& actionName);

    std::function<void(PlayerInput&)> onJoined;
    std::function<void(PlayerInput&)> onLeft;
    // Fires when a device is paired or unpaired (Unity's InputUser.onChange, narrowed to the two events that matter).
    std::function<void(PlayerInput&, Device*)> onDevicePaired;
    std::function<void(PlayerInput&, Device*)> onDeviceUnpaired;

private:
    friend class InputSystem;
    InputSystem* owner_ = nullptr;
    int index_ = -1;
    std::string scheme_;
    std::vector<Device*> devices_;
    std::unique_ptr<State> actions_;
    std::string currentMap_;
};

// The manager: the master devices, the players, and the frame that feeds them.
class InputSystem {
public:
    // `master` is the State the backend polls and the one the asset is loaded into. Passing one lets a game that
    // already owns a State (the World does) keep it; leaving it null makes the manager own its own.
    explicit InputSystem(State* master = nullptr);
    ~InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    State& master() { return *master_; }
    const State& master() const { return *master_; }
    Device& addDevice(DeviceKind kind, int index = 0) { return master_->addDevice(kind, index); }

    // Players. join() returns nullptr when no device set can satisfy the options — a game that asked for a second
    // gamepad and has one gets null, which is the answer it needs, not an exception.
    PlayerInput* join(const JoinOptions& options = {}, std::string* error = nullptr);
    void leave(PlayerInput* player);
    void leaveIndex(int index);
    PlayerInput* player(int index);
    const std::vector<std::unique_ptr<PlayerInput>>& players() const { return players_; }
    int playerCount() const { return int(players_.size()); }

    // The player that owns a device, or null. Used by the auto-join search and by a game that wants to know whose
    // controller just rumbled.
    PlayerInput* ownerOf(const Device* device) const;

    // The map every joining player drives (Unity's PlayerInput.defaultActionMap). Without it a player joins with all
    // its maps disabled, which makes auto-join useless: the button that joined you would not do anything.
    void setDefaultActionMap(const std::string& name) { defaultActionMap_ = name; }
    const std::string& defaultActionMap() const { return defaultActionMap_; }

    // Unity's "Join Players When Button Is Pressed". When on, update() watches every unpaired device and joins a new
    // player on the first control that goes from released to held; `maxPlayers` caps it. The scheme is chosen by
    // JoinOptions::controlScheme if set, otherwise by the device that was pressed.
    void autoJoin(bool enabled, JoinOptions options = {}, int maxPlayers = 4);
    bool autoJoinEnabled() const { return autoJoin_; }
    std::function<void(PlayerInput&)> onPlayerJoined;

    // Frame. Polls the master once, mirrors every paired device into its owners, then updates each player's State in
    // index order. Calling master().update() as well would evaluate the master's maps a second time — the master's
    // maps are the template, not a player.
    void update(float dt);

    // When something else already updated the master this frame (World::tick does, because it also drives scripts),
    // turn this off so update() only mirrors and evaluates the players. The master must still have been updated
    // *before* this call, or the players read last frame's devices.
    void setAutoUpdateMaster(bool enabled) { updateMaster_ = enabled; }
    bool autoUpdateMaster() const { return updateMaster_; }

    // Re-copy the master's maps into every player. Needed after the asset changes (a rebind written back to the
    // master, a map added by a mod). Resets per-player action values, so call it at a load boundary, not mid-frame.
    void refreshMaps();

private:
    void mirrorInto(PlayerInput& player);
    PlayerInput* pickByScheme(const std::string& scheme, bool allowDuplicates, Device** pressed);

    State* owned_;
    State* master_;
    std::vector<std::unique_ptr<PlayerInput>> players_;
    int nextIndex_ = 1;   // Unity's player indices start at 1 in the UI

    bool updateMaster_ = true;
    std::string defaultActionMap_;
    bool autoJoin_ = false;
    JoinOptions autoJoinOptions_;
    int autoJoinMax_ = 4;
    // The previous frame's pressed state of every master control, so auto-join can see a press edge.
    std::vector<std::pair<Device*, std::string>> pressedSnapshot_;
    std::vector<float> pressedBefore_;
};

// Copies one device's control values into another of the same kind, creating any control the destination is missing.
// The mirror path of update(); public because a test — and a backend replaying a recorded session — needs it.
void copyDeviceValues(const Device& from, Device& to);

} // namespace ukiyo::input
