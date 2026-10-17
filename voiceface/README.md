# voiceface — lip-sync checker scaffold

A standalone CMake project inside the Ukiyo repo. It holds the interfaces and test scaffold for a voice-to-face pipeline, plus a lip-sync checker. It is **not** linked into the Ukiyo engine build.

## What is here

| Part | Path | Status |
|---|---|---|
| Viseme mapper (letters → mouth shapes, openness curve) | `src/viseme_mapper.cpp` | Rule-based, English letters only |
| Rig controller (blendshape weights, bone quaternions) | `src/rig_controller.cpp` | CPU-side only, no renderer |
| Sync analyzer (onset detection, lag check, CSV reader) | `src/sync_analyzer.cpp` | Working |
| Tone voice cloner | `src/tone_voice_cloner.cpp` | **A test tone, not speech** |
| CLI | `src/voiceface_sync.cpp` | `--gauntlet` and `--audio/--mouth` modes |
| Tests | `tests/voiceface_test.cpp` | 25 checks |

Interfaces: `IVoiceCloner` (text → mono float PCM), `IVisemeMapper` (text → timed viseme keys).

## Not included (out of scope for this scaffold)

- Real TTS or cloud voice adapters, and any voice model
- Facial capture
- Vulkan renderer
- Assimp export
- Consent and watermark tooling

## Build and run

```sh
cmake -S voiceface -B voiceface/build -DCMAKE_BUILD_TYPE=Release
cmake --build voiceface/build --parallel
ctest --test-dir voiceface/build --output-on-failure
./voiceface/build/voiceface_sync --gauntlet
```

### CLI

```sh
voiceface_sync --gauntlet
voiceface_sync --audio env.csv --mouth mouth.csv [--max-late-ms 150] [--max-early-ms 50]
```

- `--gauntlet` runs the built-in phrases (`one two three four five`, `bump map flip`, `oh ee ah`) through the tone voice, the mapper and the checker.
- CSV files have two columns, `time_seconds,value`, with an optional header. Times must not go backwards.
- Exit status: `0` all checks pass, `1` a check failed, `2` bad arguments or unreadable file.

## How the sync check works

1. Audio onset: the first envelope sample at or above 10% of the peak.
2. Mouth onset: the first openness sample at or above 0.2.
3. Lag = mouth onset − audio onset. It passes when lag is between −50 ms and +150 ms.

The openness values are Open 1.0, Wide 0.6, Round 0.5, Neutral 0.25, LipTeeth 0.1, Closed 0.0, Rest 0.0. Because closed consonants give zero openness, the mouth onset falls on the first vowel. The `bump map flip` gauntlet case shows this: its lag is 110 ms, within the limit, because the leading `b` is closed.

## Limits

- The tone voice is a wiring signal. Its onsets are at fixed 120 ms spacing, so the gauntlet checks plumbing and timing logic, not speech quality.
- The viseme mapper is English letter-to-shape only. It does not use phonemes.
- The envelope window is 20 ms, so onsets are only accurate to about 10 ms.
- The checker reads CSV series. It does not capture or analyse live audio or video.

## CI

`.github/workflows/voiceface-sync.yml` builds the project, runs `ctest`, and runs the gauntlet on `ubuntu-latest` whenever `voiceface/**` changes.

## Claims check

See `CLAIMS_CHECK.md` for the report's claims that still need checking against primary sources.
