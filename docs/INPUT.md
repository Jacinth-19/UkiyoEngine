# Input

An action system, not a key-polling loop. A game asks whether **Jump** was pressed; a **binding** decides that Jump is
`<Keyboard>/space` on a keyboard and `<Gamepad>/buttonSouth` on a pad, and a **control scheme** decides which of the two
is live. The same file describes both, and that file is Unity's `.inputactions` JSON, so an asset authored in Unity
opens here.

The implementation is Unity's Input System minus touch and XR: Keyboard, Mouse and Gamepad devices; Button, Value and
PassThrough actions; 1D and 2D composites; Press, Hold, Tap, SlowTap and MultiTap interactions; the processors an
everyday project uses; runtime rebinding; control schemes; and the asset format in both directions.

The runtime is plain C++ with no window, no GPU and no GLFW in it, which is why `tests/input_test.cpp` can drive 120
checks of it from synthetic devices on every push. The GLFW backend is a separate target that only applications link.

```
src/input/input.h          the public API: Control, Device, Binding, Action, ActionMap, State
src/input/input.cpp        evaluation, processors, interactions, rebinding
src/input/input_asset.h    .inputactions read/write
src/input/input_asset.cpp  the JSON reader and writer
src/input/input_glfw.h     the backend: install(state, window), pushScroll
src/input/input_glfw.cpp   keyboard, mouse and gamepad polling
tests/input_test.cpp       the 120 checks CI runs
assets/input/viewer.inputactions  the standalone viewer's bindings, loaded at runtime
```

## 1. The object graph

| Object | What it is | In an asset |
| --- | --- | --- |
| `State` | The runtime: devices, maps, the current scheme, one `update(dt)` per frame | — |
| `Device` | A keyboard, a mouse or one gamepad, with a control per key/button/axis | — (built by the backend) |
| `Control` | One named value: `space`, `leftStick`, `delta`. Button, Axis or Vector2 | the last segment of a path |
| `Action` | A named thing the game reads: `Jump`, `Move`. Button, Value or PassThrough | `actions[]` |
| `ActionMap` | A group enabled together: `Gameplay`, `UI`, `Viewer` | `maps[]` |
| `Binding` | Connects an action to a control, with processors and interactions | `bindings[]` |
| `ControlScheme` | A named device set whose bindings are live together | `controlSchemes[]` |
| `Processor` | Changes a value on its way out of a binding (`invert`, `scale(factor=2)`) | `processors` text |
| `Interaction` | Turns a value change into a press/hold/tap event | `interactions` text |

A control path is `<Device>/<control>`: `<Keyboard>/space`, `<Mouse>/leftButton`, `<Gamepad>/rightStick`. The device
names are Unity's; a `Joystick` path is read as a gamepad. A path that is not `<Device>/<control>` is reported by the
loader rather than silently ignored — an asset using `<Touchscreen>/…` or `<XRController>/…` does not half-work here.

## 2. A frame

```cpp
ukiyo::input::State input;
ukiyo::input::glfw_backend::install(input, window);          // once, after the window exists

auto loaded = ukiyo::input::loadAssetFile(input, "assets/input/viewer.inputactions");
if (!loaded.ok) throw std::runtime_error(loaded.error);
for (const auto& warning : loaded.warnings) std::fprintf(stderr, "input: %s\n", warning.c_str());

input.enableMap("Viewer");
ukiyo::input::Action* quit = input.action("Viewer", "Quit");   // stable for the State's lifetime

// once a frame, after the window system has collected its events:
input.update(dt);
if (quit->triggered()) close();
```

`State::update` does four things in this order:

1. latch every control's previous value (so "pressed this frame" has one definition);
2. run the backend, which writes this frame's control values;
3. if a rebind is waiting, look for a control that just crossed the press point;
4. evaluate every action of every enabled map, latching each action's previous phase.

Only enabled maps are evaluated, and `triggered()` is a phase transition: `phase == Performed && prevPhase !=
Performed`. That is why step 1 exists and why disabling a map has to cancel its actions rather than merely stop
evaluating them (§6).

`Action` reads:

