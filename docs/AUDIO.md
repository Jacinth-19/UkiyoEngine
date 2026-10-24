# Audio

One engine, three mixer buses, and voices that behave the way a game expects them to. The whole surface is
`src/systems/audio.h`; the mixer itself is ~200 lines in `audio.cpp`, built on miniaudio's node graph.

## The graph

```
voices ──> Music   ─┐
voices ──> Sfx     ─┼──> Master (the engine volume) ──> device
voices ──> Ambient ─┘
```

Each bus is one custom node (`BusNode`). Its input bus is where the graph pre-mixes every voice attached to that
bus — miniaudio does the summing — and the node then applies, in order, the **reverb insert** and the **bus
volume**, and writes to the next node. Wet = 0 is an exact `x * volume` pass-through: a game that never touches the
reverb mixes identically to the mixer that existed before it.

Why write the node: miniaudio ships filters, a splitter and a delay, but no gain node and no reverb — and a mixer
whose three buses are one struct with one processor is easier to audit than three graphs assembled from parts that
each behave slightly differently. The reverb is a Schroeder/Four-parallel-combs-then-two-allpasses design
(Freeverb's shape, comb lengths scaled from 44.1 kHz to this engine's fixed 48 kHz), per channel, so a stereo bus
rings in stereo.

The bus node is flagged `MA_NODE_FLAG_CONTINUOUS_PROCESSING`, which is what makes a reverb tail ring into silence
after the last voice ends instead of being cut at the source's final frame.

## Voices

`load(path)` registers a sound file (it opens and decodes it once, to prove the file is real, then closes it).
`play(id)` creates a **voice**: its own `ma_sound`, opened fresh, attached to a bus, started. Voices overlap —
playing the same sound twice is two simultaneous voices, which is what `audio.h` has always claimed and what the
mixer now guarantees.

The **voice limit** (48 by default, `setVoiceLimit`) steals the oldest voice when the count is reached, so a game
that emits a sound effect per hit cannot exhaust memory by holding the trigger. Voices that reach their end are
reaped on `tick()` (called from `World::tick`, and from `pump()` in headless tests).

Per-sound defaults — `setVolume`, `setLooping`, `setSpatial`, `setPosition` — are stored per registered id and
applied to every new voice of that sound, and to voices already sounding. That is why the player's sound components
(component volume, loop flag, world position) work exactly as they did when there was one sound object per id.

## Music

```cpp
playMusic(path, loop, fadeInSeconds);   // streaming, on the Music bus, immune to the voice limit
stopMusic(fadeOutSeconds);              // scheduled on the audio thread; completes with no tick
musicPlaying();
```

The file is opened without decoding it into memory — a five-minute track costs a stream, not 50 MB. Fades use
miniaudio's own scheduling (`ma_sound_set_fade_in_pcm_frames`, `ma_sound_stop_with_fade_in_pcm_frames`), so a fade
finishes on the audio thread whether or not the game ticks (an Android activity in the background still fades its
music out).

## Buses from scripts

| Call | Meaning |
| --- | --- |
| `ukiyo.playMusic(path, loop[, fadeSeconds])` | start streaming music; `loop` truthy loops |
| `ukiyo.stopMusic([fadeSeconds])` | fade or stop the music |
| `ukiyo.setBusVolume("music"\|"sfx"\|"ambient"\|"master", v)` | bus gain, 0..4; `master` is the engine volume |
| `ukiyo.setBusReverb("music"\|"sfx"\|"ambient", wet)` | 0 = dry (exact), 1 = fully through the insert |
| `ukiyo.playSound(path[, name[, bus]])` | as before, but the third argument routes it (`music`, `sfx`, `ambient`) |

An unknown bus name is a script error with the valid names in the message — never a silent no-op.

## Platforms

The same `AudioEngine` runs on Linux, Windows and Android. miniaudio picks the platform backend (AAudio on
Android API 26+, OpenSL ES below it; WASAPI on Windows; PulseAudio/ALSA on Linux). If the device refuses to open,
`ok()` goes false with `lastError()` set and every call degrades to a no-op — the game runs silent instead of
crashing. The Android player now opens the device (`PlayerApp app(true)`); a headless run (CI, screenshots) passes
`noDevice` instead, and `pump()` pulls frames for tests.

## What is tested

`tests/systems_test.cpp` drives the mixer headlessly, with no device: overlap, the voice limit, mute-by-bus, a
reverb tail that is present with wet = 1 and exactly silent with wet = 0, music fading in, a faded stop completing
on its own, and the pre-existing spatial near/far ratio. `World::tick` reaps voices; the Lua entry points are
registered next to `playSound`.

## What is not here

* Per-voice effect sends (a "reverb amount" on a single sound): buses are the routing unit today; a per-voice send
  wants a voice-owned splitter per playing sound.
* Mix buses beyond three, sidechain ducking, snapshots (fading a whole bus on dialogue).
* Streaming music from inside a `.pak`: `playMusic` opens a real path, as `load` always has. A packaged game that
  wants music ships it loose next to the pak, or the VFS gains an `ma_vfs` front end.
* Doppler, pitch/buses automation curves, and MIDI — none of them have a caller in this engine's games.
