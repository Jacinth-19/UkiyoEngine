#!/usr/bin/env python3
"""Afterglow's three sound files, built from recorded sources.

The first version of this tool synthesized every sample from oscillators; this one starts from real recordings —
field wind, wind chimes, a bell tree, a whoosh and a sampled concert piano — and arranges them into exactly what
Scripts/game.lua asks for:

    assets/music/dusk_loop.wav   26.667 s   a seamless dusk loop: wind bed, sparse chimes, a pentatonic piano line
    assets/audio/spark.wav        0.35 s    the lantern-ignition puff
    assets/audio/bell.wav         3.0 s     the shrine bell that answers a relight

Everything is 48 kHz mono PCM16, the contract tests/afterglow_test.cpp checks (±0.12 s on each duration).

Sources (both from facebookincubator/Meta-open-sound, pinned by sha256 below):

  * Oculus Audio Pack 1 — CC BY 4.0. The credit line lives in games/afterglow/CREDITS.md and must travel with the
    built game.
  * Meta Piano — CC0-1.0, no credit required.

Usage:
    tools/afterglow_audio.py --src /path/to/Meta-open-sound --out games/afterglow/assets
    tools/afterglow_audio.py --src ... --check     # rebuild from the sources and byte-compare
    tools/afterglow_audio.py --check               # verify the shipped wavs against the pins below (no source)

The arrangement is a fixed schedule — no clocks, no randomness — so the output is byte-identical every run.
"""

import argparse
import hashlib
import os
import sys

# numpy and soundfile arrive only when the tool actually reads or writes audio — the output-pin check below runs on
# a bare python3, because CI has no reason to install a DSP stack just to hash three committed files.
np = None
sf = None


def load_deps():
    global np, sf
    import numpy
    import soundfile
    np = numpy
    sf = soundfile

RATE = 48000
MUSIC_SECONDS = 26.667
SPARK_SECONDS = 0.35
BELL_SECONDS = 3.0

# (path under --src, sha256). Every byte that reaches the game comes through this table.
SOURCES = {
    "corpora/oculus-audio-pack/ogg/UIMisc_BelltreeUp_OS_01.ogg":
        "8a464dbf3311d3f9efc42908244c008b56d4b7bfdb57dc8f6a7be8691accc1e1",
    "corpora/oculus-audio-pack/ogg/WHSH_WhooshMedium_OS_03.ogg":
        "78d60e45536e52e56fbf3f97858289cc8b3d5f19e167c5eda489bf69f68bac6e",
    "corpora/oculus-audio-pack/ogg/WINDVege_LightWindTreesBirds_LOOP_01.ogg":
        "94fc6b8764cf54f5b9d1dc95f7bce03c356fa0bf99f12c1a76b610d116812744",
    "corpora/oculus-audio-pack/ogg/MUSCChim_WindChimes_LOOP_01.ogg":
        "da68763b529b6d56fc7ee4decc9939c20d013ffa903e392a9944dbf3d3163ced",
    "systems/instruments/meta-piano/ogg/INSPiano_Vel03_A3_01.ogg":
        "850826ac767280c8d131f4992fa448b13122b5efb5d3147ff5c7c6d405e1e7f3",
    "systems/instruments/meta-piano/ogg/INSPiano_Vel03_C4_01.ogg":
        "f6df379bfb60e074fdb640eb33298a3150960b9c27fa9247740036fb8d9b55d6",
    "systems/instruments/meta-piano/ogg/INSPiano_Vel03_D4_01.ogg":
        "1454a1a58516049bb30322ce6bf640860690d4f49de8ce03bf229a14a8969008",
    "systems/instruments/meta-piano/ogg/INSPiano_Vel03_E4_01.ogg":
        "f5b314c5412e84ff9ac5ed8a92da4b3058cbfd3d4cd80c30dcfe333fb6102315",
    "systems/instruments/meta-piano/ogg/INSPiano_Vel03_G4_01.ogg":
        "ca73a2234dc42a38ed9f9e7f3f1d89e50df79bb1a5f89e1df09baeb5fce92bec",
    "systems/instruments/meta-piano/ogg/INSPiano_Vel03_A4_01.ogg":
        "304ffb429312f6a8e8dc7daf141eae3f23b4ddca147022442e9457de3f95b1d8",
}

