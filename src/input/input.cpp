#include "input/input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ukiyo::input {

const char* toString(Phase p) {
    switch (p) {
    case Phase::Waiting: return "Waiting";
    case Phase::Started: return "Started";
    case Phase::Performed: return "Performed";
    case Phase::Canceled: return "Canceled";
    }
    return "Waiting";
}

const char* toString(ActionType t) {
    switch (t) {
    case ActionType::Button: return "Button";
    case ActionType::Value: return "Value";
    case ActionType::PassThrough: return "PassThrough";
    }
    return "Button";
}

const char* deviceName(DeviceKind k) {
    switch (k) {
    case DeviceKind::Keyboard: return "Keyboard";
    case DeviceKind::Mouse: return "Mouse";
    case DeviceKind::Gamepad: return "Gamepad";
    }
    return "Device";
}

bool deviceKindFromPath(const std::string& path, DeviceKind* out) {
    if (path.size() < 3 || path[0] != '<') return false;
    const size_t close = path.find('>');
    if (close == std::string::npos) return false;
    const std::string name = path.substr(1, close - 1);
    if (name == "Keyboard") { if (out) *out = DeviceKind::Keyboard; return true; }
    if (name == "Mouse") { if (out) *out = DeviceKind::Mouse; return true; }
    if (name == "Gamepad" || name == "Joystick") { if (out) *out = DeviceKind::Gamepad; return true; }
    return false;
}

