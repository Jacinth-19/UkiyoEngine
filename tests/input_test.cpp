// The input system, driven from synthetic devices (docs/INPUT.md).
//
// Nothing here opens a window or touches a GPU: the State takes a poll lambda, the tests write control values into it,
// and every assertion is about what a game would read. That is the whole point of the design — an input system whose
// behaviour can only be checked by holding a keyboard is an input system whose behaviour is untested.
//
// The groups below follow Unity's documented behaviour, because the asset format is Unity's and a team importing a
// .inputactions file expects the same edge cases to hold.
#include "input/input.h"
#include "input/input_asset.h"
#include "input/input_users.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace ukiyo::input;

namespace {

int gChecks = 0;
int gFailures = 0;

void checkTrue(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %s\n", what.c_str());
    } else {
        std::printf("ok    %s\n", what.c_str());
    }
}

void checkNear(float got, float want, float tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol;
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %-60s got %.4f want %.4f\n", what.c_str(), got, want);
    } else {
        std::printf("ok    %-60s %.4f\n", what.c_str(), got);
    }
}

void checkNear2(glm::vec2 got, glm::vec2 want, float tol, const std::string& what) {
    const bool ok = std::fabs(got.x - want.x) <= tol && std::fabs(got.y - want.y) <= tol;
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %-60s got (%.3f,%.3f) want (%.3f,%.3f)\n", what.c_str(), got.x, got.y, want.x, want.y);
    } else {
        std::printf("ok    %-60s (%.3f,%.3f)\n", what.c_str(), got.x, got.y);
    }
}

void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    const bool ok = got == want;
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL  %-60s got \"%s\" want \"%s\"\n", what.c_str(), got.c_str(), want.c_str());
    } else {
        std::printf("ok    %-60s \"%s\"\n", what.c_str(), got.c_str());
    }
}

// A state with a keyboard, a mouse and one gamepad, and a poller that does nothing — the tests write device values by
// hand, which is exactly what a backend does.
struct Fixture {
    State state;
    Device* keyboard = nullptr;
    Device* mouse = nullptr;
    Device* pad = nullptr;

    Fixture() {
        keyboard = &state.addDevice(DeviceKind::Keyboard, 0);
        mouse = &state.addDevice(DeviceKind::Mouse, 0);
        pad = &state.addDevice(DeviceKind::Gamepad, 0);
        state.setPoller([](State&) {});
    }
    void tick(float dt = 1.0f / 60.0f) { state.update(dt); }
};

// ---------------------------------------------------------------- 1. devices and paths

void testDevices() {
    std::printf("\n1. devices and control paths\n");
    Fixture f;
    checkTrue(f.keyboard->find("space") != nullptr, "the keyboard has a space control");
    checkTrue(f.pad->find("buttonSouth") != nullptr, "the gamepad has buttonSouth");
    checkTrue(f.mouse->find("delta") != nullptr, "the mouse has a delta control");
    checkTrue(f.mouse->find("delta")->type == ControlType::Vector2, "mouse delta is a Vector2 control");
    checkTrue(f.pad->find("leftTrigger")->type == ControlType::Axis, "a trigger is an Axis, not a Button");

    DeviceKind kind;
    checkTrue(deviceKindFromPath("<Gamepad>/leftStick", &kind) && kind == DeviceKind::Gamepad,
              "<Gamepad> parses to a gamepad");
    checkTrue(!deviceKindFromPath("2DVector", &kind), "a composite name is not a device path");

    // Reading through a path is how a caller asks for a raw value without an action.
    f.keyboard->setButton("w", true);
    f.tick();
    glm::vec2 v(0.0f);
    checkTrue(f.state.readPath("<Keyboard>/w", &v), "readPath resolves <Keyboard>/w");
    checkNear(v.x, 1.0f, 1e-4f, "readPath returns the control's value");
}

// ---------------------------------------------------------------- 2. button actions

void testButtons() {
    std::printf("\n2. button actions: press, hold, release\n");
    Fixture f;
    ActionMap& map = f.state.addMap("Player");
    Action& jump = map.add("Jump", ActionType::Button);
    jump.bindings.push_back(Binding{"", "", "<Keyboard>/space", "Jump", "", "", "", false, false, -1});
    f.state.enableMap("Player");

    f.tick();
    checkTrue(!jump.triggered() && !jump.pressed(), "idle: nothing triggered");

    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(jump.triggered(), "press: triggered() is true on the press frame");
    checkTrue(jump.started(), "press: started() is true on the same frame (Unity does both)");
    checkNear(jump.value, 1.0f, 1e-4f, "press: value is 1");

    f.tick();
    checkTrue(!jump.triggered(), "held: triggered() is false on the second frame");
    checkTrue(jump.performed(), "held: performed() stays true");
    checkNear(jump.value, 1.0f, 1e-4f, "held: value stays 1");

    f.keyboard->setButton("space", false);
    f.tick();
    checkTrue(jump.canceled(), "release: canceled() is true on the release frame");
    checkNear(jump.value, 0.0f, 1e-4f, "release: value is 0");
    checkTrue(!jump.pressed(), "release: not pressed");

    f.tick();
    checkTrue(!jump.canceled(), "the frame after release: canceled() is false again");
}

// ---------------------------------------------------------------- 3. value actions and composites

