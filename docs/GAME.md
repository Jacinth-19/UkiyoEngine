# The first game: Lantern Run

The engine has had a player, a scene format and a script API for a while. What it did not have was a *game* — a project
that is not a test fixture, that someone can open, play and ship. Lantern Run is that project, and it is deliberately
small: one courtyard, five paper lanterns, one character and one script.

```
ukiyo_player --project games/lantern-run --build Build/build.json
```

WASD walks, the camera follows, Escape quits. Walk into a lantern and it lifts away with an ember burst; the HUD counts
them. When the fifth goes up, the torii answers with its own burst and the line under the title changes.

![The court at night](lantern-run.png)

*The frame the `gpu` job captures from the packaged game, on lavapipe: `renders/lantern-run.png`, published to
`docs/lantern-run.png` by CI.*

## What it exercises

Every system in the game already existed; the game is the proof that they work together on a real project.

| System | In Lantern Run |
| --- | --- |
| VRM character | `Hero` is `assets/AliciaSolid_vrm-0.51.vrm`, driven by the capsule character controller |
| Scene format | `Scenes/NightCourt.ukscene` — 16 instances, one hierarchy level, five collectibles |
| Script API | `Scripts/game.lua` — `getPosition`, `setPosition`, `spawnEffect`, `hud` |
| Physics | the court's static box, so the character has a floor to walk on |
| Lights | one moonlight directional and three warm points, with PCSS shadows |
| Look | outlines, cel banding, bloom, SSAO, contact shadows, fog, ACES tonemapping |
| HUD | the player's own layout; the game writes one line into it |

The script is the part worth reading: it bobs each lantern around its rest position, measures the hero's distance in the
horizontal plane, and on a pickup moves the lantern out of the court and fires a particle burst. That is the whole game,
and it is 60 lines of Lua with no engine changes.

Two engine changes were needed to make a game *possible* rather than to make this one:

* **every instance gets an entity.** Previously only instances with a component did, so a script could not name a prop
  with no body — `ukiyo.setPosition("Lantern 3", …)` had nothing to move. An instance without a component is now an inert
  entity: a map entry, and nothing per frame.
* **`ukiyo.hud(text)`.** The player's HUD had a message line that only the player could write. A game can now write it
  too; the last line a script sets is what the frame shows, and an empty string leaves the engine's own messages alone.

## The art is code

`games/lantern-run/assets/*.glb` are generated, not drawn: `tools/lantern_assets.cpp` builds a stone platform, a torii
gate, a stone lantern and a paper lantern out of boxes, cylinders and cones, and writes them as glTF 2.0 binary.

```
./build/lantern_assets --out games/lantern-run/assets          # rewrite them
./build/lantern_assets --out games/lantern-run/assets --check  # fail if the committed files differ
```

CI runs the check, so the committed binaries and the generator cannot drift apart. 878 triangles for the whole court;
four files, 81 KB. The materials are simple on purpose — a base colour, an emissive factor for the paper — because they
are what the engine's loader reads from any glTF, and a first game should not depend on an exporter's extensions.

The character is the one real asset: `assets/AliciaSolid_vrm-0.51.vrm`, a VRM 0.x model the engine's loader already
handles, referenced the way the player's own fixtures reference it (`../../assets/…`).

## Checking it

`game_test` runs in the portable job on Linux and Windows and needs no device. It loads the manifest, loads the scene
with the same reader the player uses, and checks the things that break silently:

* every instance's model path exists on disk;
* every GLB is a glTF 2.0 binary whose header length matches the file, with positions, normals and indices on every
  primitive, and at least the triangle count the game expects;
* the hero has a character component;
* the script names every lantern the scene places — a typo in a name is a lantern that can never be gathered, and the
  scene would still load perfectly;
* the lighting and the look are still what the game was designed with.

The device half is in the `gpu` job: it packages the game, runs the package's own launcher on lavapipe, and requires
`player: 1 script running`, no `player: warning:` line, and a frame that `tools/frame_check.py` accepts.

## The three builds

| Platform | What CI produces | Where it runs |
| --- | --- | --- |
| Linux | `lantern-run-linux.zip` and a folder package | the `gpu` job runs the package's launcher on lavapipe and captures `docs/lantern-run.png` |
| Windows | `lantern-run-windows.zip`, built and packaged by MSVC | not run in CI: the Windows runner has no Vulkan device |
| Android | `lantern-run-android`: a signed APK, arm64 + x86_64, no Java | built and signed, not launched: CI has no phone ([docs/ANDROID.md](ANDROID.md)) |

The desktop packages carry the player binary, the project tree at its repo-relative path (`games/lantern-run/…`), the
character and font it references, and the compiled shaders. The APK carries the same trees inside its assets.

## Making it yours

Copy the folder, change `Build/build.json`'s `name`, and edit `NightCourt.ukscene`. The editor opens the same project:
`ukiyo_editor --project games/lantern-run`. Everything the scene uses — components, lights, settings — is the format the
editor writes; nothing here is player-only.
