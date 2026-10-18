#!/usr/bin/env python3
"""Audio + transcript -> face timeline for the Ukiyo face system.

The transcript supplies *what* is said and *where*: faster-whisper gives word timings
in the spoken language, which are matched character-by-character to the transcript
(so every transcript unit gets a time span). The mouth itself follows the audio:
vowel shape comes from LPC formants of the voiced sound, openness from the energy
envelope, so it moves with the actual speech rather than with guessed phonemes.
Emotion channels come from prosody (pitch excursions, laughter-rate amplitude
modulation) and from punctuation cues (? ! ...). Blinks are scheduled naturally.

Usage:
  face_align.py AUDIO TRANSCRIPT_TXT LANG OUT_JSON [--model small]
LANG is a BCP-47 tag: en, ja-JP, ko-KR, zh-CN, zh-TW, zh-HK (mapped to Whisper codes).
"""
import argparse
import difflib
import json
import random
import subprocess
import unicodedata

import numpy as np

SR = 16000
HOP = 160            # 10 ms
WIN = 400            # 25 ms
FPS = 100            # output channel rate

# Whisper's Cantonese ("yue") decoder returns no words on the small model, so zh-HK
# uses the Mandarin decoder: it recovers most shared Chinese characters, and unmatched
# transcript characters are interpolated. Mouth shapes come from audio, so this is safe.
WHISPER_LANG = {"en": "en", "ja-JP": "ja", "ko-KR": "ko", "zh-CN": "zh", "zh-TW": "zh",
                "zh-HK": "zh", "yue": "zh"}

# Peterson & Barney style (female) vowel formants: F1, F2 in Hz.
VOWELS = {"aa": (850, 1610), "ih": (310, 2790), "ou": (370, 950), "ee": (610, 2330), "oh": (590, 920)}
# Relative jaw opening per vowel (a is the most open).
OPEN_SCALE = {"aa": 1.0, "ee": 0.62, "oh": 0.8, "ih": 0.45, "ou": 0.5}