void testValueAndComposite() {
    std::printf("\n3. value actions and 2DVector composites\n");
    Fixture f;
    ActionMap& map = f.state.addMap("Player");
    Action& move = map.add("Move", ActionType::Value);
    move.expectedControlType = "Vector2";
    // The composite parent and its four parts, in the order Unity writes them.
    move.bindings.push_back(Binding{"WASD", "", "2DVector", "Move", "", "", "", true, false, -1});
    move.bindings.push_back(Binding{"up", "", "<Keyboard>/w", "Move", "", "", "", false, true, 0});
    move.bindings.push_back(Binding{"down", "", "<Keyboard>/s", "Move", "", "", "", false, true, 0});
    move.bindings.push_back(Binding{"left", "", "<Keyboard>/a", "Move", "", "", "", false, true, 0});
    move.bindings.push_back(Binding{"right", "", "<Keyboard>/d", "Move", "", "", "", false, true, 0});
    f.state.enableMap("Player");

    f.tick();
    checkNear2(move.readVector2(), glm::vec2(0.0f), 1e-4f, "no keys: Move is (0,0)");

    f.keyboard->setButton("w", true);
    f.tick();
    checkNear2(move.readVector2(), glm::vec2(0.0f, 1.0f), 1e-4f, "W alone: (0,1)");

    f.keyboard->setButton("d", true);
    f.tick();
    checkNear2(move.readVector2(), glm::vec2(1.0f, 1.0f), 1e-4f, "W+D: (1,1), not normalised");

    f.keyboard->setButton("w", false);
    f.tick();
    checkNear2(move.readVector2(), glm::vec2(1.0f, 0.0f), 1e-4f, "D alone: (1,0)");

    f.keyboard->setButton("d", false);
    f.tick();
    checkNear2(move.readVector2(), glm::vec2(0.0f), 1e-4f, "released: back to (0,0)");

    // A 1D axis (E/Q) is one signed number, not a length: back must read negative.
    Action& elev = map.add("Elevate", ActionType::Value);
    elev.expectedControlType = "Axis";
    elev.bindings.push_back(Binding{"EQ", "", "1DAxis", "Elevate", "", "", "", true, false, -1});
    elev.bindings.push_back(Binding{"positive", "", "<Keyboard>/e", "Elevate", "", "", "", false, true, 0});
    elev.bindings.push_back(Binding{"negative", "", "<Keyboard>/q", "Elevate", "", "", "", false, true, 0});
    // Processors on the composite itself run after the parts are summed. normalizeVector2 is the usual one: it keeps
    // a diagonal from moving at 1.41x speed. (Unity's version also scales short vectors up; see docs/INPUT.md.)
    Action& normMove = map.add("MoveNormalized", ActionType::Value);
    normMove.bindings.push_back(Binding{"WASD", "", "2DVector", "MoveNormalized", "", "normalizeVector2", "", true,
                                        false, -1});
    normMove.bindings.push_back(Binding{"up", "", "<Keyboard>/w", "MoveNormalized", "", "", "", false, true, 0});
    normMove.bindings.push_back(Binding{"down", "", "<Keyboard>/s", "MoveNormalized", "", "", "", false, true, 0});
    normMove.bindings.push_back(Binding{"left", "", "<Keyboard>/a", "MoveNormalized", "", "", "", false, true, 0});
    normMove.bindings.push_back(Binding{"right", "", "<Keyboard>/d", "MoveNormalized", "", "", "", false, true, 0});
    f.state.resolveBindings();
    f.keyboard->setButton("w", true);
    f.keyboard->setButton("d", true);
    f.tick();
    checkNear(glm::length(normMove.readVector2()), 1.0f, 1e-4f, "normalizeVector2 on a composite makes W+D unit length");
    checkNear2(normMove.readVector2(), glm::vec2(0.7071f), 1e-3f, "and keeps the diagonal's direction");
    f.keyboard->setButton("w", false);
    f.keyboard->setButton("d", false);

    f.state.resolveBindings();
    f.keyboard->setButton("e", true);
    f.tick();
    checkNear(elev.readFloat(), 1.0f, 1e-4f, "1D axis: E reads +1");
    f.keyboard->setButton("e", false);
    f.keyboard->setButton("q", true);
    f.tick();
    checkNear(elev.readFloat(), -1.0f, 1e-4f, "1D axis: Q reads -1");
    f.keyboard->setButton("q", false);

    // The same composite on a gamepad stick, which is one Vector2 control rather than four buttons.
    Action& look = map.add("Look", ActionType::Value);
    look.expectedControlType = "Vector2";
    look.bindings.push_back(Binding{"", "", "<Gamepad>/rightStick", "Look", "", "", "", false, false, -1});
    f.state.resolveBindings();
    f.pad->setVector2("rightStick", glm::vec2(0.5f, -0.25f));
    f.tick();
    checkNear2(look.readVector2(), glm::vec2(0.5f, -0.25f), 1e-4f, "right stick reads as a Vector2 action");
}

// ---------------------------------------------------------------- 4. processors

void testProcessors() {
    std::printf("\n4. processors\n");
    Fixture f;
    ActionMap& map = f.state.addMap("Player");
    Action& invert = map.add("Invert", ActionType::Value);
    invert.bindings.push_back(Binding{"", "", "<Gamepad>/leftStick", "Invert", "", "invert", "", false, false, -1});
    Action& norm = map.add("Norm", ActionType::Value);
    norm.bindings.push_back(
        Binding{"", "", "<Keyboard>/w", "Norm", "", "normalize(min=0,max=10)", "", false, false, -1});
    Action& dead = map.add("Dead", ActionType::Value);
    dead.bindings.push_back(
        Binding{"", "", "<Gamepad>/leftStick", "Dead", "", "stickDeadzone(min=0.2,max=0.9)", "", false, false, -1});
    Action& scaled = map.add("Scaled", ActionType::Value);
    scaled.bindings.push_back(Binding{"", "", "<Gamepad>/leftTrigger", "Scaled", "", "scale(factor=2)", "", false,
                                      false, -1});
    f.state.enableMap("Player");

    // A deadzone and an invert both on the same stick, read through three actions.
    f.pad->setVector2("leftStick", glm::vec2(0.5f, 0.0f));
    f.keyboard->setAxis("w", 5.0f);
    f.pad->setAxis("leftTrigger", 0.3f);
    f.tick();
    checkNear2(invert.readVector2(), glm::vec2(-0.5f, 0.0f), 1e-4f, "invert flips a stick");
    checkNear(norm.value, 0.5f, 1e-4f, "normalize(min=0,max=10) maps 5 to 0.5");
    checkNear(scaled.value, 0.6f, 1e-4f, "scale(factor=2) doubles the trigger");

    // Inside the deadzone the stick reads exactly zero, not a small value.
    f.pad->setVector2("leftStick", glm::vec2(0.1f, 0.0f));
    f.tick();
    checkNear2(dead.readVector2(), glm::vec2(0.0f), 1e-5f, "a stick inside the deadzone reads (0,0)");

    // Outside it, the value is remapped from 0 at the inner edge to 1 at the outer one.
    f.pad->setVector2("leftStick", glm::vec2(0.55f, 0.0f));
    f.tick();
    const float expected = (0.55f - 0.2f) / (0.9f - 0.2f);
    checkNear(dead.readVector2().x, expected, 1e-3f, "a stick past the deadzone is remapped");

    // An unknown processor is reported rather than silently dropped.
    std::vector<std::string> unknown;
    parseProcessors("invert;wobble(strength=2);scale(factor=3)", &unknown);
    checkTrue(unknown.size() == 1 && unknown[0] == "wobble", "an unknown processor is reported by name");
}

// ---------------------------------------------------------------- 5. interactions

