# Shipping a game

The player runs a project in place: `ukiyo_player --project games/lantern-run --build Build/build.json`. CI packages
that sample *and* the shipped game `games/afterglow` the same way — the second is how `afterglow-linux` and
`afterglow-windows` exist; everything below describes both. A build is that
plus the files that travel with it, arranged so the same relative paths still resolve. `ukiyo_pack` does the arranging,
and `ukiyo_pack --zip` turns it into the thing a player downloads.

```
./build/ukiyo_pack --project games/lantern-run --player build/ukiyo_player \
  --platform linux --shaders build/shaders --out dist/lantern-run-linux --zip dist/lantern-run-linux.zip
```

```
dist/lantern-run-linux/
  ukiyo_player            the player binary, renamed per platform (ukiyo_player.exe on Windows)
  run.sh                  ./ukiyo_player --project games/lantern-run --build Build/build.json --shaders shaders
  run.bat                 the same for Windows
  shaders/                the compiled shaders: spv/ and gl/
  package.json            every file, its size and its CRC-32
  games/lantern-run/…     the project directory, byte for byte
  assets/…                anything else the project names, at the path it names it by
```

## Why the layout is a small repository

A project's references are relative to its own directory, and they are allowed to leave it: Lantern Run's manifest points
at `../../editor/res/fonts/IBMPlexSansJP-Regular.ttf` and its scene at `../../assets/AliciaSolid_vrm-0.51.vrm`. Those are
the paths the editor and the player already resolve, and rewriting them for a build would mean parsing and editing every
scene — which is exactly where a packer starts shipping subtly different content from what was tested.

So the packer keeps the layout the references were written against. The package contains `games/lantern-run` and the
shared folders beside it, and the launcher runs the player with `--project games/lantern-run`. Nothing is rewritten, and
the packaged game is the same bytes the editor ran.

The cost is a package whose paths look like a checkout. The benefit is that "it worked on my machine" is not a category.

## What goes in

`pack::collect` starts with the project directory (everything in it, referenced or not — scripts, scenes, notes) and adds
every file the manifest and scenes name: the HUD font, each instance's model, each `script` component's Lua file and each
`audio` component's sound. A reference that does not exist, or that leaves the root the packer was pointed at, fails the
build: a package with a missing asset is worse than no package, because it fails on the player's machine.

Then the binary, the compiled shaders, and the launchers.

Shaders deserve their own paragraph. The engine builds against `UKIYO_SHADER_DIR`, an absolute path into the build tree,
which is correct for a developer and useless on a player's machine. `rhi::setShaderRoot` overrides it at runtime; the
player takes `--shaders DIR`, and the packer copies `build/shaders` into the package and puts that flag in the launcher.
A package built without shaders is refused rather than shipped black.

## Verification

Every file is recorded with its size and CRC-32 in `package.json`, and `ukiyo_pack` (and `tests/pack_test.cpp`) read the
package back: every listed file must exist, match its size and hash, and every file on disk must be listed. That catches
the three ways a package goes wrong — a file dropped, a file truncated in transit, and a file nobody meant to ship.

```
./build/ukiyo_pack --project games/lantern-run --player build/ukiyo_player --shaders build/shaders --out dist/x
# … then, later, on the machine that received it:
#   package.json lists every file, so a build server can check a download before running it
```

The zip is store-only: these files are a binary, a font, a VRM and a handful of small GLBs, so compression would buy a
few percent for a class of bug (an archive whose contents differ from the verified folder). The writer emits a fixed
timestamp, so two packs of one package are byte-identical.

## CI

The Linux job builds the package and uploads `lantern-run-linux.zip`; the Windows job does the same with MSVC and uploads
`lantern-run-windows.zip`. Both are the actual artifacts of this repository's first game, not a demonstration that the
tool runs.

The `gpu` job goes one step further: it packages the game, then runs the *package's own launcher* on lavapipe under Xvfb,
and fails unless the script is running, no warning was printed and the captured frame passes `tools/frame_check.py`. That
is the check that a package is a game rather than a folder.

## Android

The Android job compiles the portable core — the rules, the manifest, the scene reader, the HUD — for arm64-v8a and
x86_64, and runs its tests there. It does not yet build the player: the bundled GLFW has no Android platform, so a window,
a Vulkan surface from an `ANativeWindow`, and asset staging out of the APK are still to be written. Until then a Lantern
Run package can be built for Linux and Windows, and the game's logic is the same code the Android job compiles.
