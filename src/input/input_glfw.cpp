#include "input/input_glfw.h"

#include "input/input.h"

// This file uses GLFW only for windows, keys and gamepads. It must not pull in GL/gl.h: the backend has to compile on
// a machine with GLFW and no GL development headers, which is what makes it usable in a headless test build.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>

namespace ukiyo::input::glfw_backend {

namespace {

GLFWwindow* gWindow = nullptr;
bool gEnabled = true;
double gLastX = 0.0, gLastY = 0.0;
bool gHaveLast = false;
glm::vec2 gScroll{0.0f};

struct KeyName {
    const char* name;
    int key;
};

// Unity's keyboard control names against GLFW's key codes. GLFW's own names are close enough that this is a table
// rather than a rule: "leftCtrl" is GLFW_KEY_LEFT_CONTROL, "numpadEnter" is GLFW_KEY_KP_ENTER, and so on.
const KeyName kKeys[] = {
    {"a", GLFW_KEY_A}, {"b", GLFW_KEY_B}, {"c", GLFW_KEY_C}, {"d", GLFW_KEY_D}, {"e", GLFW_KEY_E},
    {"f", GLFW_KEY_F}, {"g", GLFW_KEY_G}, {"h", GLFW_KEY_H}, {"i", GLFW_KEY_I}, {"j", GLFW_KEY_J},
    {"k", GLFW_KEY_K}, {"l", GLFW_KEY_L}, {"m", GLFW_KEY_M}, {"n", GLFW_KEY_N}, {"o", GLFW_KEY_O},
    {"p", GLFW_KEY_P}, {"q", GLFW_KEY_Q}, {"r", GLFW_KEY_R}, {"s", GLFW_KEY_S}, {"t", GLFW_KEY_T},
    {"u", GLFW_KEY_U}, {"v", GLFW_KEY_V}, {"w", GLFW_KEY_W}, {"x", GLFW_KEY_X}, {"y", GLFW_KEY_Y},
    {"z", GLFW_KEY_Z},
    {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3}, {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5},
    {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7}, {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9}, {"0", GLFW_KEY_0},
    {"space", GLFW_KEY_SPACE}, {"enter", GLFW_KEY_ENTER}, {"tab", GLFW_KEY_TAB},
    {"backquote", GLFW_KEY_GRAVE_ACCENT}, {"quote", GLFW_KEY_APOSTROPHE}, {"semicolon", GLFW_KEY_SEMICOLON},
    {"comma", GLFW_KEY_COMMA}, {"period", GLFW_KEY_PERIOD}, {"slash", GLFW_KEY_SLASH},
    {"backslash", GLFW_KEY_BACKSLASH}, {"leftBracket", GLFW_KEY_LEFT_BRACKET},
    {"rightBracket", GLFW_KEY_RIGHT_BRACKET}, {"minus", GLFW_KEY_MINUS}, {"equals", GLFW_KEY_EQUAL},
    {"leftShift", GLFW_KEY_LEFT_SHIFT}, {"rightShift", GLFW_KEY_RIGHT_SHIFT},
    {"leftCtrl", GLFW_KEY_LEFT_CONTROL}, {"rightCtrl", GLFW_KEY_RIGHT_CONTROL},
    {"leftAlt", GLFW_KEY_LEFT_ALT}, {"rightAlt", GLFW_KEY_RIGHT_ALT},
    {"leftMeta", GLFW_KEY_LEFT_SUPER}, {"rightMeta", GLFW_KEY_RIGHT_SUPER},
    {"escape", GLFW_KEY_ESCAPE}, {"capsLock", GLFW_KEY_CAPS_LOCK}, {"backspace", GLFW_KEY_BACKSPACE},
    {"insert", GLFW_KEY_INSERT}, {"delete", GLFW_KEY_DELETE}, {"home", GLFW_KEY_HOME}, {"end", GLFW_KEY_END},
    {"pageUp", GLFW_KEY_PAGE_UP}, {"pageDown", GLFW_KEY_PAGE_DOWN},
    {"upArrow", GLFW_KEY_UP}, {"downArrow", GLFW_KEY_DOWN}, {"leftArrow", GLFW_KEY_LEFT},
    {"rightArrow", GLFW_KEY_RIGHT},
    {"f1", GLFW_KEY_F1}, {"f2", GLFW_KEY_F2}, {"f3", GLFW_KEY_F3}, {"f4", GLFW_KEY_F4}, {"f5", GLFW_KEY_F5},
    {"f6", GLFW_KEY_F6}, {"f7", GLFW_KEY_F7}, {"f8", GLFW_KEY_F8}, {"f9", GLFW_KEY_F9}, {"f10", GLFW_KEY_F10},
    {"f11", GLFW_KEY_F11}, {"f12", GLFW_KEY_F12},
    {"printScreen", GLFW_KEY_PRINT_SCREEN}, {"scrollLock", GLFW_KEY_SCROLL_LOCK}, {"pause", GLFW_KEY_PAUSE},
    {"numLock", GLFW_KEY_NUM_LOCK},
    {"numpad0", GLFW_KEY_KP_0}, {"numpad1", GLFW_KEY_KP_1}, {"numpad2", GLFW_KEY_KP_2},
    {"numpad3", GLFW_KEY_KP_3}, {"numpad4", GLFW_KEY_KP_4}, {"numpad5", GLFW_KEY_KP_5},
    {"numpad6", GLFW_KEY_KP_6}, {"numpad7", GLFW_KEY_KP_7}, {"numpad8", GLFW_KEY_KP_8},
    {"numpad9", GLFW_KEY_KP_9}, {"numpadEnter", GLFW_KEY_KP_ENTER}, {"numpadPlus", GLFW_KEY_KP_ADD},
    {"numpadMinus", GLFW_KEY_KP_SUBTRACT}, {"numpadMultiply", GLFW_KEY_KP_MULTIPLY},
    {"numpadDivide", GLFW_KEY_KP_DIVIDE}, {"numpadPeriod", GLFW_KEY_KP_DECIMAL},
};

int glfwButtonForControl(const std::string& name) {
    if (name == "leftButton") return GLFW_MOUSE_BUTTON_LEFT;
    if (name == "rightButton") return GLFW_MOUSE_BUTTON_RIGHT;
    if (name == "middleButton") return GLFW_MOUSE_BUTTON_MIDDLE;
    if (name == "forwardButton") return GLFW_MOUSE_BUTTON_4;
    if (name == "backButton") return GLFW_MOUSE_BUTTON_5;
    return -1;
}

void pollKeyboard(State& state, GLFWwindow* w) {
    Device* keyboard = state.firstDevice(DeviceKind::Keyboard);
    if (!keyboard || !keyboard->enabled) return;
    for (const auto& name : keyboardControls()) {
        const int key = keyForControl(name);
        if (key < 0) continue;
        keyboard->setButton(name, glfwGetKey(w, key) == GLFW_PRESS);
    }
}

void pollMouse(State& state, GLFWwindow* w) {
    Device* mouse = state.firstDevice(DeviceKind::Mouse);
    if (!mouse || !mouse->enabled) return;
    for (const auto& name : mouseControls()) {
        const int button = glfwButtonForControl(name);
        if (button < 0) continue;
        mouse->setButton(name, glfwGetMouseButton(w, button) == GLFW_PRESS);
    }
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    if (!gHaveLast) {
        gLastX = x;
        gLastY = y;
        gHaveLast = true;
    }
    // Unity's Mouse.position is in pixels with the origin at the bottom-left; GLFW's cursor is top-left. Flipping
    // here means a game written against Unity's convention reads the same numbers.
    int winW = 0, winH = 0;
    glfwGetWindowSize(w, &winW, &winH);
    const glm::vec2 pos(float(x), float(winH) - float(y));
    const glm::vec2 delta(float(x - gLastX), float(gLastY - y));   // y up, like position
    gLastX = x;
    gLastY = y;
    mouse->setVector2("position", pos);
    mouse->setVector2("delta", delta);
    mouse->setVector2("scroll", gScroll);
    gScroll = glm::vec2(0.0f);   // scroll is a per-frame delta, accumulated by pushScroll and cleared here
}

void pollGamepads(State& state) {
    int deviceIndex = 0;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (!glfwJoystickPresent(jid) || !glfwJoystickIsGamepad(jid)) continue;
        Device* pad = state.device(DeviceKind::Gamepad, deviceIndex);
        if (!pad) continue;
        pad->connected = true;
        GLFWgamepadstate gs{};
        if (!glfwGetGamepadState(jid, &gs)) continue;
        auto button = [&](int b) { return gs.buttons[b] == GLFW_PRESS; };
        pad->setButton("buttonSouth", button(GLFW_GAMEPAD_BUTTON_A));
        pad->setButton("buttonEast", button(GLFW_GAMEPAD_BUTTON_B));
        pad->setButton("buttonWest", button(GLFW_GAMEPAD_BUTTON_X));
        pad->setButton("buttonNorth", button(GLFW_GAMEPAD_BUTTON_Y));
        pad->setButton("leftShoulder", button(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER));
        pad->setButton("rightShoulder", button(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER));
        pad->setButton("leftStickButton", button(GLFW_GAMEPAD_BUTTON_LEFT_THUMB));
        pad->setButton("rightStickButton", button(GLFW_GAMEPAD_BUTTON_RIGHT_THUMB));
        pad->setButton("start", button(GLFW_GAMEPAD_BUTTON_START));
        pad->setButton("select", button(GLFW_GAMEPAD_BUTTON_BACK));
        pad->setButton("guide", button(GLFW_GAMEPAD_BUTTON_GUIDE));
        // Triggers read 0..1, sticks -1..1, and the Y axis is flipped so up is +1 like Unity.
        pad->setAxis("leftTrigger", (gs.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f);
        pad->setAxis("rightTrigger", (gs.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f);
        pad->setVector2("leftStick",
                        glm::vec2(gs.axes[GLFW_GAMEPAD_AXIS_LEFT_X], -gs.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]));
        pad->setVector2("rightStick",
                        glm::vec2(gs.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], -gs.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]));
        // GLFW reports the d-pad as four buttons, not as an axis pair, so the vector is assembled here.
        const glm::vec2 dpad(button(GLFW_GAMEPAD_BUTTON_DPAD_LEFT)    ? -1.0f
                             : button(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1.0f
                                                                      : 0.0f,
                             button(GLFW_GAMEPAD_BUTTON_DPAD_DOWN)    ? -1.0f
                             : button(GLFW_GAMEPAD_BUTTON_DPAD_UP)    ? 1.0f
                                                                      : 0.0f);
        pad->setVector2("dpad", dpad);
        pad->setButton("dpad/up", dpad.y > 0.5f);
        pad->setButton("dpad/down", dpad.y < -0.5f);
        pad->setButton("dpad/left", dpad.x < -0.5f);
        pad->setButton("dpad/right", dpad.x > 0.5f);
        ++deviceIndex;
    }
}

void poll(State& state) {
    if (!gWindow || !gEnabled) return;
    pollKeyboard(state, gWindow);
    pollMouse(state, gWindow);
    pollGamepads(state);
}

} // namespace

int keyForControl(const std::string& name) {
    for (const KeyName& k : kKeys)
        if (name == k.name) return k.key;
    return -1;
}

void install(State& state, GLFWwindow* window, int maxGamepads) {
    gWindow = window;
    if (!state.firstDevice(DeviceKind::Keyboard)) state.addDevice(DeviceKind::Keyboard, 0);
    if (!state.firstDevice(DeviceKind::Mouse)) state.addDevice(DeviceKind::Mouse, 0);
    for (int i = 0; i < maxGamepads; ++i)
        if (!state.device(DeviceKind::Gamepad, i)) state.addDevice(DeviceKind::Gamepad, i);
    gLastX = gLastY = 0.0;
    gHaveLast = false;   // the first frame has no delta rather than a jump from the origin
    state.setPoller(poll);
}

GLFWwindow* window() { return gWindow; }

void pushScroll(double x, double y) { gScroll += glm::vec2(float(x), float(y)); }

void setEnabled(bool enabled) { gEnabled = enabled; }

} // namespace ukiyo::input::glfw_backend