void testInteractions() {
    std::printf("\n5. interactions: tap, hold, slow tap, multi tap\n");
    // Tap: performs on release inside the duration, cancels outside it.
    {
        Fixture f;
        ActionMap& map = f.state.addMap("M");
        Action& a = map.add("Tap", ActionType::Button);
        a.bindings.push_back(Binding{"", "", "<Keyboard>/e", "Tap", "", "", "tap(duration=0.2)", false, false, -1});
        f.state.enableMap("M");

        f.keyboard->setButton("e", true);
        f.tick(0.05f);
        checkTrue(!a.triggered(), "tap: not performed while held");
        f.keyboard->setButton("e", false);
        f.tick(0.05f);   // released after 0.05 s, inside the 0.2 s window
        checkTrue(a.triggered(), "tap: performs on a short release");

        f.keyboard->setButton("e", true);
        f.tick(0.05f);
        f.tick(0.30f);   // held past the window
        checkTrue(a.canceled(), "tap: cancels the moment the duration is exceeded, while still held");
        f.keyboard->setButton("e", false);
        f.tick(0.05f);
        checkTrue(!a.triggered(), "tap: a long press does not perform");
    }
    // Hold: performs when the duration elapses while held, once.
    {
        Fixture f;
        ActionMap& map = f.state.addMap("M");
        Action& a = map.add("Hold", ActionType::Button);
        a.bindings.push_back(Binding{"", "", "<Keyboard>/f", "Hold", "", "", "hold(duration=0.3)", false, false, -1});
        f.state.enableMap("M");

        f.keyboard->setButton("f", true);
        f.tick(0.1f);
        checkTrue(!a.triggered(), "hold: nothing before the duration");
        f.tick(0.1f);
        checkTrue(!a.triggered(), "hold: still nothing at 0.2 s of a 0.3 s hold");
        f.tick(0.15f);
        checkTrue(a.triggered(), "hold: performs when 0.3 s elapses");
        f.tick(0.1f);
        checkTrue(!a.triggered(), "hold: performs only once");
        f.keyboard->setButton("f", false);
        f.tick(0.05f);
        checkTrue(a.canceled(), "hold: releasing after it performed cancels it");

        // Releasing before the threshold cancels instead of performing.
        f.keyboard->setButton("f", true);
        f.tick(0.1f);
        f.keyboard->setButton("f", false);
        f.tick(0.05f);
        checkTrue(a.canceled() && !a.triggered(), "hold: a short press cancels, it does not perform");
    }
    // Slow tap: the mirror of tap.
    {
        Fixture f;
        ActionMap& map = f.state.addMap("M");
        Action& a = map.add("Slow", ActionType::Button);
        a.bindings.push_back(
            Binding{"", "", "<Keyboard>/r", "Slow", "", "", "slowTap(duration=0.3)", false, false, -1});
        f.state.enableMap("M");
        f.keyboard->setButton("r", true);
        f.tick(0.1f);
        f.keyboard->setButton("r", false);
        f.tick(0.1f);
        checkTrue(!a.triggered(), "slow tap: a short press does not perform");
        f.keyboard->setButton("r", true);
        f.tick(0.4f);
        f.keyboard->setButton("r", false);
        f.tick(0.05f);
        checkTrue(a.triggered(), "slow tap: a long press performs on release");
    }
    // Multi tap: two quick presses.
    {
        Fixture f;
        ActionMap& map = f.state.addMap("M");
        Action& a = map.add("Double", ActionType::Button);
        a.bindings.push_back(
            Binding{"", "", "<Keyboard>/q", "Double", "", "", "multiTap(tapCount=2,duration=0.4)", false, false, -1});
        f.state.enableMap("M");
        f.keyboard->setButton("q", true);
        f.tick(0.05f);
        f.keyboard->setButton("q", false);
        f.tick(0.05f);
        checkTrue(!a.triggered(), "multi tap: one tap is not enough");
        f.keyboard->setButton("q", true);
        f.tick(0.05f);
        f.keyboard->setButton("q", false);
        f.tick(0.05f);
        checkTrue(a.triggered(), "multi tap: the second tap performs");

        // One tap, then wait past the window: the sequence is cancelled rather than left hanging.
        f.keyboard->setButton("q", true);
        f.tick(0.05f);
        f.keyboard->setButton("q", false);
        f.tick(0.05f);
        f.tick(0.5f);   // the 0.4 s window closes
        checkTrue(a.canceled() && !a.triggered(), "multi tap: the window closing cancels a lone tap");
    }
}

// ---------------------------------------------------------------- 6. maps, schemes and conflicts

void testMapsAndSchemes() {
    std::printf("\n6. maps, control schemes and the same key in two maps\n");
    Fixture f;
    ActionMap& game = f.state.addMap("Gameplay");
    Action& fire = game.add("Fire", ActionType::Button);
    fire.bindings.push_back(Binding{"", "", "<Keyboard>/space", "Fire", "Keyboard", "", "", false, false, -1});
    ActionMap& ui = f.state.addMap("UI");
    Action& submit = ui.add("Submit", ActionType::Button);
    submit.bindings.push_back(Binding{"", "", "<Keyboard>/space", "Submit", "Keyboard", "", "", false, false, -1});

    // Neither map is enabled: the same key does nothing.
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(!fire.triggered() && !submit.triggered(), "a key in two disabled maps does nothing");

    // Enabling one map makes only its action fire. Unity's model: the map decides, not the key.
    f.state.enableMap("Gameplay");
    f.tick();
    checkTrue(fire.triggered(), "with Gameplay enabled, Fire triggers");
    checkTrue(!submit.triggered(), "and Submit in the disabled UI map does not");

    f.state.disableMap("Gameplay");
    f.state.enableMap("UI");
    f.tick();
    checkTrue(!fire.triggered(), "after the switch, Fire is silent");
    f.state.disableAllMaps();
    f.tick();
    checkTrue(!submit.triggered(), "disableAllMaps silences the UI map too");

    // A control scheme filters bindings by their group.
    ControlScheme kb;
    kb.name = "Keyboard";
    kb.bindingGroup = "Keyboard";
    kb.devices = {DeviceKind::Keyboard};
    ControlScheme pad;
    pad.name = "Gamepad";
    pad.bindingGroup = "Gamepad";
    pad.devices = {DeviceKind::Gamepad};
    f.state.controlSchemes = {kb, pad};

    ActionMap& m = f.state.addMap("Move");
    Action& go = m.add("Go", ActionType::Button);
    go.bindings.push_back(Binding{"", "", "<Keyboard>/w", "Go", "Keyboard", "", "", false, false, -1});
    go.bindings.push_back(Binding{"", "", "<Gamepad>/buttonSouth", "Go", "Gamepad", "", "", false, false, -1});
    f.state.setControlScheme("Gamepad");
    f.state.enableMap("Move");
    f.keyboard->setButton("w", true);
    f.pad->setButton("buttonSouth", false);
    f.tick();
    checkTrue(!go.triggered(), "with the Gamepad scheme, the keyboard binding is filtered out");
    f.keyboard->setButton("w", false);
    f.pad->setButton("buttonSouth", true);
    f.tick();
    checkTrue(go.triggered(), "and the gamepad binding works");
    // The value has to return to zero before the next press can read as a new press — the same rule Unity applies
    // when the winning binding changes while the action value never dropped.
    f.state.setControlScheme("Keyboard");
    f.pad->setButton("buttonSouth", false);
    f.tick();
    f.keyboard->setButton("w", true);
    f.tick();
    checkTrue(go.triggered(), "switching scheme flips which binding is live");
}

