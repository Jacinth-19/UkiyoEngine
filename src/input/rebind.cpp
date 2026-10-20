#include "input/rebind.h"

#include <algorithm>
#include <cctype>

namespace ukiyo::input {

std::string controlNameFromPath(const std::string& path) {
    DeviceKind kind;
    if (!deviceKindFromPath(path, &kind)) return path;
    const size_t slash = path.find('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

namespace {

// "leftShift" -> "Left Shift", "dpad/up" -> "Dpad Up", "f1" -> "F1".
std::string spacedWords(const std::string& text) {
    std::string out;
    bool newWord = true;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '/' || c == '-') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            newWord = true;
            continue;
        }
        if (newWord) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            newWord = false;
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(c)) && i > 0 &&
            std::islower(static_cast<unsigned char>(text[i - 1])))
            out += ' ';
        out += c;
    }
    return out;
}

} // namespace

std::string controlDisplayName(const std::string& path) {
    if (path.empty()) return path;
    DeviceKind kind;
    const bool hasDevice = deviceKindFromPath(path, &kind);
    const std::string control = controlNameFromPath(path);
    if (control.empty()) return path;

    // The names a person expects, where the control name is not one (Unity's InputControlPath.ToHumanReadableString
    // does the same with a display-name table per control).
    static const std::pair<const char*, const char*> kSpecial[] = {
        {"space", "Space"},
        {"enter", "Enter"},
        {"numpadEnter", "Numpad Enter"},
        {"escape", "Esc"},
        {"tab", "Tab"},
        {"backspace", "Backspace"},
        {"delete", "Del"},
        {"insert", "Ins"},
        {"pageUp", "Page Up"},
        {"pageDown", "Page Down"},
        {"upArrow", "Up Arrow"},
        {"downArrow", "Down Arrow"},
        {"leftArrow", "Left Arrow"},
        {"rightArrow", "Right Arrow"},
        {"leftShift", "Left Shift"},
        {"rightShift", "Right Shift"},
        {"leftCtrl", "Left Ctrl"},
        {"rightCtrl", "Right Ctrl"},
        {"leftAlt", "Left Alt"},
        {"rightAlt", "Right Alt"},
        {"leftMeta", "Left Meta"},
        {"rightMeta", "Right Meta"},
        {"capsLock", "Caps Lock"},
        {"scrollLock", "Scroll Lock"},
        {"printScreen", "Print Screen"},
        {"buttonSouth", "South"},
        {"buttonEast", "East"},
        {"buttonWest", "West"},
        {"buttonNorth", "North"},
        {"leftStick", "Left Stick"},
        {"rightStick", "Right Stick"},
        {"leftStickButton", "Left Stick Button"},
        {"rightStickButton", "Right Stick Button"},
        {"dpad", "D-pad"},
        {"dpad/up", "D-pad Up"},
        {"dpad/down", "D-pad Down"},
        {"dpad/left", "D-pad Left"},
        {"dpad/right", "D-pad Right"},
        {"leftButton", "Left Button"},
        {"rightButton", "Right Button"},
        {"middleButton", "Middle Button"},
        {"delta", "Mouse Delta"},
        {"position", "Mouse Position"},
        {"scroll", "Mouse Wheel"},
    };
    for (const auto& entry : kSpecial)
        if (control == entry.first) return entry.second;

    // Everything else is camel case: "leftTrigger" -> "Left Trigger", "numpad5" -> "Numpad5", "a" -> "A".
    std::string name = spacedWords(control);
    if (hasDevice && kind == DeviceKind::Mouse && name.compare(0, 5, "Mouse") != 0) return "Mouse " + name;
    return name.empty() ? path : name;
}

// ------------------------------------------------------------------------------------------------ RebindOperation

struct RebindOperation::Core {
    RebindOperation* owner = nullptr;
    State* state = nullptr;
    ActionMap* map = nullptr;
    Action* action = nullptr;
    size_t binding = 0;
    RebindOperation::Status status = RebindOperation::Status::Idle;
    std::string path;
    std::string previous;
    std::string previousName;
    bool previousComposite = false;
    bool previousPart = false;
    float timeout = 0.0f;
    float elapsed = 0.0f;
    bool installed = false;
    bool inCallback = false;
    bool restorePending = false;
    std::function<void(const std::string&)> savedRebind;
    std::function<void()> savedCanceled;

    // Puts back whatever the state had before start(). It never runs while one of the handlers below is on the stack:
    // assigning to a std::function that is currently executing is undefined behaviour, so a request from inside a
    // callback is deferred until the callback returns.
    void restoreCallbacks() {
        if (!installed || !state) return;
        if (inCallback) {
            restorePending = true;
            return;
        }
        state->onRebind = savedRebind;
        state->onRebindCanceled = savedCanceled;
        installed = false;
        restorePending = false;
    }
};

RebindOperation::RebindOperation() = default;

