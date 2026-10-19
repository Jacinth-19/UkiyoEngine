// The GLFW backend: fills a State's devices from the window once a frame.
//
// It polls rather than installing callbacks, so it can be dropped into an application that already owns GLFW's
// callbacks (the viewer does) without taking them over. Scroll is the one input GLFW cannot poll, so the application
// forwards it from its existing scroll callback with pushScroll.
#pragma once

#include <string>

struct GLFWwindow;

namespace ukiyo::input {

class State;

namespace glfw_backend {

// Creates the Keyboard, Mouse and (up to `maxGamepads`) Gamepad devices if they are not there yet and installs the
// poller. Safe to call once after the window exists; call it again after a device hot-plug.
void install(State& state, GLFWwindow* window, int maxGamepads = 4);

// The pointer the poller reads from, if install was called.
GLFWwindow* window();

// Forward the app's scroll callback here: `x` and `y` are GLFW's scroll deltas, in notches.
void pushScroll(double x, double y);

// The GLFW key a Unity keyboard control name maps to, or -1. Exposed for the backend's tests and for anything that
// wants to translate the other way.
int keyForControl(const std::string& name);

// Which device kind has focus for input, when the application routes input itself. Not used by the poller.
void setEnabled(bool enabled);

} // namespace glfw_backend
} // namespace ukiyo::input