// ---------------------------------------------------------------- 7. the asset format

const char* kAsset = R"({
  "name": "Ukiyo Controls",
  "maps": [
    {
      "name": "Player",
      "id": "11111111-1111-1111-1111-111111111111",
      "actions": [
        {"name": "Move", "type": "Value", "id": "22222222-2222-2222-2222-222222222222",
         "expectedControlType": "Vector2", "processors": "", "interactions": "", "initialStateCheck": true},
        {"name": "Jump", "type": "Button", "id": "33333333-3333-3333-3333-333333333333",
         "expectedControlType": "Button", "processors": "", "interactions": "", "initialStateCheck": false}
      ],
      "bindings": [
        {"name": "WASD", "id": "44444444-4444-4444-4444-444444444444", "path": "2DVector", "action": "Move",
         "isComposite": true, "isPartOfComposite": false},
        {"name": "up", "id": "55555555-5555-5555-5555-555555555555", "path": "<Keyboard>/w", "action": "Move",
         "isComposite": false, "isPartOfComposite": true},
        {"name": "down", "id": "66666666-6666-6666-6666-666666666666", "path": "<Keyboard>/s", "action": "Move",
         "isComposite": false, "isPartOfComposite": true},
        {"name": "left", "id": "77777777-7777-7777-7777-777777777777", "path": "<Keyboard>/a", "action": "Move",
         "isComposite": false, "isPartOfComposite": true},
        {"name": "right", "id": "88888888-8888-8888-8888-888888888888", "path": "<Keyboard>/d", "action": "Move",
         "isComposite": false, "isPartOfComposite": true},
        {"name": "", "id": "99999999-9999-9999-9999-999999999999", "path": "<Keyboard>/space", "action": "Jump",
         "processors": "", "interactions": "press", "groups": "Keyboard", "isComposite": false,
         "isPartOfComposite": false},
        {"name": "", "id": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "path": "<Gamepad>/buttonSouth",
         "action": "Jump", "groups": "Gamepad", "isComposite": false, "isPartOfComposite": false}
      ]
    }
  ],
  "controlSchemes": [
    {"name": "Keyboard", "bindingGroup": "Keyboard",
     "devices": [{"devicePath": "<Keyboard>", "isOptional": false, "isOR": false}]},
    {"name": "Gamepad", "bindingGroup": "Gamepad",
     "devices": [{"devicePath": "<Gamepad>", "isOptional": false, "isOR": false}]}
  ]
})";

void testAsset() {
    std::printf("\n7. the Unity .inputactions format\n");
    Fixture f;
    AssetLoadResult r = loadAssetJson(f.state, kAsset);
    checkTrue(r.ok, "a Unity-shaped asset loads");
    checkTrue(r.warnings.empty(), "and needs no warnings");
    checkEq(f.state.assetName, "Ukiyo Controls", "the asset name is read");

    ActionMap* player = f.state.map("Player");
    checkTrue(player != nullptr, "the Player map exists");
    if (!player) return;
    checkTrue(player->find("Move") != nullptr && player->find("Jump") != nullptr, "both actions exist");
    Action* move = player->find("Move");
    checkNear(float(move->bindings.size()), 5.0f, 0.0f, "Move has its composite and four parts");
    checkTrue(move->bindings[0].isComposite, "the first binding is the composite parent");
    checkTrue(move->bindings[1].isPartOfComposite, "the second is a part");
    Action* jump = player->find("Jump");
    checkTrue(jump->bindings.size() == 2, "Jump has a keyboard and a gamepad binding");
    checkEq(jump->bindings[0].groups, "Keyboard", "the first Jump binding is in the Keyboard group");

    checkTrue(f.state.controlSchemes.size() == 2, "both control schemes were read");
    checkTrue(f.state.controlSchemes[1].bindingGroup == "Gamepad", "the second scheme's group is Gamepad");

    // It actually plays.
    f.state.setControlScheme("Keyboard");
    f.state.enableMap("Player");
    f.keyboard->setButton("w", true);
    f.tick();
    checkNear2(move->readVector2(), glm::vec2(0.0f, 1.0f), 1e-4f, "the loaded asset drives Move");
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(jump->triggered(), "the loaded asset drives Jump");

    // Round trip: save, reload, and the structure is the same.
    const std::string text = saveAssetJson(f.state);
    State again;
    again.addDevice(DeviceKind::Keyboard, 0);
    AssetLoadResult r2 = loadAssetJson(again, text);
    checkTrue(r2.ok, "the file this engine writes loads again");
    ActionMap* p2 = again.map("Player");
    checkTrue(p2 && p2->find("Jump") && p2->find("Jump")->bindings.size() == 2, "the round trip keeps the bindings");
    const std::string text2 = saveAssetJson(again);
    checkTrue(text == text2, "and saving twice produces identical text (stable GUIDs, stable order)");

    // A binding to an action that does not exist is reported, not dropped in silence.
    AssetLoadResult r3 = loadAssetJson(f.state, R"({"name":"x","maps":[{"name":"M","actions":[],
      "bindings":[{"name":"","path":"<Keyboard>/w","action":"Nope"}]}]})");
    checkTrue(r3.ok && !r3.warnings.empty(), "a binding to a missing action is reported");

    // Malformed input fails with a message rather than an empty asset.
    AssetLoadResult r4 = loadAssetJson(f.state, R"({"name":"x"})");
    checkTrue(!r4.ok && !r4.error.empty(), "an asset with no maps array is an error, not an empty asset");

    // The file on disk round-trips too.
    const std::string path = "/tmp/ukiyo_input_test.inputactions";
    std::string err;
    checkTrue(saveAssetFile(f.state, path, &err), "the asset saves to a file");
    State fromFile;
    fromFile.addDevice(DeviceKind::Keyboard, 0);
    AssetLoadResult r5 = loadAssetFile(fromFile, path);
    checkTrue(r5.ok, "and loads back from the file");
    std::remove(path.c_str());
}

