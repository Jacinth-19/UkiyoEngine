// Input: devices, actions, bindings, processors and interactions — the engine's answer to Unity's Input System.
//
// The design is Unity's, because the asset format is (§ input_asset.h): an *action* is a named thing the game reads
// ("Move", "Jump"); a *binding* connects it to a *control* on a *device* (`<Keyboard>/space`) and may carry
// *processors* (invert, scale, deadzone) and *interactions* (press, hold, tap). Actions live in *action maps* that are
// enabled and disabled as a whole, so a menu map and a gameplay map never fight over the same key.
//
// This header is the whole public surface; there is no dependency on the renderer, on GLFW or on Vulkan, which is what
// lets tests/input_test.cpp drive it from synthetic devices on a machine with no window and no GPU.
//
// Update order, once a frame:
//     poll   — the backend writes each device's current control values (input_glfw.h for the real one)
//     update — latches the previous values, then evaluates every enabled action
//     read   — the game asks action.triggered() / action.value()
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ukiyo::input {

// What a single control produces.
enum class ControlType {
    Button,   // 0 or 1 (analogue buttons and triggers use Axis)
    Axis,     // -1..1, or 0..1 for a trigger
    Vector2,  // two axes, e.g. a stick or the mouse delta
};

// What an action produces; this mirrors Unity's InputActionType.
enum class ActionType {
    Button,       // digital: 0 or 1, with a press point
    Value,        // analogue or digital; one binding wins (the largest contribution)
    PassThrough,  // every binding contributes at once; no disambiguation
};

// Unity's InputActionPhase. `Waiting` is idle (disabled or not started).
enum class Phase { Waiting, Started, Performed, Canceled };

const char* toString(Phase p);
const char* toString(ActionType t);

// A control's state. `x` is the button/axis value; `xy` the vector. `prev*` are latched by State::update so a caller
// can ask "was this pressed this frame" without keeping its own copy.
struct Control {
    std::string name;                          // "space", "buttonSouth", "leftStick"
    ControlType type = ControlType::Button;
    float x = 0.0f;
    glm::vec2 xy{0.0f};
    float prevX = 0.0f;
    glm::vec2 prevXY{0.0f};

    // The value this control contributes for an action of the given expected type.
    float asFloat() const { return type == ControlType::Vector2 ? glm::length(xy) : x; }
    glm::vec2 asVector2() const { return type == ControlType::Vector2 ? xy : glm::vec2(x, 0.0f); }
};

enum class DeviceKind { Keyboard, Mouse, Gamepad };

const char* deviceName(DeviceKind k);   // "Keyboard", "Mouse", "Gamepad"
// The device kind a binding path starts with (`<Gamepad>/…`), or false when the path has no angle-bracket device.
bool deviceKindFromPath(const std::string& path, DeviceKind* out);

// Translates a key named the way Qt writes it ("Space", "W", "Left", "F1") to a Unity keyboard control name
// ("space", "w", "leftArrow", "f1"). The editor's project settings store portable key text; the runtime and the
// .inputactions format speak Unity control names, so this is the one place the two spellings meet.
bool controlNameFromPortableKey(const std::string& portable, std::string* out);

// The other direction, for writing a captured control back into a project setting: "leftShift" -> "LeftShift",
// "space" -> "Space", "f1" -> "F1". The inverse picks one spelling where controlNameFromPortableKey accepts several
// ("Return", not "Enter"), so a rebind produces stable text. Returns "" for a control with no keyboard spelling.
std::string portableKeyFromControlName(const std::string& control);

// One physical (or virtual) device. Devices are created by State::addDevice and are stable for its lifetime.
class Device {
public:
    Device(DeviceKind kind, int index);

    DeviceKind kind() const { return kind_; }
    int index() const { return index_; }
    const std::string& name() const { return name_; }   // "Keyboard", "Gamepad 2"
    bool connected = true;
    bool enabled = true;

    // Creates the control if it does not exist, so a backend can just write values.
    Control& control(const std::string& name, ControlType type);
    Control* find(const std::string& name);
    const std::map<std::string, Control>& controls() const { return controls_; }
    std::map<std::string, Control>& controls() { return controls_; }

    // What a backend calls. Writing a value does not latch prev; State::update does, once per frame.
    void setButton(const std::string& name, bool down);
    void setAxis(const std::string& name, float v);
    void setVector2(const std::string& name, glm::vec2 v);

private:
    DeviceKind kind_;
    int index_;
    std::string name_;
    std::map<std::string, Control> controls_;
};

// A processor changes a value on its way out of a binding: "invert", "scale(factor=2)", "deadzone(min=0.2,max=0.8)".
// Parsed from the binding's `processors` string; unknown names are reported by parseProcessors' out-parameter rather
// than silently ignored.
class Processor {
public:
    std::string name;
    std::map<std::string, float> params;

    float apply(float v) const;
    glm::vec2 apply(glm::vec2 v) const;
    // Does this processor operate on a single axis (scale, invert, deadzone on |v|) or on a vector?
    bool isVector() const;
};

// Parses Unity's semicolon-separated processor list: "invert;scale(factor=2)".
std::vector<Processor> parseProcessors(const std::string& text, std::vector<std::string>* unknown = nullptr);

// One interaction: "press", "hold(duration=0.4)", "tap(duration=0.2)", "slowTap(duration=0.5)", "multiTap(tapCount=2)".
struct Interaction {
    enum class Kind { Press, Hold, Tap, SlowTap, MultiTap } kind = Kind::Press;
    float duration = 0.0f;   // Hold / Tap / SlowTap threshold, seconds
    int tapCount = 1;        // MultiTap
    std::string raw;         // the text it was parsed from, for a round trip
};

std::vector<Interaction> parseInteractions(const std::string& text, std::vector<std::string>* unknown = nullptr);

// A binding: one control (or one composite of several) feeding one action.
struct Binding {
    std::string name;                          // "WASD", or the control name for a plain binding
    std::string id;                            // GUID from the asset
    std::string path;                          // "<Keyboard>/w", or "2DVector" for a composite
    std::string action;                        // action name this binding feeds
    std::string groups;                        // control-scheme binding groups, "Keyboard&Mouse"
    std::string processors;                    // raw text, kept for the round trip
    std::string interactions;                  // raw text, likewise
    bool isComposite = false;                  // path is "2DVector" / "1DAxis" / "Dpad"
    bool isPartOfComposite = false;            // a part of the composite above it
    int compositeIndex = -1;                   // runtime: index of the composite this part belongs to

    // Runtime state.
    std::vector<Processor> processorList;
    std::vector<Interaction> interactionList;
    float holdTime = 0.0f;
    int tapCount = 0;
    float lastTapTime = 1e9f;
    bool startedThisFrame = false;
    bool performedThisFrame = false;
    bool canceledThisFrame = false;
    bool down = false;                         // past the action's press point this frame
    bool prevDown = false;
    bool interactionDone = false;              // Hold has reached its threshold; cleared on the next press
    bool interactionFailed = false;            // a Tap ran past its duration; cleared on the next press
    float prevMag = 0.0f;                      // for the analogue "changed" test
    bool hasValue = false;                     // false when the binding's device is absent
    float outValue = 0.0f;                     // this binding's contribution after processors
    glm::vec2 outVector{0.0f};
    Control* control = nullptr;                // resolved device control, or null when the device is absent
    Device* device = nullptr;
};

// An action: what the game reads.
class Action {
public:
    std::string name;
    std::string id;                    // GUID from the asset, kept so a round trip does not churn the file
    ActionType type = ActionType::Button;
    std::string expectedControlType;   // "Button", "Axis", "Vector2" — metadata, as in the asset
    std::string processors;            // action-level processors, run after each binding's own
    std::string interactions;          // action-level interactions, added to each binding's own
    float pressPoint = 0.5f;           // threshold for a Button action
    bool enabled = true;               // mirrors the owning map

    // Runtime value. `value` is the float reading, `value2` the vector; both are kept so a caller can read either
    // without a templated accessor, and so a test can compare against the merged result directly.
    float value = 0.0f;
    glm::vec2 value2{0.0f};
    float prevValue = 0.0f;
    glm::vec2 prevValue2{0.0f};
    Phase phase = Phase::Waiting;
    Phase prevPhase = Phase::Waiting;
    bool startedThisFrame = false;   // a press, or the first non-zero frame of an analogue value

    std::vector<Binding> bindings;

    // Unity's action callbacks (action.performed += ...), the alternative to polling triggered(). They fire from
    // State::update, in started → performed → canceled order, on the same frame the polling accessors would report.
    // A callback must not add or remove actions or maps: the update is iterating them.
    std::function<void(const Action&)> onStarted;
    std::function<void(const Action&)> onPerformed;
    std::function<void(const Action&)> onCanceled;

    // True on the frame the action started performing (a button press, a tap, a hold that completed).
    bool triggered() const { return phase == Phase::Performed && prevPhase != Phase::Performed; }
    // True when the action is performing now.
    bool performed() const { return phase == Phase::Performed; }
    // True on the frame the action started — the frame a button goes down, which for a plain press is also the frame
    // it performs. Unity reports Started and Performed on the same frame; so does this.
    bool started() const { return startedThisFrame; }
    // True on the frame the action was canceled (button up).
    bool canceled() const { return phase == Phase::Canceled && prevPhase != Phase::Canceled; }
    bool pressed() const { return value > 0.0f || phase == Phase::Started || phase == Phase::Performed; }
    bool held() const { return phase == Phase::Started || phase == Phase::Performed; }

    float readFloat() const { return value; }
    glm::vec2 readVector2() const { return value2; }
};

// A map of actions, enabled and disabled together. Gameplay and UI are the usual two.
//
// The actions live in a deque, not a vector, on purpose: ActionMap::add hands out a reference and gameplay code caches
// it (`Action* jump = ...`), so adding a second action to the same map must not move the first one. The same reason
// puts State's maps in a deque.
class ActionMap {
public:
    std::string name;
    std::string id;
    bool enabled = false;
    std::deque<Action> actions;

    Action* find(const std::string& actionName);
    const Action* find(const std::string& actionName) const;
    Action& add(const std::string& actionName, ActionType type);
    std::function<void(const std::string&)> onActionPerformed;   // optional callback, name of the action
};

// A control scheme: a named group of devices that a set of bindings belongs to ("Keyboard&Mouse", "Gamepad").
struct ControlScheme {
    std::string name;
    std::string bindingGroup;
    std::vector<DeviceKind> devices;
};

class State;

// A backend writes device state once a frame. The GLFW one is in input_glfw.h; tests pass a lambda.
using PollFn = std::function<void(State&)>;

// The whole runtime: devices, maps, the current control scheme, and the frame update.
class State {
public:
    State();
    ~State();
    // A State owns its devices and its bindings point into them, so a copy would double-free and dangle. Move is not
    // offered either: actions and maps are addressed by reference for their whole life.
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    // Devices. addDevice returns a stable reference; the engine keeps at most one keyboard and one mouse and up to
    // kMaxGamepads gamepads.
    Device& addDevice(DeviceKind kind, int index = 0);
    Device* device(DeviceKind kind, int index = 0);
    Device* firstDevice(DeviceKind kind);
    std::vector<Device*> devices(DeviceKind kind);
    std::vector<Device*>& all() { return devices_; }
    const std::vector<Device*>& all() const { return devices_; }

    // Maps.
    ActionMap& addMap(const std::string& name);
    ActionMap* map(const std::string& name);
    std::string assetName = "Ukiyo Controls";   // the .inputactions asset's name, written back on save
    std::deque<ActionMap>& maps() { return maps_; }
    const std::deque<ActionMap>& maps() const { return maps_; }
    Action* action(const std::string& mapName, const std::string& actionName);
    const Action* action(const std::string& mapName, const std::string& actionName) const;

    // Enables a map and resolves its bindings against the connected devices.
    void enableMap(const std::string& name);
    void disableMap(const std::string& name);
    void disableAllMaps();

    // Control schemes. With a scheme set, bindings whose `groups` is non-empty and does not contain the scheme's
    // binding group are ignored; a binding with no groups always applies (Unity's rule).
    void setControlScheme(const std::string& name);
    const std::string& controlScheme() const { return schemeName_; }
    std::vector<ControlScheme> controlSchemes;

    // Frame.
    void setPoller(PollFn fn) { poll_ = std::move(fn); }
    // Latches prev values, polls the backend, then evaluates every enabled action. dt drives Hold/Tap/MultiTap.
    void update(float dt);
    uint64_t frame() const { return frame_; }

    // Devices present when update last ran, used by tests and by the rebind path.
    Device* deviceForPath(const std::string& path);

    // Copies the *authoring* of another state: its maps, their actions and bindings, and its control schemes. The
    // callbacks, the enabled flags and every runtime value are left alone — they belong to the receiver, which is the
    // whole point: four players share one asset and none of them shares a press. Used by InputSystem when a player
    // joins (see input_users.h).
    void copyMapsFrom(const State& other);

    // Interactive rebinding (Unity's PerformInteractiveRebinding): the next control pressed on any enabled device
    // whose kind matches the binding's device becomes the binding's path, and `onRebind` fires with it. `anyDevice`
    // drops the kind restriction, which is what an overlay wants: pressing a gamepad button while rebinding a
    // keyboard binding is a rebind to that button, not a non-event. (docs/INPUT.md §7)
    void startRebind(ActionMap& map, Action& action, size_t bindingIndex, bool anyDevice = false);
    void cancelRebind();
    bool rebinding() const { return rebind_.active; }
    std::function<void(const std::string&)> onRebind;
    std::function<void()> onRebindCanceled;

    // Re-resolves every binding of every map against the current devices. Called after adding/removing devices and
    // on enable; exposed because a backend that hot-plugs a gamepad needs it.
    void resolveBindings();

    // Reads a control by path, for tests and for code that wants a raw value.
    bool readPath(const std::string& path, glm::vec2* out);

private:
    friend class Device;
    void evaluateAction(Action& a, float dt);
    void evaluateBinding(Action& a, Binding& b, float dt);

    std::vector<Device*> devices_;
    std::deque<ActionMap> maps_;
    std::string schemeName_;
    PollFn poll_;
    uint64_t frame_ = 0;

    struct Rebind {
        bool active = false;
        ActionMap* map = nullptr;
        Action* action = nullptr;
        size_t index = 0;
        bool anyDevice = false;                // capture from every device kind, not just the binding's own
        std::map<std::string, float> before;   // control name -> value at the moment rebinding started
        std::vector<std::pair<Device*, std::string>> snapshot;   // every control, to detect a new press
    } rebind_;
};

// The canonical Unity control names this engine understands, per device. A backend only fills the ones it has.
// These are what a .inputactions asset from Unity uses, so importing one needs no translation table.
const std::vector<std::string>& keyboardControls();
const std::vector<std::string>& mouseControls();
const std::vector<std::string>& gamepadControls();

} // namespace ukiyo::input