def load_audio(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def lpc(frame, order):
    """Autocorrelation LPC via Levinson-Durbin. Returns polynomial [1, a1..ap]."""
    r = np.correlate(frame, frame, mode="full")[len(frame) - 1:len(frame) + order]
    if r[0] <= 1e-12:
        return None
    r = r / r[0]
    a = np.zeros(order + 1)
    a[0] = 1.0
    err = r[0] + 1e-9
    for i in range(1, order + 1):
        acc = r[i] + np.dot(a[1:i], r[i - 1:0:-1])
        k = -acc / err
        a_prev = a.copy()
        for j in range(1, i):
            a[j] = a_prev[j] + k * a_prev[i - j]
        a[i] = k
        err *= (1.0 - k * k)
        if err <= 0:
            break
    return a


def formants(frame):
    """Returns (F1, F2) in Hz or None, from LPC roots."""
    w = np.hamming(len(frame))
    x = np.append(frame[0], frame[1:] - 0.97 * frame[:-1]) * w
    a = lpc(x, 12)
    if a is None:
        return None
    roots = np.roots(a)
    freqs = []
    for z in roots:
        if z.imag <= 0:
            continue
        f = np.angle(z) * SR / (2 * np.pi)
        bw = -np.log(max(abs(z), 1e-9)) * SR / np.pi
        if 150 < f < 4000 and bw < 450:
            freqs.append(f)
    freqs.sort()
    f1 = next((f for f in freqs if 200 < f < 1000), None)
    if f1 is None:
        return None
    f2 = next((f for f in freqs if f > f1 + 350 and f < 3300), None)
    if f2 is None:
        return None
    return f1, f2


def pitch(frame):
    """Autocorrelation pitch in Hz, or 0 if unvoiced."""
    x = frame - frame.mean()
    if np.sqrt(np.mean(x * x)) < 1e-4:
        return 0.0, 0.0
    lo, hi = SR // 400, SR // 70
    ac = np.correlate(x, x, mode="full")[len(x) - 1:]
    if ac[0] <= 0:
        return 0.0, 0.0
    ac = ac / ac[0]
    seg = ac[lo:hi]
    k = int(np.argmax(seg)) + lo
    return (SR / k, float(ac[k])) if ac[k] > 0.35 else (0.0, float(ac[k]))


def pulses(impulses, length_frames):
    """Widens each impulse (value at its frame) into a raised-cosine pulse of the given length."""
    out = np.zeros_like(impulses)
    shape = np.hanning(length_frames)
    for i in np.nonzero(impulses)[0]:
        seg = shape * impulses[i]
        j1 = min(len(out), i + length_frames)
        out[i:j1] = np.maximum(out[i:j1], seg[:j1 - i])
    return out


def smooth(x, attack, release):
    """One-pole attack/release follower at FPS."""
    a_att = 1.0 - np.exp(-1.0 / (attack * FPS))
    a_rel = 1.0 - np.exp(-1.0 / (release * FPS))
    y = np.zeros_like(x)
    cur = 0.0
    for i, v in enumerate(x):
        a = a_att if v > cur else a_rel
        cur += a * (v - cur)
        y[i] = cur
    return y


def frame_features(x):
    n = max(1, (len(x) - WIN) // HOP)
    rms_db = np.full(n, -120.0)
    f0 = np.zeros(n)
    corr = np.zeros(n)
    vowel_p = np.zeros((n, 5))           # aa ih ou ee oh posteriors
    names = list(VOWELS)
    tmpl = np.array([VOWELS[k] for k in names], dtype=float)
    for i in range(n):
        seg = x[i * HOP:i * HOP + WIN]
        rms = np.sqrt(np.mean(seg * seg) + 1e-12)
        rms_db[i] = 20 * np.log10(rms + 1e-9)
        f0[i], corr[i] = pitch(seg)
        if rms_db[i] > -45:
            fm = formants(seg)
            if fm is not None:
                d = np.sum(((np.array(fm)[None, :] - tmpl) / np.array([220.0, 420.0])) ** 2, axis=1)
                p = np.exp(-(d - d.min()) / 1.2)
                vowel_p[i] = p / p.sum()
            else:
                # Formants unresolved: a generic open mouth, the usual shape for speech.
                vowel_p[i] = np.array([0.5, 0.05, 0.05, 0.2, 0.2])
    return rms_db, f0, vowel_p, names


def align_units(text, whisper_words):
    """Maps transcript characters to Whisper character times. Returns list of dicts."""
    units = []
    for ch in text:
        if ch.isspace():
            continue
        cat = unicodedata.category(ch)
        if cat.startswith("P") or cat.startswith("S"):
            if units:
                units[-1]["punct"] += ch
            continue
        units.append({"ch": ch, "t0": None, "t1": None, "punct": ""})

    hyp, hyp_t = [], []
    for (t0, t1, word) in whisper_words:
        word = "".join(c for c in word if not c.isspace())
        if not word:
            continue
        step = (t1 - t0) / len(word)
        for k, c in enumerate(word):
            hyp.append(c)
            hyp_t.append((t0 + k * step, t0 + (k + 1) * step))

    ref = [u["ch"].lower() for u in units]
    hyp_l = [c.lower() for c in hyp]
    sm = difflib.SequenceMatcher(None, ref, hyp_l, autojunk=False)
    for a, b, size in sm.get_matching_blocks():
        for k in range(size):
            units[a + k]["t0"], units[a + k]["t1"] = hyp_t[b + k]

    # Interpolate any unmatched units between their matched neighbours.
    idx = [i for i, u in enumerate(units) if u["t0"] is not None]
    if not idx:
        raise RuntimeError("alignment failed: no transcript character matched speech")
    for i, u in enumerate(units):
        if u["t0"] is not None:
            continue
        prev = max([j for j in idx if j < i], default=None)
        nxt = min([j for j in idx if j > i], default=None)
        if prev is None:
            u["t0"] = units[nxt]["t0"] - 0.08
        elif nxt is None:
            u["t0"] = units[prev]["t1"]
        else:
            span = units[nxt]["t0"] - units[prev]["t1"]
            frac = (i - prev) / (nxt - prev)
            u["t0"] = units[prev]["t1"] + span * frac
        u["t1"] = u["t0"] + 0.07
    for u in units:
        u["t0"], u["t1"] = float(u["t0"]), float(max(u["t1"], u["t0"] + 0.03))
    return units


def transcribe(audio, lang, model_size):
    from faster_whisper import WhisperModel
    model = WhisperModel(model_size, device="cpu", compute_type="int8")
    segs, _ = model.transcribe(audio, language=WHISPER_LANG[lang], word_timestamps=True,
                               vad_filter=False, condition_on_previous_text=False)
    words = []
    for s in segs:
        for w in (s.words or []):
            words.append((float(w.start), float(w.end), w.word))
    return words


def laugh_mask(env_db, voiced, n):
    """Laughter: strong amplitude modulation at 4-7 Hz in a 0.4 s window, while voiced."""
    lin = np.power(10.0, (env_db - env_db.max()) / 20.0)
    out = np.zeros(n)
    win = int(0.4 * FPS)
    for i in range(win, n - win):
        if not voiced[i]:
            continue
        seg = lin[i - win:i + win]
        if seg.mean() < 0.05:
            continue
        m = seg - seg.mean()
        zc = np.sum(np.abs(np.diff(np.sign(m))) > 0) / 2.0
        rate = zc / (2 * win / FPS)
        depth = np.std(m) / (seg.mean() + 1e-6)
        if 4.0 <= rate <= 8.0 and depth > 0.45:
            out[i] = 1.0
    return out


def build_timeline(audio, units, dur, seed):
    n = int(np.ceil(dur * FPS))
    rms_db, f0, vowel_p, names = frame_features(audio)
    # Resample 10 ms feature frames to FPS (100 Hz is the same rate here).
    m = min(n, len(rms_db))
    def fit(a, shape=None):
        out = np.zeros((n,) + (shape or ()))
        out[:m] = a[:m]
        return out

    rms_db = fit(rms_db)
    f0 = fit(f0)
    vowel_p = fit(vowel_p, (5,))

    peak = rms_db.max()
    speech = np.clip((rms_db - (peak - 38.0)) / 22.0, 0.0, 1.0)
    voiced = (f0 > 0) & (rms_db > peak - 40.0)

    # Speech spans from the transcript timing (units), padded slightly.
    gate = np.zeros(n)
    for u in units:
        i0 = int((u["t0"] - 0.03) * FPS)
        i1 = int((u["t1"] + 0.03) * FPS)
        gate[max(0, i0):min(n, i1)] = 1.0
    gate = np.maximum(gate, 0.0)

    # Mouth openness follows the audio envelope, weighted by vowel shape.
    vowel_scale = np.array([OPEN_SCALE[k] for k in names])
    openness = speech * gate * (vowel_p @ vowel_scale)
    openness = np.where(voiced, openness, openness * 0.35)

    # Consonant closures: a dip of more than 10 dB below the local peak inside a speech
    # span (stops and nasals such as p, b, m, t, d, k) closes the lips for that moment.
    win = 12                                  # +-120 ms
    local_max = np.array([rms_db[max(0, i - win):i + win + 1].max() for i in range(n)])
    closure = ((rms_db < local_max - 10.0) & (gate > 0.5) & (speech > 0.15)).astype(float)
    openness = openness * (1.0 - 0.85 * closure)
    openness = np.clip(openness, 0.0, 1.0)
    openness = smooth(openness, 0.022, 0.05)

    vowel_w = {k: vowel_p[:, i] * openness for i, k in enumerate(names)}
    vowel_out = {k: np.clip(smooth(vowel_w[k], 0.03, 0.05), 0, 1) for k in names}

    # Punctuation cues at the time of each punctuated unit.
    surprise_impulse = np.zeros(n)
    question = np.zeros(n)
    beats = np.zeros(n)
    for u in units:
        i = int(u["t1"] * FPS)
        if i >= n:
            continue
        p = u["punct"]
        if "!" in p or "！" in p:
            surprise_impulse[i] = 1.0
        if "?" in p or "？" in p:
            question[max(0, i - 25):min(n, i + 60)] = 1.0
        if any(c in p for c in "。.!！…"):
            beats[i] = 1.0
    surprise_pulse = pulses(surprise_impulse, 40)     # 0.4 s burst on '!'
    question = smooth(question, 0.08, 0.25)

    # Prosody relative to the speaker's median voiced pitch.
    v = f0[(f0 > 0) & (speech > 0.3)]
    med = float(np.median(v)) if v.size else 180.0
    rel = np.where(f0 > 0, f0 / med, 1.0)
    rel_c = np.clip(rel - 1.0, -0.5, 1.0)
    # Mood: phrase-level pitch height (bright > 0, low < 0) and arousal (energy).
    bright = np.clip(smooth(np.where(voiced, rel_c, 0.0), 0.2, 0.5) * 3.0, -1.0, 1.0)
    arousal = np.clip(smooth(speech, 0.15, 0.3), 0.0, 1.0)
    bright_pos = np.clip(bright, 0.0, 1.0)
    bright_neg = np.clip(-bright, 0.0, 1.0)
    # Startle: fast upward pitch jumps while speaking.
    rel_fast = smooth(rel_c, 0.03, 0.2)
    jump = np.clip(np.diff(rel_fast, prepend=rel_fast[0]) * FPS / 6.0, 0.0, 1.0)
    startle = smooth(jump * np.clip(speech * 1.5, 0.0, 1.0), 0.02, 0.3)

    # Head nods on sentence ends and strong energy peaks.
    env = smooth(speech, 0.02, 0.08)
    for i in range(2, n - 2):
        if env[i] > 0.8 and env[i] >= env[i - 2] and env[i] >= env[i + 2] and speech[i] > 0.6:
            beats[i] = max(beats[i], 0.35)
    nod = np.clip(pulses(beats, 30), 0.0, 1.0)        # 0.3 s nod
    tilt = np.clip(question, 0.0, 1.0)

    def norm95(x):
        # Per-clip normalisation: each speaker's own 95th percentile maps to 1, so sad and
        # angry moments show up on every clip instead of only on extreme pitch.
        return np.clip(x / max(float(np.percentile(x[speech > 0.2], 95)) if np.any(speech > 0.2) else 1.0, 1e-3), 0.0, 1.0)
    low_n = norm95(bright_neg)
    energy_n = norm95(arousal)

    surprised = np.clip(0.6 * surprise_pulse + 0.45 * startle, 0.0, 1.0)
    happy = np.clip(0.08 + 0.7 * bright_pos * arousal, 0.0, 1.0)
    # Sad: sustained low pitch. Angry: low, forceful, high-energy delivery.
    sad = 0.75 * low_n * (1.0 - 0.4 * energy_n)
    angry = 0.75 * low_n * energy_n
    relaxed = np.clip(0.10 + 0.2 * question, 0.0, 1.0)
    # Brows and eye smile, bound to named morphs where the model has them.
    browup = np.clip(0.5 * surprised + 0.4 * bright_pos * arousal + 0.6 * question, 0.0, 1.0)
    browdown = np.clip(0.5 * low_n * energy_n + 0.2 * angry, 0.0, 1.0)
    eyesmile = np.clip(0.85 * happy, 0.0, 1.0)

    # Blinks: natural spacing, extra blink after pauses.
    rng = random.Random(seed)
    blink = np.zeros(n)
    t = rng.uniform(0.8, 2.0)
    blink_times = []
    while t < dur - 0.2:
        blink_times.append(t)
        t += rng.uniform(2.4, 5.2)
    for u in units:
        if u["punct"] and rng.random() < 0.5:
            blink_times.append(u["t1"] + 0.1)
    for bt in blink_times:
        i = int(bt * FPS)
        for k, (dt, val) in enumerate([(0, 0.0), (3, 0.6), (5, 1.0), (7, 1.0), (9, 0.5), (12, 0.0)]):
            if 0 <= i + dt < n:
                blink[i + dt] = max(blink[i + dt], val)
    blink = np.maximum(blink, 0)

    chans = {
        "aa": vowel_out["aa"], "ih": vowel_out["ih"], "ou": vowel_out["ou"],
        "ee": vowel_out["ee"], "oh": vowel_out["oh"],
        "blink": blink, "happy": happy, "angry": angry, "sad": sad,
        "surprised": surprised, "relaxed": relaxed,
        "browup": browup, "browdown": browdown, "eyesmile": eyesmile,
        "nod": nod, "tilt": tilt,
    }
    return {k: np.clip(np.asarray(v, dtype=float)[:n], 0.0, 1.0) for k, v in chans.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("transcript")
    ap.add_argument("lang")
    ap.add_argument("out")
    ap.add_argument("--model", default="small")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    if args.lang not in WHISPER_LANG:
        raise SystemExit(f"unsupported language {args.lang}")
    with open(args.transcript, encoding="utf-8") as f:
        text = f.read().strip()
    audio = load_audio(args.audio)
    dur = len(audio) / SR

    words = transcribe(audio, args.lang, args.model)
    units = align_units(text, words)
    chans = build_timeline(audio, units, dur, args.seed)

    out = {
        "lang": args.lang,
        "fps": FPS,
        "duration": round(dur, 3),
        "units": [{"ch": u["ch"], "t0": round(u["t0"], 3), "t1": round(u["t1"], 3), "punct": u["punct"]}
                  for u in units],
        "channels": {k: [round(float(x), 3) for x in v] for k, v in chans.items()},
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False)
    peak = {k: round(float(v.max()), 2) for k, v in chans.items()}
    print(f"{args.lang}: {len(units)} units over {dur:.2f}s; peaks {peak}")


if __name__ == "__main__":
    main()
