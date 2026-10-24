# Lantern Run on a phone

The `android` job builds an installable APK of the first game. It is the same game the Linux and Windows packages run —
the same project folder, the same scene, the same Lua — with three things swapped for the platform:

| Desktop | Android |
| --- | --- |
| a GLFW window | the activity's `ANativeWindow`, through the NDK's native-activity glue |
| keyboard and mouse | two thumbs: walk on the left half, look on the right |
| the checkout's paths | the project staged out of the APK into the app's storage on first launch |

There is no Java in the APK. `AndroidManifest.xml` sets `android:hasCode="false"` and points `NativeActivity` at
`libukiyo_player.so`, so the process is the engine from `android_main` down.

## What the APK contains

```
lib/arm64-v8a/libukiyo_player.so     the player, engine, renderer, physics, Lua, audio
lib/x86_64/libukiyo_player.so        the same, for an emulator
assets/games/afterglow/…            the game that boots: scene, script, music, locales
assets/games/lantern-run/…           the first game, staged beside it
assets/assets/AliciaSolid_vrm-0.51.vrm   the character the scene references (../../../assets/…)
assets/editor/res/fonts/…            the font the manifest names
assets/shaders/spv/…                 the compiled Vulkan shaders
assets/files.txt                     every file above, one per line
```

`android_main.cpp` reads `files.txt`, copies each file into `internalDataPath`, and `chdir`s there. That is why the
project keeps its repo-relative layout: `../../assets/AliciaSolid_vrm-0.51.vrm` in the scene resolves inside the app's
storage exactly as it does on a desktop, and no path in the project is rewritten for any platform.

## Building it

CI does this (see `.github/workflows/build.yml`, job `android`), and a machine with the NDK does the same by hand:

```
cmake -B build-android-arm64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build build-android-arm64 --parallel

python3 tools/shaderc.py shaders apk/assets/shaders $(command -v glslangValidator)
# assets/ as above, lib/ with the two .so files, the manifest, the icon resources and a version number, then the
# SDK's own tools:
aapt2 link -o apk/unaligned.apk -I $SDK/platforms/android-34/android.jar \
  --manifest android/AndroidManifest.xml -A apk/assets \
  --version-code "$(date +%s)" --min-sdk-version 33 --target-sdk-version 34 --dir android/res
(cd apk && zip -q -r unaligned.apk lib)
zipalign -f 4 apk/unaligned.apk apk/aligned.apk
apksigner sign --ks android/lantern-run-ci.p12 --ks-type pkcs12 --ks-pass pass:lantern-run-ci \
  --key-pass pass:lantern-run-ci --out LanternRun.apk apk/aligned.apk
apksigner verify LanternRun.apk
adb install -r LanternRun.apk
```

The APK from a CI run is attached to it as `afterglow-android` (label *Afterglow*; the player boots
`games/afterglow`, and Lantern Run is staged in the assets too).

## Signing, versionCode, and installing over the last build

Android orders installed packages by `versionCode` and refuses `adb install -r` (and every upgrade) when the incoming
number is not greater than the installed one. Two things follow, and both were wrong in the first version of this
pipeline:

* **A version per build.** CI passes `--version-code ${{ github.run_number }}`, so every APK the pipeline produces is
  newer than the last one. The manifest's `versionCode="1"` remains as the value a hand-built APK gets.
* **A key that outlives the run.** The pipeline used to run `keytool -genkeypair` for each build, which signed every
  APK with a different throwaway key: a second build could never replace the first on a phone. It now signs with
  `android/lantern-run-ci.p12`, committed in the repository — deliberately a CI convenience key with its password
  (`lantern-run-ci`) in the workflow, not a secret: it proves *that* the pipeline signed the APK, and it must never be
  used for a store release. A project with real keys sets the repository secrets `ANDROID_KEYSTORE_B64`,
  `ANDROID_KEYSTORE_PASS`, `ANDROID_KEYSTORE_ALIAS` and `ANDROID_KEYSTORE_KEY_PASS`, and those win when they are set.

```sh
apksigner verify --print-certs LanternRun.apk   # who signed it, either way
```

## The launcher icon

`android/res/mipmap-{m,h,x,xx,xxx}hdpi/ic_launcher.png` — one drawing at five densities, referenced by the manifest as
`@mipmap/ic_launcher` and linked by aapt2's `--dir android/res`. The drawing is `tools/android_icon.cpp`: the identity's
ensō, one brush stroke that runs dry into a gap, and the vermilion 浮 seal sitting in the gap, on the 藍 ground, drawn
from the same colours and the same Shippori Mincho the brand uses. It writes all five files plus `docs/icon.png`, and CI
reruns it and diffs, so a hand edit of a PNG fails the build rather than being silently overwritten later.

```sh
g++ -std=c++17 -O2 -I third_party tools/android_icon.cpp -o android_icon
./android_icon --out android/res --doc docs/icon.png
```

## Background and resume

Android gives an activity a window, takes it away when the app is backgrounded, and on resume hands back a *different*
one — and a Vulkan swapchain cannot follow it. `android_main` used to build the device, the renderer and every model
once, before the main loop, against whatever window existed then; the first resume therefore drew into a destroyed
surface, which on most phones is the first time the user switches away and back.

The player now has a *session* over the GPU: it is built when `state->window` exists (`APP_CMD_INIT_WINDOW`) and torn
down when the window goes (`APP_CMD_TERM_WINDOW` — `waitIdle` first, so nothing is destroyed under a frame in flight).
Everything that is not GPU-bound — the game, the physics, the scripts, the camera, the font — survives the whole run, so
a resume costs one device creation and a model reload, not a restart, and the clock is re-based so the gap is never
simulated as a frame. Without a window the loop sleeps instead of busy-spinning.

## Why Android 13 (API 33)

The renderer is Vulkan **1.3**: dynamic rendering instead of render passes, `vkCmdBeginRendering` and friends, and no
compatibility path back to 1.0. Android's `libvulkan.so` only exports the 1.3 entry points from API 33, so linking a
1.3 renderer against an API 24 stub leaves `vkCmdBeginRendering`, `vkBindImageMemory2` and eight more undefined — which
is exactly what the first Android link said. API 33 is therefore the build's floor, and the manifest says so:
`minSdkVersion="33"` and `android.hardware.vulkan.version 0x403000`.

## Playing it

The game starts on the court. The left thumb is a virtual stick — the touch point is its centre, and the direction from
there is what the character controller gets as W/A/S/D. The right thumb orbits the camera, which follows the hero. Walk
into a paper lantern and it lifts away with an ember burst; the line under the title counts them; lighting all five
answers with a burst from the torii.

The HUD is drawn at the same proportions as the desktop frame: the layout is authored for 720p and scaled by the
backbuffer height, so a 1080p phone gets the same sizes relative to the screen.

## What is not there yet

* **Sound.** `PlayerApp` is constructed with `audio = false` on Android: the game has no audio of its own, and opening
  a device on a phone before the surface exists is a lifecycle problem worth solving when there is something to play.
* **Shader graphs.** The runtime GLSL compiler is glslang, which the NDK does not ship. `shader_compiler_stub.cpp`
  answers for it, so a graph material cannot be generated on the device. Everything a game ships is compiled already.
* **The editor.** It is a Qt program. A phone build of it is a separate project, not a configuration flag.
* **A GPU.** CI builds and signs the APK but has no device to launch it on: the picture in `docs/GAME.md` is the
  packaged game on Linux with lavapipe. The Android claim this repository makes today is "it builds, it is installable,
  and it is the same engine" — not "I ran it on my phone".