// ---------------------------------------------------------------- 8. rebinding

void testRebinding() {
    std::printf("\n8. interactive rebinding\n");
    Fixture f;
    ActionMap& map = f.state.addMap("Player");
    Action& jump = map.add("Jump", ActionType::Button);
    jump.bindings.push_back(Binding{"", "", "<Keyboard>/space", "Jump", "", "", "", false, false, -1});
    f.state.enableMap("Player");

    std::string captured;
    f.state.onRebind = [&](const std::string& path) { captured = path; };
    f.state.startRebind(map, jump, 0);
    checkTrue(f.state.rebinding(), "rebinding starts");

    // Nothing held yet: the update must not capture.
    f.tick();
    checkTrue(f.state.rebinding() && captured.empty(), "no capture while nothing is pressed");

    // A gamepad button must not capture a keyboard binding.
    f.pad->setButton("buttonSouth", true);
    f.tick();
    checkTrue(f.state.rebinding(), "a gamepad press does not capture a keyboard binding");

    f.keyboard->setButton("j", true);
    f.tick();
    checkTrue(!f.state.rebinding(), "a keyboard press ends the rebind");
    checkEq(captured, "<Keyboard>/j", "and captures the right path");
    checkEq(jump.bindings[0].path, "<Keyboard>/j", "the binding holds the new path");

    // The new key drives the action; the old one does not.
    f.keyboard->setButton("j", false);
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(!jump.triggered(), "the old key no longer fires");
    f.keyboard->setButton("space", false);
    f.keyboard->setButton("j", true);
    f.tick();
    checkTrue(jump.triggered(), "the new key does");

    // Cancelling leaves the binding alone and reports it.
    bool canceled = false;
    f.state.onRebindCanceled = [&]() { canceled = true; };
    f.state.startRebind(map, jump, 0);
    f.state.cancelRebind();
    checkTrue(canceled && !f.state.rebinding(), "cancelRebind reports and stops");
    checkEq(jump.bindings[0].path, "<Keyboard>/j", "cancelling leaves the binding unchanged");
}

// ---------------------------------------------------------------- 9. missing devices

void testMissingDevices() {
    std::printf("\n9. bindings whose device is absent\n");
    State s;   // no devices at all
    s.setPoller([](State&) {});
    ActionMap& map = s.addMap("Player");
    Action& jump = map.add("Jump", ActionType::Button);
    jump.bindings.push_back(Binding{"", "", "<Gamepad>/buttonSouth", "Jump", "", "", "", false, false, -1});
    s.enableMap("Player");
    s.update(1.0f / 60.0f);
    checkTrue(!jump.triggered() && jump.value == 0.0f, "a binding to an absent device is simply never active");

    // Adding the device later makes it work, without rebuilding the asset.
    Device& pad = s.addDevice(DeviceKind::Gamepad, 0);
    s.resolveBindings();
    pad.setButton("buttonSouth", true);
    s.update(1.0f / 60.0f);
    checkTrue(jump.triggered(), "hot-plugging the device makes the binding live");

    // Disabling the device disables the binding.
    jump.triggered();
    pad.enabled = false;
    s.resolveBindings();
    pad.setButton("buttonSouth", false);
    s.update(1.0f / 60.0f);
    pad.setButton("buttonSouth", true);
    s.update(1.0f / 60.0f);
    checkTrue(!jump.triggered(), "a disabled device does not drive actions");
}

// ---------------------------------------------------------------- 10. pass-through and callbacks

void testPassThroughAndCallbacks() {
    std::printf("\n10. pass-through actions and the performed callback\n");
    Fixture f;
    ActionMap& map = f.state.addMap("Player");
    Action& any = map.add("Any", ActionType::PassThrough);
    any.bindings.push_back(Binding{"", "", "<Keyboard>/a", "Any", "", "", "", false, false, -1});
    any.bindings.push_back(Binding{"", "", "<Keyboard>/b", "Any", "", "", "", false, false, -1});
    f.state.enableMap("Player");

    std::vector<std::string> fired;
    map.onActionPerformed = [&](const std::string& n) { fired.push_back(n); };

    f.keyboard->setButton("a", true);
    f.tick();
    checkTrue(any.triggered(), "pass-through: A performs");
    f.keyboard->setButton("a", false);
    f.tick();
    checkTrue(!any.triggered(), "pass-through: releasing A ends the press");
    f.keyboard->setButton("b", true);
    f.tick();
    checkTrue(any.triggered(), "pass-through: B performs after A is released");
    checkTrue(fired.size() == 2 && fired[0] == "Any" && fired[1] == "Any", "the map callback fired twice");

    // Action callbacks fire on the same frames as the accessors, and only on those frames.
    Action& cb = map.add("Callback", ActionType::Button);
    cb.bindings.push_back(Binding{"", "", "<Keyboard>/x", "Callback", "", "", "", false, false, -1});
    int starts = 0, performs = 0, cancels = 0;
    cb.onStarted = [&](const Action&) { ++starts; };
    cb.onPerformed = [&](const Action&) { ++performs; };
    cb.onCanceled = [&](const Action&) { ++cancels; };
    f.state.resolveBindings();
    f.keyboard->setButton("x", true);
    f.tick();
    checkTrue(starts == 1 && performs == 1 && cancels == 0, "callbacks: a press reports started and performed once");
    f.tick();
    checkTrue(starts == 1 && performs == 1, "callbacks: holding does not repeat them");
    f.keyboard->setButton("x", false);
    f.tick();
    checkTrue(cancels == 1, "callbacks: the release reports canceled");

    // An action-level interaction is honoured the same way a binding's is.
    ActionMap& m2 = f.state.addMap("Menu");
    Action& hold = m2.add("Hold", ActionType::Button);
    hold.interactions = "hold(duration=0.2)";
    hold.bindings.push_back(Binding{"", "", "<Keyboard>/c", "Hold", "", "", "", false, false, -1});
    f.state.enableMap("Menu");
    f.keyboard->setButton("c", true);
    f.tick(0.1f);
    checkTrue(!hold.triggered(), "an action-level hold does nothing at 0.1 s");
    f.tick(0.1f);
    checkTrue(hold.triggered(), "and performs at 0.2 s");
}

// ---------------------------------------------------------------- 11. the parser's edges