| Call | True when |
| --- | --- |
| `triggered()` | the action started performing this frame (a press, a completed hold, a satisfied tap) |
| `performed()` | the action is performing now (a held button stays true) |
| `started()` | the press began this frame — for a plain press, the same frame as `triggered()` |
| `canceled()` | the action was cancelled this frame (release, a failed tap, a disabled map) |
| `pressed()` | the value is non-zero or the phase is Started/Performed |
| `held()` | the phase is Started or Performed |
| `readFloat()` / `readVector2()` | the current value |

## 3. The asset format

The reader takes Unity's shape and reports everything it cannot honour, by name, in `AssetLoadResult::warnings`. It
does not drop a binding in silence.

```json
{
  "name": "Ukiyo Viewer",
  "maps": [{
    "name": "Viewer",
    "actions": [
      { "name": "Move", "type": "Value", "id": "…", "expectedControlType": "Vector2",
        "processors": "", "interactions": "", "initialStateCheck": true }
    ],
    "bindings": [
      { "name": "WASD", "id": "…", "path": "2DVector", "action": "Move",
        "isComposite": true, "isPartOfComposite": false },
      { "name": "up", "id": "…", "path": "<Keyboard>/w", "action": "Move",
        "isComposite": false, "isPartOfComposite": true }
    ]
  }],
  "controlSchemes": [
    { "name": "Keyboard&Mouse", "bindingGroup": "Keyboard&Mouse",
      "devices": [{ "devicePath": "<Keyboard>", "isOptional": false, "isOR": false }] }
  ]
}
```

What is read and written:

| Field | Behaviour |
| --- | --- |
| `name`, `type`, `expectedControlType` | read; `type` defaults to Button, as Unity's does |
| `processors`, `interactions` | read, parsed, and written back as the same text |
| `initialStateCheck` | read and ignored — see §6 |
| `path` | `<Device>/<control>`, or a composite name (`2DVector`, `1DAxis`, `Dpad`) |
| `isComposite` / `isPartOfComposite` | the parent and its parts; parts take their names from `name` |
| `groups` | a scheme's binding group, `&`-separated, as Unity writes it |
| `id` | kept, so saving an imported asset does not churn it; a missing id is generated deterministically |
| `controlSchemes[].devices[]` | `devicePath`, `isOptional`, `isOR`; read as a device set |

The writer emits the same fields in the same order, so saving an imported asset gives a diff of only what changed. Two
saves of the same state are byte-identical, which is what makes an asset reviewable in a pull request. `makeGuid` is
FNV-1a over a seed (`"action/Viewer/Move"`), not random: an action added in code gets the same id on every machine.

Rejected with a warning, not silently: unknown processors, unknown interactions, bindings to actions that do not exist,
bindings whose path is not a device path, and control schemes with touch or XR devices.

## 4. Composites

| Composite | Parts | Result |
| --- | --- | --- |
| `2DVector` | `up`, `down`, `left`, `right` | a Vector2: right-left, up-down |
| `1DAxis` | `positive`, `negative` | one signed float, positive minus negative |
| `Dpad` | `up`, `down`, `left`, `right` | a Vector2, digital parts |
| a Vector2 control | — | the control's own two axes |

WASD and the left stick are the same action when both are bound. A 2D composite does **not** normalize: W+D reads
`(1,1)`, length 1.41, and a character walks 1.41× faster diagonally. That is Unity's behaviour; put
`normalizeVector2` on the composite binding to fix it, which is also Unity's answer.

A part's processors run on the part's control; the composite's processors run on the sum (§6 has the exact order).

## 5. Processors and interactions

Processors, in the order written, `;`-separated:

| Processor | Effect | Defaults |
| --- | --- | --- |
| `invert` | negates | — |
| `scale(factor=)` | multiplies | 1 |
| `clamp(min=,max=)` | clamps | 0, 1 |
| `normalize(min=,max=)` | remaps `[min,max]` to `[0,1]`, no clamping | 0, 1 |
| `deadzone(min=,max=)` / `axisDeadzone` | scalar dead zone with a linear remap | 0.125, 0.925 |
| `stickDeadzone` / `deadzone` on a vector | radial dead zone; inside min reads zero, past max reads unit length | 0.125, 0.925 |
| `invertVector2` / `scaleVector2(x=,y=)` / `clampVector2` / `normalizeVector2` | the vector forms | as Unity |

Interactions:

| Interaction | Performs | Cancels |
| --- | --- | --- |
| `press` (the default) | on press | on release |
| `hold(duration=0.4)` | when the key has been held for the duration | on release, performed or not |
| `tap(duration=0.2)` | on release inside the duration | the moment the duration is exceeded, while still held |
| `slowTap(duration=0.5)` | on release after the duration | on release before it |
| `multiTap(tapCount=2,duration=0.4)` | on the release that completes the count | when the window closes without the count |

An interaction on the action applies to every binding of it, after the binding's own interactions; the same is true of
processors. That ordering is Unity's.

## 6. Unity's behaviour, including the parts that look wrong

These are deliberate. Where Unity's behaviour is surprising it is preserved and written down here, because an asset
authored in Unity has to behave the same way here. The one extension is marked as such.

1. **Started and Performed arrive together on a press.** There is no frame between them, so a game that shows "started"
   and "performed" separately sees both on the same frame. Unity does the same.
2. **A Button's value is 0 or 1.** It snaps at the press point (0.5); the underlying analogue value is gone by the time
   the action reads. A trigger bound as a Button is digital; bind it as a Value action to read travel.
3. **A Value action takes the strongest binding.** With keyboard and stick bound together, the larger contribution
   wins; equal magnitudes keep binding order. Unity's rule.
4. **Diagonals are not normalized** (§4).
5. **`normalizeVector2` scales short vectors up as well as long ones.** A stick at 10 % deflection normalizes to full
   length. This is Unity's `Vector2.normalized`; preserve it, or put `stickDeadzone` before it.
6. **A Tap cancels while still held**, at the moment its duration is exceeded — not on release. A Hold cancels on
   release, including a hold that already performed.
7. **A MultiTap sequence cancels when its window closes** instead of quietly resetting.
8. **A Tap's value is 1 while the key is down**, even though its phase is Started and it has not performed. `pressed()`
   is the "is it actuated" question; `performed()` is the interaction's question.
9. **Disabling a map cancels its actions**: the value returns to zero and one last Canceled is reported. Unity does
   this. It matters more here than there, because `triggered()` is a phase transition and an action that stopped being
   evaluated would otherwise report the same press forever.
10. **A binding with no `groups` is always live**, under every scheme. Only grouped bindings are filtered. Unity's
    rule, and the reason a keyboard binding works with no scheme set at all.
11. **Changing the control scheme re-resolves bindings.** The value has to return to zero before a new press reads as a
    press: switching from a held pad button to a held key with no gap in the value does not re-trigger. Unity behaves
    this way; the test in §9 pins it down.
12. **An action bound to a device that is not connected is inactive, not an error.** Plugging the device in later makes
    it live after `resolveBindings()`, without rebuilding the asset.
13. **`initialStateCheck` is accepted and ignored.** The engine evaluates every action every frame against the current
    control values, so an action whose control is already held reads its value on the first update either way. Unity
    uses the flag to decide whether to perform on enable; here the value is always current.
14. **Scheme groups match as substrings.** A binding in `Keyboard&Mouse` is live under the scheme whose binding group
    is `Keyboard`. Unity compares the `&`-separated tokens; this is a simplification and the one place a group name is
    interpreted loosely.
15. **A scalar processor on a vector control applies to both components** — `invert` on a stick negates both axes.
    *This is an extension.* Unity would refuse the pairing (a scalar processor cannot consume a Vector2) and require
    `invertVector2`. The extension is forgiving; an asset that uses the Unity form gets Unity's result.

Ignoring the flag in (13) and the substring match in (14) are the two places an asset could mean something subtly
different from Unity. Both are noted here rather than guessed at in code.

## 7. Rebinding

```cpp
input.startRebind(*map, *action, 0);          // binding 0 of Jump
input.onRebind = [](const std::string& path) { save(path); };
input.onRebindCanceled = [] { showPrompt(false); };
input.cancelRebind();
```

The next control to cross 0.5 on an enabled device of the binding's kind becomes the binding's path and the binding's
name. A gamepad button cannot capture a keyboard binding. Controls already held when rebinding starts do not count —
only a new press does, which is what makes "press the key you want" behave. The binding is saved as
`<Device>/<control>`, so the result round-trips through the asset.

The target is a `(map, action, binding index)`, not a path: rebinding a composite part replaces that part, and the
composite parent stays.

## 8. Backend and wiring

`input_glfw` polls rather than installing GLFW callbacks, so it can be dropped into an application that already owns
them. Scroll is the one input GLFW cannot poll; the application's scroll callback calls
`glfw_backend::pushScroll(x, y)` and the backend accumulates it into the next frame's `<Mouse>/scroll`.

