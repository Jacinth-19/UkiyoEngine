// Rebinding, the layer above the capture: a session with a result, a timeout, a conflict list, an undo, and control
// names a person can read.
//
// State::startRebind is the mechanism (input.h § "interactive rebinding"): it watches the devices, waits for a control
// to cross the press point, and writes the new path into the binding. What a UI needs on top of that is bookkeeping —
// is it still waiting, what did it capture, who else already owns that control, and how do I put it back — and that is
// this file. The editor's Input settings page is the first caller (editor/src/render_worker.cpp, on the running
// world); a game that wants a rebinding screen uses the same class against its own State.
#pragma once

#include "input/input.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ukiyo::input {

// The control part of a path: "<Keyboard>/space" -> "space", "<Gamepad>/dpad/up" -> "dpad/up". A path with no device
// prefix is returned unchanged, so callers can hand in whatever a project file stored.
std::string controlNameFromPath(const std::string& path);

// A control path as a person reads it, for a rebinding prompt: "<Keyboard>/space" -> "Space",
// "<Keyboard>/leftShift" -> "Left Shift", "<Gamepad>/buttonSouth" -> "South", "<Gamepad>/dpad/up" -> "D-pad Up",
// "<Mouse>/leftButton" -> "Left Button". A path this build cannot name comes back unchanged rather than empty.
std::string controlDisplayName(const std::string& path);

// What a binding cell's text means, resolved to what to store. A control path ("<Gamepad>/buttonSouth",
// "<Mouse>/leftButton") is kept as written once its device and control are known; anything else is key text
// ("W", "Space", "LeftShift", "PgUp") and becomes that key's keyboard path. Returns false and fills *error with a
// sentence for the person who typed it when the text is empty, names no device, names a control that device does not
// have, or is not a single key. This is the rule the editor's table and ProjectSettings.json share, kept out of the
// widget so it can be tested without one.
bool resolveBindingText(const std::string& text, std::string* stored, std::string* error);

// Another action that already uses the control being bound. `map` is the action's map.
struct RebindConflict {
    std::string map;
    std::string action;
    std::string path;
};

// Unity's InputActionRebindingExtensions.RebindingOperation, cut to what an overlay needs.
//
//     RebindOperation op;
//     op.onApplied  = [](const std::string& path) { ... };
//     op.onCanceled = [] { ... };
//     op.start(state, "Gameplay", "Jump");
//     // each frame, after state.update(dt):
//     op.tick(dt);
//
// The capture still happens inside State::update — the caller drives the state, which is what makes this usable with
// a real backend, a probe state fed from a dialog, or a test. The operation installs its own handlers on the state's
// onRebind/onRebindCanceled slots, chains to whatever was there before, and puts them back when it finishes.
//
// It is safe to destroy the operation at any point, including from its own callbacks: the installed handlers hold a
// weak reference, so a late call finds nothing to do rather than freed memory.
class RebindOperation {
public:
    enum class Status {
        Idle,      // never started, or finished and cleared
        Waiting,   // started, no control has been pressed yet
        Applied,   // a control was captured and written into the binding
        Canceled,  // cancel() was called, or the state's rebind was canceled from outside
        TimedOut,  // the timeout set in start() expired
    };

    RebindOperation();
    ~RebindOperation();
    RebindOperation(const RebindOperation&) = delete;
    RebindOperation& operator=(const RebindOperation&) = delete;

    // Starts on `bindingIndex` of (map, action). `anyDevice` lets a keyboard binding capture a gamepad button and the
    // other way round, which is what an overlay wants; with it false the capture stays within the binding's device
    // kind, as State::startRebind does. A timeout above 0 cancels the wait after that many seconds of tick().
    // Returns false when the map, the action or the binding does not exist.
    bool start(State& state, const std::string& map, const std::string& action, size_t bindingIndex = 0,
               bool anyDevice = true, float timeout = 0.0f);

    // Stops waiting. The binding is left as it was.
    void cancel();

    // The caller's frame, after it has updated the state. Enforces the timeout and delivers a deferred callback
    // restore; it does not touch the state's values.
    void tick(float dt);

    // Detaches from the state without changing the result: for a UI that wants to read the path after closing.
    void finish();

    Status status() const;
    bool running() const { return status() == Status::Waiting; }

    // The captured path on Applied ("<Gamepad>/buttonSouth"), otherwise empty.
    const std::string& path() const;
    // controlDisplayName(path()).
    std::string display() const;
    // The path the binding had before start(), for revert().
    const std::string& previousPath() const;
    float elapsed() const;

    // Other actions bound to the captured path, in every map of the state. Empty while nothing is captured, or when
    // the control is free. The action being rebound is not in the list.
    std::vector<RebindConflict> conflicts() const;

    // Puts the previous path back into the binding. The undo of an applied rebind; it does not restart the wait.
    void revert();

    std::function<void(const std::string& path)> onApplied;
    std::function<void()> onCanceled;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};

} // namespace ukiyo::input