void testParserEdges() {
    std::printf("\n11. parser edge cases\n");
    std::vector<std::string> unknown;
    auto interactions = parseInteractions("hold(duration=0.75);tap", &unknown);
    checkTrue(interactions.size() == 2, "two interactions parse");
    checkNear(interactions[0].duration, 0.75f, 1e-5f, "hold's duration parameter is read");
    checkNear(interactions[1].duration, 0.2f, 1e-5f, "tap's default is 0.2 s");

    auto procs = parseProcessors("scale(factor=2.5)", nullptr);
    checkTrue(procs.size() == 1, "one processor parses");
    checkNear(procs[0].apply(2.0f), 5.0f, 1e-5f, "scale applies its factor");

    auto none = parseProcessors("", nullptr);
    checkTrue(none.empty(), "an empty processor list is empty, not a stub");

    // Qt's portable key text to Unity control names. The editor stores "Space"; the asset says "<Keyboard>/space".
    std::string control;
    checkTrue(controlNameFromPortableKey("Space", &control) && control == "space", "portable Space -> space");
    checkTrue(controlNameFromPortableKey("W", &control) && control == "w", "portable W -> w");
    checkTrue(controlNameFromPortableKey("Left", &control) && control == "leftArrow", "portable Left -> leftArrow");
    checkTrue(controlNameFromPortableKey("F7", &control) && control == "f7", "portable F7 -> f7");
    // Qt says "Shift" for the left shift key; a project file may say "LeftShift", and Unity says "leftShift".
    checkTrue(controlNameFromPortableKey("Shift", &control) && control == "leftShift", "portable Shift -> leftShift");
    checkTrue(controlNameFromPortableKey("LeftShift", &control) && control == "leftShift",
              "LeftShift is accepted as an alias");
    checkTrue(controlNameFromPortableKey("leftShift", &control) && control == "leftShift",
              "a Unity control name is accepted as it is");
    checkTrue(controlNameFromPortableKey("Esc", &control) && control == "escape", "portable Esc -> escape");
    checkTrue(controlNameFromPortableKey("PgUp", &control) && control == "pageUp", "portable PgUp -> pageUp");
    checkTrue(controlNameFromPortableKey(";", &control) && control == "semicolon", "portable ; -> semicolon");
    checkTrue(!controlNameFromPortableKey("Hyper", &control), "an unknown key name is refused, not guessed");

    checkEq(makeGuid("same"), makeGuid("same"), "makeGuid is stable for the same seed");
    checkTrue(makeGuid("a") != makeGuid("b"), "makeGuid differs for different seeds");
    checkTrue(makeGuid("a").size() == 32, "a GUID is 32 hex characters, as Unity writes them");
}

// ---------------------------------------------------------------- 12. the shipped viewer asset

// The viewer (src/app/main.cpp) looks these up by name. If one is renamed in the asset or dropped from it, the viewer
// loses that key and this test says so, which is the only way a data file gets checked in CI.
void testViewerAsset() {
    std::printf("\n12. the viewer's .inputactions asset\n");
    const std::string path = std::string(UKIYO_ROOT) + "/assets/input/viewer.inputactions";
    State s;
    Device& keyboard = s.addDevice(DeviceKind::Keyboard, 0);
    Device& mouse = s.addDevice(DeviceKind::Mouse, 0);
    s.setPoller([](State&) {});
    AssetLoadResult r = loadAssetFile(s, path);
    if (!r.ok) {
        checkTrue(false, "the viewer asset loads: " + r.error);
        return;
    }
    checkTrue(r.warnings.empty(), "the viewer asset has nothing the engine has to skip");
    ActionMap* viewer = s.map("Viewer");
    checkTrue(viewer != nullptr, "the Viewer map is there");
    if (!viewer) return;

    const char* names[] = {"Move", "Elevate", "Look", "Orbit", "Pan", "Zoom", "Quit", "AutoOrbit",
                           "ResetCamera", "Screenshot", "Hit", "ToggleGrid", "ToggleBloom", "ToggleOutlines",
                           "ToggleShadows", "ToggleSky", "Aa1", "Aa2", "Aa3", "Aa4"};
    int missing = 0;
    for (const char* n : names)
        if (!viewer->find(n)) {
            ++missing;
            std::printf("      missing action: %s\n", n);
        }
    checkTrue(missing == 0, "every action the viewer looks up exists");

    // And it plays: WASD through the composite, E/Q through the axis, the mouse through its controls.
    s.enableMap("Viewer");
    keyboard.setButton("w", true);
    keyboard.setButton("d", true);
    s.update(1.0f / 60.0f);
    checkNear2(viewer->find("Move")->readVector2(), glm::vec2(1.0f, 1.0f), 1e-4f, "the viewer's Move composite reads W+D");
    keyboard.setButton("w", false);
    keyboard.setButton("d", false);
    keyboard.setButton("e", true);
    s.update(1.0f / 60.0f);
    checkNear(viewer->find("Elevate")->readFloat(), 1.0f, 1e-4f, "the viewer's Elevate axis reads E");
    keyboard.setButton("e", false);
    mouse.setVector2("scroll", glm::vec2(0.0f, 2.0f));
    s.update(1.0f / 60.0f);
    checkNear(viewer->find("Zoom")->readVector2().y, 2.0f, 1e-4f, "the viewer's Zoom reads the wheel");
}

// ---------------------------------------------------------------- 13. players and pairing
//
// Two players, one keyboard and two gamepads. The master State owns the devices and the asset; each PlayerInput owns a
// copy of the maps and a mirror of only its own devices. Everything below is a consequence of that split.

struct MultiFixture {
    State master;
    Device* keyboard = nullptr;
    Device* mouse = nullptr;
    Device* pad0 = nullptr;
    Device* pad1 = nullptr;
    std::unique_ptr<InputSystem> sys;
    int masterCallbacks = 0;