The mapping is Unity's: `leftCtrl` is `GLFW_KEY_LEFT_CONTROL`, `numpadEnter` is `GLFW_KEY_KP_ENTER`, the engine's
`dpad/up` is `GLFW_GAMEPAD_BUTTON_DPAD_UP`, and the left stick's y is positive up. `<Mouse>/position` is in pixels with
the origin at the bottom-left, as Unity reports it; GLFW's cursor origin is top-left, and the flip happens in the
backend so game code written against Unity's convention reads the same numbers. `<Mouse>/delta` is per-frame pixels,
y up. The scroll wheel is in notches.

Gamepads are matched in GLFW's joystick order to `Gamepad 0…3`; the engine supports four. A pad that is not present is
not an error — the actions bound to it simply do not fire.

Where it is used:

| Place | What it does |
| --- | --- |
| `src/systems/world.cpp` | Builds a real `Gameplay` action map from the project's actions (name + portable key). `setKeyHeld` writes the keyboard device, `actionDown` reads the action, `input()` exposes the whole state to game code. Input runs on unscaled time, so a paused game can still read a menu key. |
| `src/app/main.cpp` | The standalone viewer's bindings live in `assets/input/viewer.inputactions`; the camera, its toggles and its screenshot key are actions. |
| `editor/src/main_window.cpp` | The editor's own chrome (Ctrl+S, menu shortcuts) stays on Qt. Key presses in the Game view are forwarded to the running world as portable key text, which `World::setKeyHeld` translates to a control name. |

## 9. The test

`tests/input_test.cpp` drives the runtime with synthetic devices: no window, no GPU, no GLFW. 120 checks, grouped:

1. devices and control paths — a keyboard has `space`, a trigger is an Axis, `readPath` resolves a control;
2. button actions — press, hold, release, and `triggered()` being true for exactly one frame;
3. value actions and composites — WASD, gamepad stick, 1D axis reading negative, `normalizeVector2` on a composite;
4. processors — invert, normalize, scale, radial dead zone, an unknown processor reported by name;
5. interactions — tap (including the cancel-while-held), hold, slow tap, multi tap and the window closing;
6. maps and schemes — the same key in two maps, enabling one, disabling it, filtering by scheme;
7. the asset format — a Unity-shaped file, warnings, save/load/save byte equality, a file on disk;
8. rebinding — a device-kind mismatch ignored, the new key working and the old one dead, cancel leaving it alone;
9. absent devices — inactive, then live when hot-plugged, then dead when disabled;
10. pass-through actions and the `onActionPerformed` callback;
11. parser edges — interactions, processors, GUID stability, portable key names;
12. the shipped `viewer.inputactions`: every action the viewer reads exists, and WASD/E/Q/wheel drive it.

Run it anywhere:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target input_test
./build/input_test
```

CI runs it on every push (`Run Input System Test` in `.github/workflows/build.yml`) on a machine with no GPU. The test
is what makes the asset format safe to change: the round trip fails the build if a save stops reproducing its input.

Three bugs it caught before the system shipped, each now covered:

- `ActionMap::add` returned a reference into a `std::vector`, so adding a second action invalidated the first. Actions
  and maps are `std::deque`s now; a cached `Action*` survives setup, which is how game code uses it.
- A Button action was marked Performed whenever its value was past the press point, even with a Tap attached, so a tap
  fired on press. Interactions own the phase now.
- Disabling a map left the phases frozen, so `triggered()` reported the same press forever. Disabling cancels (§6.9).

## 10. What is not here

Touch, XR, `PlayerInput`, `InputUser` pairing and multi-player device assignment; control path wildcards
(`<Keyboard>/*`); per-binding press points; `press` interaction parameters; `Dpad` as a non-composite control; and the
editor's rebinding overlay (the API is there — §7 — the UI is not). A device is one keyboard, one mouse and up to four
gamepads, which is the shape this engine's games use.

## 11. Rebuilding the viewer asset

The viewer's file is the source of truth for its bindings; if an action is renamed there, the build fails at §9.12
rather than the viewer silently losing a key. To regenerate it from code, build the state you want and call
`saveAssetFile(state, "assets/input/viewer.inputactions", &error)`; the writer's output is stable, so the diff will be
only the change.