RebindOperation::~RebindOperation() {
    if (!core_) return;
    // Order matters: the handlers check `owner`, so clearing it first makes any call that arrives during the teardown a
    // no-op instead of a use-after-free.
    core_->owner = nullptr;
    if (core_->inCallback) {
        core_->restorePending = true;
        return;   // the stacked handler restores the callbacks when it unwinds
    }
    if (core_->status == Status::Waiting && core_->state && core_->state->rebinding()) core_->state->cancelRebind();
    core_->restoreCallbacks();
}

bool RebindOperation::start(State& state, const std::string& mapName, const std::string& actionName,
                            size_t bindingIndex, bool anyDevice, float timeout) {
    if (core_) {
        cancel();
        core_->restoreCallbacks();
    }
    ActionMap* map = state.map(mapName);
    if (!map) return false;
    Action* action = map->find(actionName);
    if (!action || bindingIndex >= action->bindings.size()) return false;

    auto core = std::make_shared<Core>();
    core->owner = this;
    core->state = &state;
    core->map = map;
    core->action = action;
    core->binding = bindingIndex;
    core->previous = action->bindings[bindingIndex].path;
    core->previousName = action->bindings[bindingIndex].name;
    core->previousComposite = action->bindings[bindingIndex].isComposite;
    core->previousPart = action->bindings[bindingIndex].isPartOfComposite;
    core->timeout = timeout;
    core->savedRebind = state.onRebind;
    core->savedCanceled = state.onRebindCanceled;
    core->installed = true;
    core->status = Status::Waiting;

    const std::weak_ptr<Core> weak = core;
    state.onRebind = [weak](const std::string& captured) {
        const auto self = weak.lock();
        if (!self) return;
        self->inCallback = true;
        self->status = RebindOperation::Status::Applied;
        self->path = captured;
        if (self->owner && self->owner->onApplied) self->owner->onApplied(captured);
        self->inCallback = false;
        if (self->restorePending) self->restoreCallbacks();
    };
    state.onRebindCanceled = [weak] {
        const auto self = weak.lock();
        if (!self) return;
        self->inCallback = true;
        self->status = RebindOperation::Status::Canceled;
        if (self->owner && self->owner->onCanceled) self->owner->onCanceled();
        self->inCallback = false;
        if (self->restorePending) self->restoreCallbacks();
    };

    core_ = core;
    state.startRebind(*map, *action, bindingIndex, anyDevice);
    return true;
}

void RebindOperation::cancel() {
    if (!core_ || core_->status != Status::Waiting) return;
    core_->status = Status::Canceled;
    const bool active = core_->state && core_->state->rebinding();
    core_->restoreCallbacks();
    if (active) core_->state->cancelRebind();
    if (onCanceled) onCanceled();
}

void RebindOperation::tick(float dt) {
    if (!core_ || core_->status != Status::Waiting) return;
    core_->elapsed += dt;
    if (core_->timeout <= 0.0f || core_->elapsed < core_->timeout) return;
    core_->status = Status::TimedOut;
    const bool active = core_->state && core_->state->rebinding();
    core_->restoreCallbacks();
    if (active) core_->state->cancelRebind();
    if (onCanceled) onCanceled();
}

void RebindOperation::finish() {
    if (core_) core_->restoreCallbacks();
}

RebindOperation::Status RebindOperation::status() const { return core_ ? core_->status : Status::Idle; }

const std::string& RebindOperation::path() const {
    static const std::string empty;
    return core_ ? core_->path : empty;
}

std::string RebindOperation::display() const { return controlDisplayName(path()); }

const std::string& RebindOperation::previousPath() const {
    static const std::string empty;
    return core_ ? core_->previous : empty;
}

float RebindOperation::elapsed() const { return core_ ? core_->elapsed : 0.0f; }

std::vector<RebindConflict> RebindOperation::conflicts() const {
    std::vector<RebindConflict> out;
    if (!core_ || !core_->state || core_->path.empty()) return out;
    for (const ActionMap& map : core_->state->maps()) {
        for (const Action& action : map.actions) {
            for (size_t i = 0; i < action.bindings.size(); ++i) {
                if (&map == core_->map && &action == core_->action && i == core_->binding) continue;
                if (action.bindings[i].path != core_->path) continue;
                out.push_back(RebindConflict{map.name, action.name, action.bindings[i].path});
            }
        }
    }
    return out;
}

void RebindOperation::revert() {
    if (!core_ || !core_->action || core_->status != Status::Applied) return;
    Binding& binding = core_->action->bindings[core_->binding];
    binding.path = core_->previous;
    binding.name = core_->previousName;
    binding.isComposite = core_->previousComposite;
    binding.isPartOfComposite = core_->previousPart;
    if (core_->state) core_->state->resolveBindings();
    core_->status = Status::Idle;
    core_->path.clear();
}

} // namespace ukiyo::input