    MultiFixture() {
        keyboard = &master.addDevice(DeviceKind::Keyboard, 0);
        mouse = &master.addDevice(DeviceKind::Mouse, 0);
        pad0 = &master.addDevice(DeviceKind::Gamepad, 0);
        pad1 = &master.addDevice(DeviceKind::Gamepad, 1);
        master.setPoller([](State&) {});

        ControlScheme kb;
        kb.name = "Keyboard";
        kb.bindingGroup = "Keyboard";
        kb.devices = {DeviceKind::Keyboard, DeviceKind::Mouse};
        ControlScheme gp;
        gp.name = "Gamepad";
        gp.bindingGroup = "Gamepad";
        gp.devices = {DeviceKind::Gamepad};
        master.controlSchemes = {kb, gp};

        ActionMap& game = master.addMap("Gameplay");
        Action& jump = game.add("Jump", ActionType::Button);
        jump.bindings.push_back(Binding{"", "", "<Keyboard>/space", "Jump", "Keyboard", "", "", false, false, -1});
        jump.bindings.push_back(Binding{"", "", "<Gamepad>/buttonSouth", "Jump", "Gamepad", "", "", false, false, -1});
        Action& move = game.add("Move", ActionType::Value);
        move.expectedControlType = "Vector2";
        move.bindings.push_back(Binding{"", "", "<Gamepad>/leftStick", "Move", "Gamepad", "", "", false, false, -1});
        ActionMap& menu = master.addMap("Menu");
        menu.add("Pause", ActionType::Button)
            .bindings.push_back(Binding{"", "", "<Keyboard>/escape", "Pause", "Keyboard", "", "", false, false, -1});

        // A callback on the master's action must not be inherited by a player: the master's maps are a template.
        master.action("Gameplay", "Jump")->onPerformed = [this](const Action&) { ++masterCallbacks; };
        sys = std::make_unique<InputSystem>(&master);
    }

    PlayerInput* join(JoinOptions options, std::string* error = nullptr) {
        PlayerInput* p = sys->join(options, error);
        if (p) p->switchActionMap("Gameplay");
        return p;
    }

    void tick(float dt = 1.0f / 60.0f) { sys->update(dt); }

    Action* jump(PlayerInput* p) { return p->action("Gameplay", "Jump"); }
};

void testPlayers() {
    std::printf("\n13. players, pairing and independent actions\n");
    MultiFixture f;

    std::string error;
    PlayerInput* p1 = f.join(JoinOptions{"Keyboard", {f.keyboard, f.mouse}, false}, &error);
    PlayerInput* p2 = f.join(JoinOptions{"Gamepad", {f.pad0}, false}, &error);
    checkTrue(p1 != nullptr && p2 != nullptr, "two players join with their own devices");
    checkTrue(p1 && p1->index() == 1 && p2 && p2->index() == 2, "player indices are 1 and 2");
    checkTrue(p1 && p1->devices().size() == 2, "player 1 holds a keyboard and a mouse");
    checkTrue(p2 && p2->devices().size() == 1, "player 2 holds one gamepad");
    checkEq(p1 ? p1->controlScheme() : "", "Keyboard", "player 1's scheme is recorded");
    checkTrue(f.sys->ownerOf(f.pad0) == p2 && f.sys->ownerOf(f.keyboard) == p1, "ownerOf maps a device to its player");
    checkTrue(p1 && p1->actions().device(DeviceKind::Gamepad, 0) == nullptr,
              "player 1 has no gamepad in its own State");
    checkTrue(p2 && p2->actions().device(DeviceKind::Keyboard, 0) == nullptr,
              "player 2 has no keyboard in its own State");

    // One press, one player.
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(p1 && f.jump(p1)->triggered(), "player 1's Jump triggers on the keyboard");
    checkTrue(p2 && !f.jump(p2)->triggered(), "and player 2's Jump, which has no keyboard, does not");
    checkNear(p2 ? f.jump(p2)->readFloat() : -1.0f, 0.0f, 1e-4f, "player 2's Jump value stays at zero");

    f.keyboard->setButton("space", false);
    f.pad0->setButton("buttonSouth", true);
    f.tick();
    checkTrue(!f.jump(p1)->triggered(), "player 1 cannot press player 2's button");
    checkTrue(f.jump(p2)->triggered(), "player 2's Jump triggers on its own gamepad");

    // Two players can hold the same logical action at once; the values are separate numbers.
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(f.jump(p1)->performed() && f.jump(p2)->performed(), "both players hold Jump in the same frame");
    checkNear(f.jump(p1)->readFloat(), 1.0f, 1e-4f, "player 1 reads its own value");
    checkNear(f.jump(p2)->readFloat(), 1.0f, 1e-4f, "player 2 reads its own value");

    // The master's callback belongs to the master, not to the players that copied its maps.
    f.pad0->setButton("buttonSouth", false);
    f.keyboard->setButton("space", false);
    f.tick();
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(f.masterCallbacks == 0, "a callback installed on the master's action does not fire for a player");

    // Analog values are per player too: player 2's stick moves only player 2.
    f.keyboard->setButton("space", false);
    f.pad0->setVector2("leftStick", glm::vec2(0.75f, -0.25f));
    f.tick();
    checkNear2(f.sys->player(2)->action("Gameplay", "Move")->readVector2(), glm::vec2(0.75f, -0.25f), 1e-4f,
               "player 2 reads its stick");
    checkTrue(f.sys->player(1)->action("Gameplay", "Move") == nullptr ||
                  glm::length(f.sys->player(1)->action("Gameplay", "Move")->readVector2()) < 1e-4f,
              "player 1's Move stays at zero with no gamepad");

    // Action-map switching is per player: player 2 pauses, player 1 keeps playing.
    Action* pause = p2->action("Menu", "Pause");
    checkTrue(pause != nullptr, "player 2 has the Menu map");
    p2->switchActionMap("Menu");
    checkEq(p2->currentActionMap(), "Menu", "player 2 switches to Menu");
    checkTrue(!f.jump(p2)->enabled, "switching away disables Gameplay for that player");
    f.pad0->setButton("buttonSouth", false);
    f.tick();
    f.pad0->setButton("buttonSouth", true);
    f.tick();
    checkTrue(!f.jump(p2)->triggered(), "player 2's Jump is silent in the Menu map");
    f.keyboard->setButton("space", true);
    f.tick();
    checkTrue(f.jump(p1)->triggered(), "player 1's Jump still fires in its own Gameplay map");
    p2->switchActionMap("Gameplay");
    checkTrue(f.jump(p2)->enabled, "switching back re-enables Gameplay");
}

// ---------------------------------------------------------------- 14. join rules