# The dusk line: (seconds, note, gain). A-minor pentatonic, slow enough to breathe; the last notes run past the
# loop point and are wrapped around, so the seam never swallows a decay.
MOTIF = [
    (0.20, "A3", 0.50), (2.60, "E4", 0.40), (4.80, "G4", 0.45), (7.10, "A4", 0.50),
    (9.40, "E4", 0.35), (11.60, "D4", 0.40), (14.00, "C4", 0.45), (16.30, "E4", 0.40),
    (18.50, "G4", 0.45), (21.00, "A4", 0.50), (23.40, "E4", 0.40), (25.35, "A3", 0.45),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load(src, rel):
    path = os.path.join(src, rel)
    if not os.path.exists(path):
        raise SystemExit(f"afterglow_audio: missing source {rel} (clone facebookincubator/Meta-open-sound)")
    want = SOURCES[rel]
    got = sha256(path)
    if got != want:
        raise SystemExit(f"afterglow_audio: {rel} has sha256 {got}, expected {want} — upstream changed")
    data, rate = sf.read(path, dtype="float32", always_2d=True)
    if rate != RATE:
        raise SystemExit(f"afterglow_audio: {rel} is {rate} Hz, expected {RATE}")
    return data.mean(axis=1)  # stereo sources come down to the mono the game contract wants


def looped(bed, frames, fade):
    """bed[:frames] as a seamless loop: crossfade the tail back over the head."""
    if len(bed) < frames + fade:
        bed = np.concatenate([bed, bed[:frames + fade - len(bed)]])
    out = bed[:frames].copy()
    ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
    out[:fade] = out[:fade] * ramp + bed[frames:frames + fade] * (1.0 - ramp)
    return out


def build_music(src):
    frames = int(round(MUSIC_SECONDS * RATE))
    fade = int(1.25 * RATE)
    mix = looped(load(src, "corpora/oculus-audio-pack/ogg/WINDVege_LightWindTreesBirds_LOOP_01.ogg"),
                 frames, fade) * 0.34
    mix += looped(load(src, "corpora/oculus-audio-pack/ogg/MUSCChim_WindChimes_LOOP_01.ogg"),
                  frames, fade) * 0.20
    notes = {n: load(src, f"systems/instruments/meta-piano/ogg/INSPiano_Vel03_{n}_01.ogg")
             for n in ("A3", "C4", "D4", "E4", "G4", "A4")}
    for when, note, gain in MOTIF:
        start = int(when * RATE)
        wave = notes[note] * gain
        # Place, wrapping anything past the loop point back to the head — the seam must not guillotine a decay.
        head = min(len(wave), frames - start)
        mix[start:start + head] += wave[:head]
        if head < len(wave):
            wrap = len(wave) - head
            mix[:min(wrap, frames)] += wave[head:head + min(wrap, frames)]
    peak = float(np.max(np.abs(mix))) or 1.0
    return mix * (0.89 / peak)


def build_spark(src):
    whoosh = load(src, "corpora/oculus-audio-pack/ogg/WHSH_WhooshMedium_OS_03.ogg")
    frames = int(round(SPARK_SECONDS * RATE))
    out = whoosh[:frames] if len(whoosh) >= frames else np.pad(whoosh, (0, frames - len(whoosh)))
    edge = int(0.006 * RATE)
    out[:edge] *= np.linspace(0.0, 1.0, edge, dtype=np.float32)
    out[-edge:] *= np.linspace(1.0, 0.0, edge, dtype=np.float32)
    return out * (0.92 / (float(np.max(np.abs(out))) or 1.0))


def build_bell(src):
    bell = load(src, "corpora/oculus-audio-pack/ogg/UIMisc_BelltreeUp_OS_01.ogg")
    frames = int(round(BELL_SECONDS * RATE))
    out = bell[:frames] if len(bell) >= frames else np.pad(bell, (0, frames - len(bell)))
    release = int(0.45 * RATE)          # the recording runs five seconds; the contract is three — fade, don't chop
    out[-release:] *= np.linspace(1.0, 0.0, release, dtype=np.float32)
    edge = int(0.004 * RATE)
    out[:edge] *= np.linspace(0.0, 1.0, edge, dtype=np.float32)
    return out * (0.95 / (float(np.max(np.abs(out))) or 1.0))


def write_wav(path, samples):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pcm = np.clip(samples, -1.0, 1.0)
    sf.write(path, pcm, RATE, subtype="PCM_16", format="WAV")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", help="clone of facebookincubator/Meta-open-sound (required unless --check stands alone)")
    ap.add_argument("--out", default="games/afterglow/assets", help="game assets directory")
    ap.add_argument("--check", action="store_true", help="fail if the wavs on disk differ from the build or its pins")
    args = ap.parse_args()

    # The three shipped files are pinned too, so CI can verify them without cloning the source pack: a
    # byte-change anywhere in the soundtrack pipeline fails the check with the file's name.
    OUT_PINS = {
        "music/dusk_loop.wav": "c7be39881325dc68f5a7eb9ef9fc4318924240bc222f681a8151051102c43164",
        "audio/spark.wav": "57727b7b9bd27a99d40e1725413bcdb2f3922f9ddcf86cb3194ba027b4be8a22",
        "audio/bell.wav": "45233d0676fffa8f605044eb75b82b0c7550a1294a276a7e347f7df50b7019e8",
    }
    if args.check and not args.src:
        status = 0
        for rel, want in OUT_PINS.items():
            path = os.path.join(args.out, rel)
            data = open(path, "rb").read() if os.path.exists(path) else b""
            got = hashlib.sha256(data).hexdigest()
            ok = bool(data) and got == want
            print(f"{'OK' if ok else 'FAIL'} {rel}: {len(data)} bytes")
            status |= 0 if ok else 1
        return status
    if not args.src:
        ap.error("--src is required unless --check is given")

    load_deps()
    built = {
        "music/dusk_loop.wav": build_music(args.src),
        "audio/spark.wav": build_spark(args.src),
        "audio/bell.wav": build_bell(args.src),
    }
    status = 0
    for rel, samples in built.items():
        path = os.path.join(args.out, rel)
        if args.check:
            # Same library, same parameters, same samples: the bytes must match the committed file exactly.
            import io
            buf = io.BytesIO()
            sf.write(buf, np.clip(samples, -1.0, 1.0), RATE, subtype="PCM_16", format="WAV")
            have = open(path, "rb").read() if os.path.exists(path) else b""
            same = bool(have) and have == buf.getvalue()
            data, rate = sf.read(io.BytesIO(have)) if have else (np.zeros(1), 0)
            print(f"{'OK' if same else 'FAIL'} {rel}: {len(data) / max(rate, 1):.3f} s")
            status |= 0 if same else 1
            continue
        write_wav(path, samples)
        print(f"wrote {path}: {len(samples) / RATE:.3f} s @ {RATE} Hz mono")
    return status


if __name__ == "__main__":
    sys.exit(main())