bool controlNameFromPortableKey(const std::string& portable, std::string* out) {
    if (portable.empty()) return false;
    // A project setting may already spell the control the way Unity does ("space", "leftShift"); that is the name a
    // rebind produces, so accepting it here keeps a hand-edited action and a rebound one identical.
    for (const std::string& control : keyboardControls())
        if (portable == control) {
            if (out) *out = portable;
            return true;
        }
    std::string name = portable;
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') name[0] = char(name[0] - 'A' + 'a');
    // Letters and digits are their own names; the punctuation keys are named after what they print, as Unity does.
    if (name.size() == 1) {
        const char c = name[0];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (out) *out = name;
            return true;
        }
        const char* punct = c == '`'   ? "backquote"
                            : c == '\''  ? "quote"
                            : c == ';'   ? "semicolon"
                            : c == ','   ? "comma"
                            : c == '.'   ? "period"
                            : c == '/'   ? "slash"
                            : c == '\\'  ? "backslash"
                            : c == '['   ? "leftBracket"
                            : c == ']'   ? "rightBracket"
                            : c == '-'   ? "minus"
                            : c == '='   ? "equals"
                                         : nullptr;
        if (punct) {
            if (out) *out = punct;
            return true;
        }
    }
    static const std::pair<const char*, const char*> kTable[] = {
        {"Space", "space"}, {"Return", "enter"}, {"Enter", "enter"}, {"Escape", "escape"}, {"Esc", "escape"},
        {"Tab", "tab"}, {"Backspace", "backspace"}, {"Delete", "delete"}, {"Del", "delete"}, {"Insert", "insert"},
        {"Ins", "insert"}, {"Home", "home"}, {"End", "end"}, {"PageUp", "pageUp"}, {"PgUp", "pageUp"},
        {"PageDown", "pageDown"}, {"PgDown", "pageDown"}, {"Left", "leftArrow"}, {"Right", "rightArrow"},
        {"Up", "upArrow"}, {"Down", "downArrow"}, {"Shift", "leftShift"}, {"LeftShift", "leftShift"},
        {"RightShift", "rightShift"}, {"Ctrl", "leftCtrl"}, {"LeftCtrl", "leftCtrl"}, {"RightCtrl", "rightCtrl"},
        {"Alt", "leftAlt"}, {"LeftAlt", "leftAlt"}, {"RightAlt", "rightAlt"}, {"Meta", "leftMeta"},
        {"LeftMeta", "leftMeta"}, {"RightMeta", "rightMeta"}, {"CapsLock", "capsLock"}, {"NumLock", "numLock"},
        {"Print", "printScreen"}, {"Pause", "pause"}, {"ScrollLock", "scrollLock"}, {"Backquote", "backquote"},
        {"Quote", "quote"},
        {"Semicolon", "semicolon"}, {"Comma", "comma"}, {"Period", "period"}, {"Slash", "slash"},
        {"Backslash", "backslash"}, {"BracketLeft", "leftBracket"}, {"BracketRight", "rightBracket"},
        {"Minus", "minus"}, {"Equal", "equals"}, {"Plus", "numpadPlus"},
    };
    for (const auto& entry : kTable) {
        if (portable == entry.first) {
            if (out) *out = entry.second;
            return true;
        }
    }
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) {
            if (out) *out = "f" + std::to_string(n);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- control names

const std::vector<std::string>& keyboardControls() {
    // Unity's Keyboard layout names. The list is what a .inputactions asset can reference, and it is what the GLFW
    // backend fills every frame.
    static const std::vector<std::string> names = {
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
        "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
        "space", "enter", "tab", "backquote", "quote", "semicolon", "comma", "period", "slash", "backslash",
        "leftBracket", "rightBracket", "minus", "equals",
        "leftShift", "rightShift", "leftCtrl", "rightCtrl", "leftAlt", "rightAlt", "leftMeta", "rightMeta",
        "escape", "capsLock", "backspace", "insert", "delete", "home", "end", "pageUp", "pageDown",
        "upArrow", "downArrow", "leftArrow", "rightArrow",
        "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        "printScreen", "scrollLock", "pause", "numLock",
        "numpad0", "numpad1", "numpad2", "numpad3", "numpad4", "numpad5", "numpad6", "numpad7", "numpad8", "numpad9",
        "numpadEnter", "numpadPlus", "numpadMinus", "numpadMultiply", "numpadDivide", "numpadPeriod",
    };
    return names;
}

const std::vector<std::string>& mouseControls() {
    static const std::vector<std::string> names = {"leftButton", "rightButton", "middleButton", "position", "delta",
                                                   "scroll", "forwardButton", "backButton"};
    return names;
}

const std::vector<std::string>& gamepadControls() {
    static const std::vector<std::string> names = {
        "buttonSouth", "buttonEast", "buttonWest", "buttonNorth",
        "leftShoulder", "rightShoulder", "leftTrigger", "rightTrigger", "leftStickButton", "rightStickButton",
        "start", "select", "guide",
        "leftStick", "rightStick", "dpad",
        "dpad/up", "dpad/down", "dpad/left", "dpad/right",
    };
    return names;
}

// ---------------------------------------------------------------- processors

namespace {

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool isVectorProcessor(const std::string& n) {
    return n == "normalizeVector2" || n == "scaleVector2" || n == "clampVector2" || n == "invertVector2";
}

} // namespace

bool Processor::isVector() const { return isVectorProcessor(name); }

float Processor::apply(float v) const {
    auto get = [&](const char* key, float fallback) {
        auto it = params.find(key);
        return it == params.end() ? fallback : it->second;
    };
    if (name == "invert") return -v;
    if (name == "scale") return v * get("factor", 1.0f);
    if (name == "clamp") {
        const float lo = get("min", 0.0f), hi = get("max", 1.0f);
        return std::clamp(v, lo, hi);
    }
    if (name == "normalize") {
        // Unity: Normalize(min,max) remaps [min,max] onto [0,1] without clamping.
        const float lo = get("min", 0.0f), hi = get("max", 1.0f);
        const float span = hi - lo;
        return std::fabs(span) < 1e-8f ? 0.0f : (v - lo) / span;
    }
    if (name == "axisDeadzone" || name == "deadzone") {
        const float lo = get("min", 0.125f), hi = get("max", 0.925f);
        const float a = std::fabs(v);
        if (a <= lo) return 0.0f;
        if (a >= hi) return v < 0.0f ? -1.0f : 1.0f;
        const float t = (a - lo) / std::max(1e-6f, hi - lo);
        return (v < 0.0f ? -1.0f : 1.0f) * t;
    }
    // Unknown processors are reported at parse time and pass the value through unchanged, so a typo degrades to
    // "no processing" rather than to a dead binding.
    return v;
}

glm::vec2 Processor::apply(glm::vec2 v) const {
    auto get = [&](const char* key, float fallback) {
        auto it = params.find(key);
        return it == params.end() ? fallback : it->second;
    };
    if (name == "normalizeVector2") {
        // Unity's NormalizeVector2 returns value.normalized, which scales *up* a short vector as well as down a long
        // one: an almost-centered stick reads full tilt. That is surprising, but it is what an asset built in Unity
        // expects, so it is preserved (docs/INPUT.md).
        const float len = glm::length(v);
        return len > 1e-8f ? v / len : v;
    }
    if (name == "scaleVector2") return v * glm::vec2(get("x", 1.0f), get("y", 1.0f));
    if (name == "clampVector2") {
        const float lo = get("min", -1.0f), hi = get("max", 1.0f);
        return glm::clamp(v, glm::vec2(lo), glm::vec2(hi));
    }
    if (name == "invertVector2") return -v;
    // A scalar processor on a vector applies to both components, which is what Unity does for invert/scale.
    if (name == "invert") return -v;
    if (name == "scale") return v * get("factor", 1.0f);
    if (name == "deadzone" || name == "stickDeadzone") {
        const float lo = get("min", 0.125f), hi = get("max", 0.925f);
        const float len = glm::length(v);
        if (len <= lo) return glm::vec2(0.0f);
        if (len >= hi) return glm::normalize(v);
        return v * ((len - lo) / std::max(1e-6f, hi - lo) / len);
    }
    return v;
}

std::vector<Processor> parseProcessors(const std::string& text, std::vector<std::string>* unknown) {
    std::vector<Processor> out;
    for (const std::string& pieceRaw : split(text, ';')) {
        const std::string piece = trim(pieceRaw);
        if (piece.empty()) continue;
        Processor p;
        const size_t open = piece.find('(');
        if (open == std::string::npos) {
            p.name = piece;
        } else {
            p.name = trim(piece.substr(0, open));
            const size_t close = piece.rfind(')');
            const std::string inner = piece.substr(open + 1, close == std::string::npos ? std::string::npos
                                                                                       : close - open - 1);
            for (const std::string& pairText : split(inner, ',')) {
                const size_t eq = pairText.find('=');
                if (eq == std::string::npos) continue;
                p.params[trim(pairText.substr(0, eq))] = std::strtof(trim(pairText.substr(eq + 1)).c_str(), nullptr);
            }
        }
        const bool known = p.name == "invert" || p.name == "scale" || p.name == "clamp" || p.name == "normalize" ||
                           p.name == "axisDeadzone" || p.name == "deadzone" || p.name == "stickDeadzone" ||
                           p.name == "normalizeVector2" || p.name == "scaleVector2" || p.name == "clampVector2" ||
                           p.name == "invertVector2";
        if (!known) {
            if (unknown) unknown->push_back(p.name);
            continue;   // not appended: a pass-through stub would only look like it did something
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Interaction> parseInteractions(const std::string& text, std::vector<std::string>* unknown) {
    std::vector<Interaction> out;
    for (const std::string& pieceRaw : split(text, ';')) {
        const std::string piece = trim(pieceRaw);
        if (piece.empty()) continue;
        Interaction it;
        it.raw = piece;
        const size_t open = piece.find('(');
        const std::string head = trim(piece.substr(0, open));
        if (head == "press" || head.empty()) it.kind = Interaction::Kind::Press;
        else if (head == "hold") it.kind = Interaction::Kind::Hold;
        else if (head == "tap") it.kind = Interaction::Kind::Tap;
        else if (head == "slowTap") it.kind = Interaction::Kind::SlowTap;
        else if (head == "multiTap") it.kind = Interaction::Kind::MultiTap;
        else {
            if (unknown) unknown->push_back(head);
            continue;
        }
        // Defaults follow Unity's: hold 0.4 s, tap 0.2 s, slow tap 0.5 s, multi tap 2 presses within 0.4 s.
        it.duration = it.kind == Interaction::Kind::Hold        ? 0.4f
                      : it.kind == Interaction::Kind::Tap       ? 0.2f
                      : it.kind == Interaction::Kind::SlowTap   ? 0.5f
                                                                 : 0.4f;
        it.tapCount = it.kind == Interaction::Kind::MultiTap ? 2 : 1;
        if (open != std::string::npos) {
            const size_t close = piece.rfind(')');
            const std::string inner = piece.substr(open + 1, close == std::string::npos ? std::string::npos
                                                                                       : close - open - 1);
            for (const std::string& pairText : split(inner, ',')) {
                const size_t eq = pairText.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = trim(pairText.substr(0, eq));
                const float v = std::strtof(trim(pairText.substr(eq + 1)).c_str(), nullptr);
                if (key == "duration" || key == "maxTapTime" || key == "tapTime") it.duration = v;
                else if (key == "tapCount") it.tapCount = std::max(1, int(v));
            }
        }
        out.push_back(it);
    }
    return out;
}

// ---------------------------------------------------------------- device

Device::Device(DeviceKind kind, int index) : kind_(kind), index_(index) {
    name_ = deviceName(kind);
    if (index > 0) name_ += " " + std::to_string(index + 1);
    // Every named control exists from the start with a zero value. A backend writes into them; a binding resolves
    // against them; a test can read them. Creating them lazily would make a binding to an untouched control null.
    const std::vector<std::string>* list = kind == DeviceKind::Keyboard ? &keyboardControls()
                                          : kind == DeviceKind::Mouse  ? &mouseControls()
                                                                       : &gamepadControls();
    ControlType type = ControlType::Button;
    for (const std::string& n : *list) {
        type = ControlType::Button;
        if (n == "position" || n == "delta") type = ControlType::Vector2;
        else if (n == "scroll") type = ControlType::Vector2;
        else if (n == "leftStick" || n == "rightStick" || n == "dpad") type = ControlType::Vector2;
        else if (n == "leftTrigger" || n == "rightTrigger") type = ControlType::Axis;
        controls_[n] = Control{n, type, 0.0f, glm::vec2(0.0f), 0.0f, glm::vec2(0.0f)};
    }
}

Control& Device::control(const std::string& name, ControlType type) {
    // A control's type is fixed when it is created. Writing a value must not retype it: a backend that calls
    // setButton on a stick (or a test that writes an analogue value into a key) would otherwise silently change how
    // every binding that resolves to that control reads it.
    auto it = controls_.find(name);
    if (it != controls_.end()) return it->second;
    Control& c = controls_[name];
    c.name = name;
    c.type = type;
    return c;
}

Control* Device::find(const std::string& name) {
    auto it = controls_.find(name);
    return it == controls_.end() ? nullptr : &it->second;
}

void Device::setButton(const std::string& name, bool down) { control(name, ControlType::Button).x = down ? 1.0f : 0.0f; }
void Device::setAxis(const std::string& name, float v) { control(name, ControlType::Axis).x = v; }
void Device::setVector2(const std::string& name, glm::vec2 v) {
    Control& c = control(name, ControlType::Vector2);
    c.xy = v;
    c.x = glm::length(v);
}

// ---------------------------------------------------------------- action / map

Action* ActionMap::find(const std::string& actionName) {
    for (Action& a : actions)
        if (a.name == actionName) return &a;
    return nullptr;
}

const Action* ActionMap::find(const std::string& actionName) const {
    for (const Action& a : actions)
        if (a.name == actionName) return &a;
    return nullptr;
}

Action& ActionMap::add(const std::string& actionName, ActionType type) {
    if (Action* existing = find(actionName)) return *existing;
    Action a;
    a.name = actionName;
    a.type = type;
    actions.push_back(std::move(a));
    return actions.back();
}

// ---------------------------------------------------------------- state

State::State() = default;
State::~State() {
    for (Device* d : devices_) delete d;
}

Device& State::addDevice(DeviceKind kind, int index) {
    for (Device* d : devices_)
        if (d->kind() == kind && d->index() == index) return *d;
    devices_.push_back(new Device(kind, index));
    resolveBindings();
    return *devices_.back();
}

Device* State::device(DeviceKind kind, int index) {
    for (Device* d : devices_)
        if (d->kind() == kind && d->index() == index) return d;
    return nullptr;
}

Device* State::firstDevice(DeviceKind kind) {
    for (Device* d : devices_)
        if (d->kind() == kind) return d;
    return nullptr;
}

std::vector<Device*> State::devices(DeviceKind kind) {
    std::vector<Device*> out;
    for (Device* d : devices_)
        if (d->kind() == kind) out.push_back(d);
    return out;
}

ActionMap& State::addMap(const std::string& name) {
    for (ActionMap& m : maps_)
        if (m.name == name) return m;
    maps_.push_back(ActionMap{});
    maps_.back().name = name;
    return maps_.back();
}

ActionMap* State::map(const std::string& name) {
    for (ActionMap& m : maps_)
        if (m.name == name) return &m;
    return nullptr;
}

Action* State::action(const std::string& mapName, const std::string& actionName) {
    ActionMap* m = map(mapName);
    return m ? m->find(actionName) : nullptr;
}

const Action* State::action(const std::string& mapName, const std::string& actionName) const {
    for (const ActionMap& m : maps_)
        if (m.name == mapName) return m.find(actionName);
    return nullptr;
}

namespace {

// Unity cancels an action when its map is disabled: the value returns to zero and a phase that was in progress
// reports one last Canceled rather than freezing at Performed. Freezing matters more here than in Unity, because
// triggered() is a phase transition, and an action that is no longer evaluated would otherwise report the same
// transition forever.
void resetActions(ActionMap& map) {
    map.enabled = false;
    for (Action& a : map.actions) {
        a.enabled = false;
        if (a.phase == Phase::Started || a.phase == Phase::Performed) a.phase = Phase::Canceled;
        a.value = 0.0f;
        a.value2 = glm::vec2(0.0f);
        for (Binding& b : a.bindings) {
            b.down = false;
            b.holdTime = 0.0f;
            b.tapCount = 0;
            b.interactionDone = false;
        }
    }
}

} // namespace

void State::enableMap(const std::string& name) {
    if (ActionMap* m = map(name)) {
        m->enabled = true;
        for (Action& a : m->actions) a.enabled = true;
        resolveBindings();
    }
}

void State::disableMap(const std::string& name) {
    if (ActionMap* m = map(name)) resetActions(*m);
}

void State::disableAllMaps() {
    for (ActionMap& m : maps_) resetActions(m);
}

void State::setControlScheme(const std::string& name) {
    schemeName_ = name;
    resolveBindings();
}

Device* State::deviceForPath(const std::string& path) {
    DeviceKind kind;
    if (!deviceKindFromPath(path, &kind)) return nullptr;
    return firstDevice(kind);
}

void State::resolveBindings() {
    // The scheme's binding group decides which grouped bindings apply. A binding with no groups always applies,
    // which is Unity's rule and the reason a keyboard binding works with no scheme set at all.
    std::string group;
    for (const ControlScheme& s : controlSchemes)
        if (s.name == schemeName_) group = s.bindingGroup;

    for (ActionMap& m : maps_) {
        for (Action& a : m.actions) {
            std::vector<Processor> processors;   // parsed once, shared by the parts of a composite? no: per binding
            int currentComposite = -1;
            for (size_t i = 0; i < a.bindings.size(); ++i) {
                Binding& b = a.bindings[i];
                if (b.isComposite) currentComposite = int(i);
                b.compositeIndex = b.isPartOfComposite ? currentComposite : -1;

                // The action's own processors and interactions run after the binding's, which is Unity's order.
                b.processorList = parseProcessors(b.processors + ";" + a.processors);
                b.interactionList = parseInteractions(b.interactions + ";" + a.interactions);

                const bool grouped = !b.groups.empty();
                const bool matches = !grouped || (group.empty() ? true : b.groups.find(group) != std::string::npos);
                b.control = nullptr;
                b.device = nullptr;
                if (b.isComposite || !matches) continue;
                if (b.path.empty()) continue;
                DeviceKind kind;
                if (!deviceKindFromPath(b.path, &kind)) continue;   // a composite name or a layout path
                Device* d = firstDevice(kind);
                if (!d || !d->enabled || !d->connected) continue;
                const size_t slash = b.path.find('/');
                if (slash == std::string::npos) continue;
                const std::string control = b.path.substr(slash + 1);
                b.control = d->find(control);
                b.device = d;
            }
        }
    }
}

bool State::readPath(const std::string& path, glm::vec2* out) {
    DeviceKind kind;
    if (!deviceKindFromPath(path, &kind)) return false;
    Device* d = firstDevice(kind);
    if (!d) return false;
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return false;
    Control* c = d->find(path.substr(slash + 1));
    if (!c) return false;
    if (out) *out = c->asVector2();
    return true;
}

void State::startRebind(ActionMap& map, Action& action, size_t bindingIndex) {
    if (bindingIndex >= action.bindings.size()) return;
    rebind_ = Rebind{};
    rebind_.active = true;
    rebind_.map = &map;
    rebind_.action = &action;
    rebind_.index = bindingIndex;
    // Snapshot every control so "the next new press" is unambiguous even if something is already held.
    for (Device* d : devices_) {
        if (!d->enabled) continue;
        for (auto& [name, c] : d->controls()) rebind_.snapshot.emplace_back(d, name);
    }
    rebind_.before.clear();
    for (auto& [d, name] : rebind_.snapshot) {
        Control* c = d->find(name);
        if (c) rebind_.before[d->name() + "/" + name] = std::max(std::fabs(c->x), glm::length(c->xy));
    }
}

void State::cancelRebind() {
    const bool was = rebind_.active;
    rebind_ = Rebind{};
    if (was && onRebindCanceled) onRebindCanceled();
}

namespace {

// The control a binding's device kind restricts it to, so rebinding a keyboard binding cannot capture a stick.
bool sameKind(const Binding& b, Device* d) {
    DeviceKind kind;
    if (!deviceKindFromPath(b.path, &kind)) return true;
    return d->kind() == kind;
}

} // namespace

void State::update(float dt) {
    // 1. Latch the previous values. Every "pressed this frame" test in the engine comes from this one place.
    for (Device* d : devices_)
        for (auto& entry : d->controls()) {
            Control& c = entry.second;
            c.prevX = c.x;
            c.prevXY = c.xy;
        }
    // 2. The backend writes this frame's values.
    if (poll_) poll_(*this);
    // 3. Interactive rebinding looks for a control that went from released to held this frame.
    if (rebind_.active && rebind_.action && rebind_.index < rebind_.action->bindings.size()) {
        const Binding& target = rebind_.action->bindings[rebind_.index];
        for (auto& [d, name] : rebind_.snapshot) {
            if (!d->enabled || !d->connected || !sameKind(target, d)) continue;
            Control* c = d->find(name);
            if (!c) continue;
            const float now = std::max(std::fabs(c->x), glm::length(c->xy));
            const auto it = rebind_.before.find(d->name() + "/" + name);
            const float was = it == rebind_.before.end() ? 0.0f : it->second;
            // A button counts at the press point; an axis at half travel, which is past any sane deadzone.
            const float threshold = c->type == ControlType::Button ? 0.5f : 0.5f;
            if (now >= threshold && was < threshold) {
                std::string path = "<" + std::string(deviceName(d->kind())) + ">/" + name;
                Binding& mutableBinding = rebind_.action->bindings[rebind_.index];
                mutableBinding.path = path;
                mutableBinding.isComposite = false;
                mutableBinding.isPartOfComposite = false;
                mutableBinding.name = name;
                const std::string captured = path;
                rebind_ = Rebind{};
                resolveBindings();
                if (onRebind) onRebind(captured);
                break;
            }
        }
    }
    // 4. Evaluate every enabled action, in map order. A disabled map still latches its previous phase, so an action
    //    that was mid-press when its map went away cannot keep reporting the press.
    for (ActionMap& m : maps_) {
        if (!m.enabled) {
            for (Action& a : m.actions) {
                a.prevPhase = a.phase;
                a.prevValue = a.value;
                a.prevValue2 = a.value2;
                a.startedThisFrame = false;
            }
            continue;
        }
        for (Action& a : m.actions) {
            evaluateAction(a, dt);
            // Callbacks see exactly the frames the polling accessors report: started, then performed, then canceled.
            if (a.startedThisFrame && a.onStarted) a.onStarted(a);
            if (a.triggered() && a.onPerformed) a.onPerformed(a);
            if (a.canceled() && a.onCanceled) a.onCanceled(a);
            if (m.onActionPerformed && a.triggered()) m.onActionPerformed(a.name);
        }
    }
    ++frame_;
}

void State::evaluateAction(Action& a, float dt) {
    a.prevValue = a.value;
    a.prevValue2 = a.value2;
    a.prevPhase = a.phase;
    a.startedThisFrame = false;

    float bestValue = 0.0f;
    glm::vec2 bestVector(0.0f);
    bool started = false, performed = false, canceled = false, heldPerformed = false;

    for (Binding& b : a.bindings) {
        if (b.isPartOfComposite) continue;   // consumed by its composite
        evaluateBinding(a, b, dt);
        started = started || b.startedThisFrame;
        performed = performed || b.performedThisFrame;
        canceled = canceled || b.canceledThisFrame;
        // A Hold that has reached its threshold stays Performed until release; that is what keeps performed() true
        // for a held key without re-reporting the transition every frame.
        if (b.hasValue && b.down && b.interactionDone) heldPerformed = true;
        if (!b.hasValue) continue;

        const float mag = std::max(std::fabs(b.outValue), glm::length(b.outVector));
        const float best = std::max(std::fabs(bestValue), glm::length(bestVector));
        if (a.type == ActionType::PassThrough) {
            // Every binding contributes; the one that just went down is the one that reads.
            if (b.down && !b.prevDown) { bestValue = b.outValue; bestVector = b.outVector; }
        } else if (mag >= best) {
            // Value and Button both take the strongest contribution. Binding order breaks ties, which is why the
            // order in the asset is preserved rather than sorted.
            bestValue = b.outValue;
            bestVector = b.outVector;
        }
    }

    a.value = bestValue;
    a.value2 = bestVector;

    if (a.type == ActionType::Button) {
        const float mag = std::max(std::fabs(a.value), glm::length(a.value2));
        const bool nowDown = mag >= a.pressPoint;
        a.value = nowDown ? 1.0f : 0.0f;
        if (!nowDown) a.value2 = glm::vec2(0.0f);
        // With an interaction attached, the interaction owns the phase: a Tap reads Started while the key is held
        // and Performed only on the release that satisfied it; a Hold reads Started until its threshold and then
        // Performed until release. A plain button's implicit press sets interactionDone while down, so it still
        // reads Performed for as long as it is held.
        if (performed || heldPerformed) a.phase = Phase::Performed;
        else if (canceled) a.phase = Phase::Canceled;
        else if (started || nowDown) a.phase = Phase::Started;
        else a.phase = Phase::Waiting;
    } else {
        const float mag = std::max(std::fabs(a.value), glm::length(a.value2));
        const float prevMag = std::max(std::fabs(a.prevValue), glm::length(a.prevValue2));
        if (mag > 0.0f && prevMag == 0.0f) a.phase = Phase::Started;
        else if (mag > 0.0f) a.phase = Phase::Performed;
        else if (prevMag > 0.0f) a.phase = Phase::Canceled;
        else a.phase = Phase::Waiting;
        if (performed && mag > 0.0f) a.phase = Phase::Performed;
    }
    // Started is reported on the same frame a press performs, as Unity does.
    a.startedThisFrame = started || (a.phase == Phase::Performed && a.prevPhase == Phase::Waiting);
}

void State::evaluateBinding(Action& a, Binding& b, float dt) {
    b.startedThisFrame = b.performedThisFrame = b.canceledThisFrame = false;
    b.prevDown = b.down;
    b.hasValue = false;
    b.outValue = 0.0f;
    b.outVector = glm::vec2(0.0f);

    // Gather the raw value: a plain control, or the parts of a composite.
    if (b.isComposite) {
        glm::vec2 sum(0.0f);
        bool any = false;
        for (Binding& part : a.bindings) {
            if (part.compositeIndex < 0 || &part == &b) continue;
            if (part.compositeIndex != int(&b - a.bindings.data())) continue;
            if (!part.control) continue;
            const std::string& n = part.name;
            float v = part.control->asFloat();
            for (const Processor& p : part.processorList) v = p.apply(v);
            const glm::vec2 vec = part.control->asVector2();
            if (n == "up" || n == "positive") sum.y += v;
            else if (n == "down" || n == "negative") sum.y -= v;
            else if (n == "left") sum.x -= v;
            else if (n == "right") sum.x += v;
            else if (n == "x" || n == "horizontal") sum.x += v;
            else if (n == "y" || n == "vertical") sum.y += v;
            else sum += vec;
            any = true;
        }
        if (!any) return;
        // A 1DAxis composite is one signed number (positive minus negative); a 2D composite's scalar reading is its
        // length. Without this, an action bound to E/Q would read the same value for forward and back.
        const bool oneD = b.path == "1DAxis" || b.path == "1DVector";
        if (oneD) {
            float value = b.path == "1DAxis" ? sum.y : sum.x;
            for (const Processor& p : b.processorList)
                if (!p.isVector()) value = p.apply(value);
            b.outValue = value;
            b.outVector = b.path == "1DAxis" ? glm::vec2(0.0f, value) : glm::vec2(value, 0.0f);
        } else {
            // The composite's own processors run on the summed vector, after each part's processors have run on its
            // control. This is where "normalizeVector2" belongs on a WASD binding.
            glm::vec2 value = sum;
            for (const Processor& p : b.processorList) value = p.apply(value);
            b.outVector = value;
            b.outValue = glm::length(value);
        }
    } else if (b.control) {
        b.outValue = b.control->asFloat();
        b.outVector = b.control->asVector2();
        for (const Processor& p : b.processorList) {
            if (p.isVector() || b.control->type == ControlType::Vector2) b.outVector = p.apply(b.outVector);
            else b.outValue = p.apply(b.outValue);
        }
        // Keep the two readings consistent: a vector control's scalar follows the processed vector, and a scalar
        // control's vector follows the processed scalar.
        if (b.control->type == ControlType::Vector2) b.outValue = glm::length(b.outVector);
        else b.outVector = glm::vec2(b.outValue, 0.0f);
    } else {
        b.down = false;
        return;
    }
    b.hasValue = true;

    // The digital reading that interactions work on.
    const float mag = std::max(std::fabs(b.outValue), glm::length(b.outVector));
    const float threshold = a.type == ActionType::Button ? a.pressPoint : 1e-4f;
    b.down = mag >= threshold;
    const float held = b.holdTime;
    if (b.down) b.holdTime += dt;
    else b.holdTime = 0.0f;
    // A new press starts a new interaction. Resetting on the press (not the release) lets the release branch below
    // still see that a tap already failed while it was held.
    if (b.down && !b.prevDown) {
        b.interactionDone = false;
        b.interactionFailed = false;
    }

    if (b.interactionList.empty()) {
        if (a.type == ActionType::Button) {
            if (b.down && !b.prevDown) { b.startedThisFrame = true; b.performedThisFrame = true; }
            else if (!b.down && b.prevDown) b.canceledThisFrame = true;
            b.interactionDone = b.down;   // an implicit press stays Performed while held
        } else {
            if (mag > 0.0f && !b.prevDown) b.startedThisFrame = true;
            if (mag != b.prevMag) b.performedThisFrame = true;
            if (mag <= 0.0f && b.prevDown) b.canceledThisFrame = true;
        }
    } else {
        for (const Interaction& it : b.interactionList) {
            switch (it.kind) {
            case Interaction::Kind::Press:
                if (b.down && !b.prevDown) { b.startedThisFrame = true; b.performedThisFrame = true; }
                else if (!b.down && b.prevDown) b.canceledThisFrame = true;
                b.interactionDone = b.down;
                break;
            case Interaction::Kind::Hold:
                if (b.down && !b.prevDown) b.startedThisFrame = true;
                if (b.down && b.holdTime >= it.duration && !b.interactionDone) {
                    b.performedThisFrame = true;
                    b.interactionDone = true;
                }
                // Releasing ends the hold whether or not it reached its threshold: performed in the first case,
                // canceled without performing in the second. Unity reports Canceled after a completed hold too.
                if (!b.down && b.prevDown) b.canceledThisFrame = true;
                break;
            case Interaction::Kind::Tap:
                if (b.down && !b.prevDown) b.startedThisFrame = true;
                // Unity cancels a tap the moment it has been held longer than its duration, not on release.
                if (b.down && b.holdTime > it.duration && !b.interactionFailed) {
                    b.canceledThisFrame = true;
                    b.interactionFailed = true;
                }
                if (!b.down && b.prevDown && !b.interactionFailed) {
                    if (held <= it.duration) b.performedThisFrame = true;
                    else b.canceledThisFrame = true;
                }
                break;
            case Interaction::Kind::SlowTap:
                if (b.down && !b.prevDown) b.startedThisFrame = true;
                if (!b.down && b.prevDown) {
                    if (held >= it.duration) b.performedThisFrame = true;
                    else b.canceledThisFrame = true;
                }
                break;
            case Interaction::Kind::MultiTap: {
                if (b.down && !b.prevDown) b.startedThisFrame = true;
                b.lastTapTime += dt;
                if (b.lastTapTime > it.duration && b.tapCount > 0) {
                    // The window closed before enough taps: Unity cancels the sequence.
                    b.tapCount = 0;
                    b.canceledThisFrame = true;
                }
                if (!b.down && b.prevDown) {
                    if (held <= it.duration) {
                        ++b.tapCount;
                        b.lastTapTime = 0.0f;
                        if (b.tapCount >= it.tapCount) { b.performedThisFrame = true; b.tapCount = 0; }
                    } else {
                        b.tapCount = 0;
                        b.canceledThisFrame = true;
                    }
                }
                break;
            }
            }
        }
    }
    b.prevMag = mag;
}

} // namespace ukiyo::input