void testJoinRules() {
    std::printf("\n14. join rules, refusals and leaving\n");
    MultiFixture f;

    // The scheme picks the devices when the caller does not.
    PlayerInput* byScheme = f.join(JoinOptions{"Gamepad", {}, false});
    checkTrue(byScheme != nullptr && byScheme->devices().size() == 1 && byScheme->devices()[0] == f.pad0,
              "a scheme join takes the first free gamepad");
    PlayerInput* second = f.join(JoinOptions{"Gamepad", {}, false});
    checkTrue(second != nullptr && second->devices()[0] == f.pad1,
              "the second scheme join takes the next free gamepad, not the same one");

    std::string error;
    checkTrue(f.join(JoinOptions{"Gamepad", {}, false}, &error) == nullptr, "a third gamepad join finds no device");
    checkTrue(!error.empty(), "and reports why");

    // The same physical device cannot drive two players by accident.
    error.clear();
    checkTrue(f.sys->join(JoinOptions{"", {f.pad0}, false}, &error) == nullptr,
              "pairing an already-paired gamepad is refused");
    checkTrue(error.find("player") != std::string::npos, "the refusal names the player that holds it");
    checkTrue(f.sys->join(JoinOptions{"", {f.pad0}, true}) != nullptr, "allowDuplicates lets two players share it");

    checkNear(float(f.sys->playerCount()), 3.0f, 0.0f, "three players are in the game");

    // Leaving frees the device, and the next join can have it.
    f.sys->players();   // const accessor, just to be exercised
    f.sys->leave(f.sys->player(2));
    checkTrue(f.sys->playerCount() == 2, "leaving removes the player");
    checkTrue(f.sys->ownerOf(f.pad1) == nullptr, "and frees its gamepad");
    PlayerInput* again = f.join(JoinOptions{"Gamepad", {}, false});
    checkTrue(again != nullptr && again->devices()[0] == f.pad1, "the freed gamepad joins a new player");
    checkTrue(again->index() == 4, "player indices keep counting up, as Unity's do");

    // The keyboard scheme needs both devices in this asset; with the keyboard taken, the join must fail rather than
    // hand out a mouse on its own.
    std::string error2;
    f.sys->join(JoinOptions{"Keyboard", {f.keyboard, f.mouse}, false});
    checkTrue(f.join(JoinOptions{"Keyboard", {}, false}, &error2) == nullptr,
              "a scheme whose devices are taken returns null");

    // An empty scheme with a device picks that device alone.
    PlayerInput* bare = f.sys->join(JoinOptions{"", {f.pad1}, true});
    checkTrue(bare != nullptr && bare->devices().size() == 1, "an explicit device joins without a scheme");
    checkTrue(bare && bare->controlScheme().empty(), "and records no scheme");

    // Maps added to the master after the fact reach the players on refreshMaps.
    f.master.addMap("Debug").add("Toggle", ActionType::Button)
        .bindings.push_back(Binding{"", "", "<Keyboard>/f1", "Toggle", "", "", "", false, false, -1});
    checkTrue(f.sys->player(1)->action("Debug", "Toggle") == nullptr, "a new map is not in the player yet");
    checkEq(f.sys->player(1)->currentActionMap(), "Gameplay", "refreshMaps keeps the map the player was driving");
    f.sys->refreshMaps();
    checkTrue(f.sys->player(1)->action("Debug", "Toggle") != nullptr, "refreshMaps copies the new map");
    checkEq(f.sys->player(1)->currentActionMap(), "Gameplay", "and keeps the player's current map");
}

// ---------------------------------------------------------------- 15. auto-join and per-player schemes

void testAutoJoin() {
    std::printf("\n15. auto-join, default maps and per-player scheme filtering\n");
    MultiFixture f;
    f.sys->setDefaultActionMap("Gameplay");
    f.sys->autoJoin(true, JoinOptions{"Gamepad", {}, false}, 2);
    int joined = 0;
    f.sys->onPlayerJoined = [&](PlayerInput&) { ++joined; };

    f.tick();
    checkTrue(f.sys->playerCount() == 0, "no player joins without a press");

    // Pressing a gamepad button joins the player that pressed it, on the same frame.
    f.pad0->setButton("buttonSouth", true);
    f.tick();
    checkTrue(f.sys->playerCount() == 1 && joined == 1, "a press edge joins one player");
    checkTrue(f.sys->player(1)->devices()[0] == f.pad0, "the player is paired with the device that was pressed");
    checkEq(f.sys->player(1)->currentActionMap(), "Gameplay", "the default map is enabled on join");
    checkTrue(f.sys->player(1)->action("Gameplay", "Jump")->performed(), "and the joining press already reads");

    // Holding does not join a second player; a new press on another device does.
    f.tick();
    checkTrue(f.sys->playerCount() == 1, "holding the button does not join again");
    f.pad1->setButton("buttonSouth", true);
    f.tick();
    checkTrue(f.sys->playerCount() == 2, "a press on an unpaired device joins the second player");

    // The cap holds.
    f.pad0->setButton("buttonSouth", false);
    f.pad1->setButton("buttonSouth", false);
    f.tick();
    f.pad0->setButton("buttonSouth", true);
    f.tick();
    checkTrue(f.sys->playerCount() == 2, "autoJoin respects maxPlayers");

    // A player with both a keyboard and a gamepad, on the Gamepad scheme, ignores its keyboard.
    MultiFixture g;
    PlayerInput* p = g.join(JoinOptions{"Gamepad", {g.pad0}, false});
    // Hand the same player a keyboard as well: the scheme, not the device list, decides what is live.
    g.sys->master().addDevice(DeviceKind::Keyboard, 0);
    p->pairDevice(g.sys->master().device(DeviceKind::Keyboard, 0));
    g.keyboard->setButton("space", true);
    g.tick();
    checkTrue(!g.jump(p)->triggered(), "with the Gamepad scheme, the player's keyboard binding is filtered out");
    g.pad0->setButton("buttonSouth", true);
    g.tick();
    checkTrue(g.jump(p)->triggered(), "and the gamepad binding still fires");

    // Losing the device mid-game zeroes the player, keeps its map, and does not leave a stale value behind.
    p->unpairDevice(g.pad0);
    checkTrue(p->devices().size() == 1, "unpairing removes the device");
    checkTrue(g.jump(p)->enabled, "the player keeps driving the map it was on across a pairing change");
    checkEq(p->currentActionMap(), "Gameplay", "and keeps its name");
    g.tick();
    checkNear(g.jump(p)->readFloat(), 0.0f, 1e-4f, "a player with no gamepad reads zero");
    // Hot-swapping is the reason the map survives: plug a gamepad back in and Jump reads again with no game code.
    g.pad1->setButton("buttonSouth", true);
    p->pairDevice(g.pad1);
    g.tick();
    checkTrue(g.jump(p)->triggered() || g.jump(p)->performed(),
              "re-pairing a gamepad while it is held reads as a press on the rebuilt bindings");
}

} // namespace

int main() {
    std::printf("input system (docs/INPUT.md)\n");
    testDevices();
    testButtons();
    testValueAndComposite();
    testProcessors();
    testInteractions();
    testMapsAndSchemes();
    testAsset();
    testRebinding();
    testMissingDevices();
    testPassThroughAndCallbacks();
    testParserEdges();
    testViewerAsset();
    testPlayers();
    testJoinRules();
    testAutoJoin();
    std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
