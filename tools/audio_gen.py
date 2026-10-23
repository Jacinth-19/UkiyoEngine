#!/usr/bin/env python3
"""Afterglow's soundtrack and effects, generated.

The game ships real audio files, and committed generated assets are this repository's answer to "where does art
come from" (the icons, the brand mark and the terrain textures all work this way): this script *is* the source.
It writes three 48 kHz 16-bit mono WAVs under games/afterglow/assets/:

  music/dusk_loop.wav   26.667 s — eight bars at 72 bpm: a warm pad walking Am - F - C - G under a soft
                          pentatonic pluck. Written as one bar-aligned crossfade so it loops under a game
                          that never stops for it.
  audio/spark.wav       0.35 s — the lantern taking its flame: a filtered noise puff over a high tick.
  audio/bell.wav        3.0 s  — the gate answering: a struck FM bell with a long tail.

Deterministic: no randomness beyond a fixed-seed LCG, so a rerun produces byte-identical files.
"""

import math
import os
import struct
import wave

SR = 48000
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "games", "afterglow", "assets")


def write_wav(path, samples):
    """16-bit mono WAV, clipped softly on the way out."""
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SR)
        frames = bytearray()
        for s in samples:
            s = max(-0.98, min(0.98, s))
            frames += struct.pack("<h", int(s * 32767.0))
        f.writeframes(bytes(frames))
    print("wrote %s (%.1f s)" % (path, len(samples) / SR))


class Lcg:
    """Fixed-seed LCG: the only 'randomness' in the file, so a rerun is bit-identical."""

    def __init__(self, seed=0x5EED):
        self.s = seed

    def next(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 0xFFFFFFFF  # 0..1


def dusk_loop():
    # 72 bpm, 4/4, eight bars: bar = 3.3333 s, loop = 26.6667 s exactly.
    beat = 60.0 / 72.0
    bar = beat * 4.0
    bars = 8
    total = int(bar * bars * SR)
    # Am, F, C, G: two bars each. Roots are Hz; the pad is a small additive stack per note.
    chords = [(220.0, 261.63, 329.63), (174.61, 220.0, 261.63),
              (261.63, 329.63, 392.0), (196.0, 246.94, 293.66)]
    # Pentatonic pluck: A C D E G across two octaves — never a semitone, never a wrong note at dusk.
    pluck_scale = [440.0, 523.25, 587.33, 659.25, 783.99, 880.0]
    rng = Lcg()
    # Per-bar pluck patterns: eighth notes with a sparse on/off mask, generated once so the loop is exact.
    pattern = []
    for b in range(bars):
        row = []
        for step in range(8):
            on = rng.next()
            row.append(pluck_scale[int(rng.next() * len(pluck_scale))] if on > 0.45 else 0.0)
        pattern.append(row)

    out = [0.0] * total
    two_pi = 2.0 * math.pi
    for b in range(bars):
        chord = chords[b % len(chords)]
        start = int(b * bar * SR)
        length = int(bar * SR)
        for i in range(length):
            t = i / SR
            # Pad: each chord tone as three slightly detuned saw-ish voices, in with a slow attack, out with
            # the bar — two bars per chord means the second bar's envelope finishes inside the loop.
            env = min(1.0, t / 1.1) * min(1.0, (bar - t) / 0.5)
            pad = 0.0
            for n, f in enumerate(chord):
                for detune in (-0.15, 0.0, 0.15):
                    ph = (t * (f + detune)) % 1.0
                    saw = 2.0 * ph - 1.0
                    pad += saw * 0.055
                    pad += math.sin(two_pi * (f * 2.0) * t) * 0.02   # one octave of shine
            pad *= env
            # Sub root, felt more than heard.
            sub = math.sin(two_pi * chord[0] * 0.5 * t) * 0.16 * env
            # The pluck: eighth-note envelope, a sine with a quick decay.
            beat_i = int(t / (beat * 0.5))
            if beat_i < 8:
                f = pattern[b][beat_i]
                if f > 0.0:
                    tb = t - beat_i * beat * 0.5
                    penv = math.exp(-tb * 9.0)
                    pluck = math.sin(two_pi * f * tb) * penv * 0.30
                else:
                    pluck = 0.0
            else:
                pluck = 0.0
            out[start + i] += pad * 0.6 + sub + pluck
    # Loop crossfade: render the tail past the end and fold it back over the head, so the seam is the mix
    # with itself rather than a click.
    fade = int(0.08 * SR)
    for i in range(fade):
        k = i / fade
        out[i] = out[i] * k + out[total - fade + i] * (1.0 - k)
    out = out[:total]
    # Global soft gain: leave headroom for the game's own SFX on the music bus.
    return [s * 0.72 for s in out]


def spark():
    n = int(0.35 * SR)
    rng = Lcg(1234)
    out = [0.0] * n
    for i in range(n):
        t = i / SR
        # The puff: high-passed noise, a fast swallow.
        noise = rng.next() * 2.0 - 1.0
        puff = noise * math.exp(-t * 26.0) * 0.5
        # The tick: a short bright pair of partials, the strike of flint.
        tick = (math.sin(2 * math.pi * 2400 * t) * 0.35 + math.sin(2 * math.pi * 3700 * t) * 0.2) * math.exp(-t * 55.0)
        out[i] = puff * 0.7 + tick
    return [s * 0.65 for s in out]


def bell():
    n = int(3.0 * SR)
    out = [0.0] * n
    # FM: a carrier struck by a modulator that dies away first — the classic struck-metal spectrum.
    for i in range(n):
        t = i / SR
        menv = math.exp(-t * 1.6)
        cenv = math.exp(-t * 0.9)
        mod = math.sin(2 * math.pi * 540.0 * t) * menv * 2.6
        s = math.sin(2 * math.pi * 810.0 * t + mod) * cenv
        s += math.sin(2 * math.pi * 1620.0 * t) * math.exp(-t * 1.8) * 0.25
        attack = min(1.0, t / 0.004)
        out[i] = s * attack * 0.5
    return out


def main():
    write_wav(os.path.join(ROOT, "music", "dusk_loop.wav"), dusk_loop())
    write_wav(os.path.join(ROOT, "audio", "spark.wav"), spark())
    write_wav(os.path.join(ROOT, "audio", "bell.wav"), bell())


if __name__ == "__main__":
    main()
