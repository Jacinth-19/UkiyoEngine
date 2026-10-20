#!/usr/bin/env python3
"""Builds the Ukiyo Engine identity from vector maths (Pillow + numpy, no other dependencies).

The mark is an ensō (円相): one breath, drawn clockwise, landing heavy at the lower left and running dry at the top.
The seal (判子) is a vermilion hanko carrying 浮 — the first character of 浮世 — and it sits in the ring's gap, not
beside it: the place the brush did not close is the place the name is stamped. That is the whole idea, and it is why
the mark needs no second element.

The wordmark is set in **Shippori Mincho**, a Japanese Mincho whose Latin is drawn with the same brush-derived
contrast as the kana, so the two lines of the lockup belong to the same hand. Mincho — high contrast, triangular
serifs — is the register a Japanese publisher uses for a title, and it is the register a print-era identity wants.
IBM Plex Sans JP remains the interface face (see editor/src/icons.cpp); the display face is for the identity only.

The Japanese second name 浮世エンジン is set in the same Mincho, small and tracked, under the Latin. Latin leads.

    python3 tools/brand.py [repo-root]      # writes editor/res/brand/*.png and docs/brand/*.png

Requires: pillow, numpy, and a `fonttools` install is only needed for tools/subset_brand_font.py.
Everything is drawn at 3x and filtered down, so the ring's edge stays clean at 32 px.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
FONT_DIR = os.path.join(ROOT, "editor/res/fonts")
RES_DIR = os.path.join(ROOT, "editor/res/brand")
DOC_DIR = os.path.join(ROOT, "docs/brand")

INK = (241, 235, 223)        # warm off-white: the paper of a woodblock print
VERMILION = (204, 72, 50)    # 朱, the seal
INDIGO = (26, 40, 68)        # 藍, the night ground
MUTED = (176, 166, 152)
PAPER = (243, 238, 228)
PAPER_INK = (150, 140, 126)

MINCHO_BOLD = os.path.join(FONT_DIR, "ShipporiMinchoDisplay-Bold.ttf")
MINCHO = os.path.join(FONT_DIR, "ShipporiMinchoDisplay-Regular.ttf")
PLEX = os.path.join(FONT_DIR, "IBMPlexSansJP-{}.ttf")

SS = 3  # supersampling


# --------------------------------------------------------------------------------------------- brush

def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def noise(shape, freq, seed):
    rng = np.random.default_rng(seed)
    gh, gw = max(2, int(shape[0] / freq)), max(2, int(shape[1] / freq))
    g = rng.random((gh, gw)).astype(np.float32)
    im = Image.fromarray((g * 255).astype(np.uint8), "L").resize((shape[1], shape[0]), Image.BICUBIC)
    a = np.asarray(im, np.float32) / 255.0
    return (a - a.min()) / max(1e-6, a.max() - a.min())


def stroke_field(px, ring=0.775, thick=0.185, gap_deg=30.0, start_deg=-58.0, seed=3):
    """Alpha of the ensō at `px` square pixels (already supersampled).

    Coordinates are normalised to -1..1. `s` is the arc angle from the landing, unsigned, 0..360: the stroke runs
    clockwise from `start_deg` for `360 - gap_deg` degrees. Everything is a function of (t, u) — position along the
    stroke and offset across it — which is what makes the brush behave like a brush.
    """
    yy, xx = np.mgrid[0:px, 0:px].astype(np.float32)
    c = (px - 1) * 0.5
    dx, dy = (xx - c) / c, (yy - c) / c
    r = np.hypot(dx, dy / 0.973)          # the hand draws slightly wide
    th = np.degrees(np.arctan2(dy, dx))

    sweep = 360.0 - gap_deg
    R = ring * 0.5
    s = (th - start_deg) % 360.0
    t = np.clip(s / sweep, 0.0, 1.0)

    # The centreline opens outward as the stroke turns, with a slow wobble: never concentric.
    drift = 0.018 * smooth(0.08, 0.95, t)
    wob = 0.0060 * np.sin(np.radians(th) * 3.0 + 1.1) + 0.0028 * np.sin(np.radians(th) * 7.0 + 0.4)
    centre = R * (1.0 + drift) + wob * 0.55

    # Width: lands heavy, eases through the body, releases to a dry point over the last third.
    w = (0.94 + 0.06 * smooth(0.0, 0.05, t)) * (1.0 - 0.09 * smooth(0.05, 0.50, t))
    w = w * (0.04 + 0.96 * (1.0 - smooth(0.46, 1.0, t)) ** 1.25)
    half = thick * 0.5 * w

    # Distance to the centreline, plus arc length in the gap, so both ends cap round.
    past = np.where(s <= sweep, 0.0, np.minimum(s - sweep, 360.0 - s)) / sweep
    d = np.hypot(r - centre, past * math.radians(sweep) * R)
    edge = np.maximum(half * 0.05, 0.75 / px)
    body = 1.0 - smooth(-edge, edge, d - half)

    u = (r - centre) / np.maximum(half, 1e-7)
    # The loaded (outer) edge carries a hair of chatter; the trailing side is smooth, as a brush drags.
    rag = noise((px, px), max(1.5, px * 0.010), seed + 5)
    body *= 1.0 - 0.55 * smooth(0.45, 1.0, u) * smooth(0.55, 0.98, rag) * 0.65

    # The last of the ink splits into three strands at the loaded edge, each ending at its own point.
    rng = np.random.default_rng(seed)
    hair = np.zeros_like(body)
    for i, t0 in enumerate((0.86, 0.90, 0.93)):
        f = 0.16 + 0.30 * (i / 2.0)
        end = min(1.0, t0 + rng.uniform(0.045, 0.075))
        off = (f + rng.uniform(-0.03, 0.03)) * half * 2.0
        hw = np.maximum(half * 0.30, 1.5 / px) * (1.0 - smooth(t0, end, t)) ** 0.6
        hw = np.maximum(hw, 1e-7)
        arc = smooth(t0 - 0.01, t0 + 0.01, t) * (1.0 - smooth(end - 0.01, end, t))
        hair = np.maximum(hair, (1.0 - smooth(0.25, 1.0, np.abs(r - centre - off) / hw)) * arc)

    dryness = smooth(0.94, 1.0, t)
    alpha = np.maximum(body * (1.0 - dryness * 0.55), hair)

    # Paper grain, light enough not to read as noise at 1x.
    alpha *= 0.95 + 0.05 * noise((px, px), 1.4, seed + 9)

    # The landing's cap, unioned rather than masked, so the start is round.
    a0 = math.radians(start_deg)
    hx, hy = R * math.cos(a0), R * math.sin(a0) * 0.973
    hr = thick * 0.5 * 0.96
    alpha = np.maximum(alpha, 1.0 - smooth(hr * 0.90, hr * 1.02, np.hypot(dx - hx, dy - hy)))
    return np.where(s <= sweep + 1e-3, np.clip(alpha, 0.0, 1.0), 0.0)


def hanko(px, glyph="浮", seal=VERMILION, ink=INK):
    """A stamped hanko: a vermilion block with the character cut out of it.

    The character is a hole, not white paint: on a dark ground it shows the ground, which is what a seal on paper
    does when the paper is dark. `glyph=""` gives the bare block for the smallest sizes.
    """
    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = px * 0.05
    d.rounded_rectangle([pad, pad, px - pad, px - pad], radius=max(2, int(px * 0.08)), fill=seal + (255,))
    if glyph:
        cut = Image.new("L", (px, px), 0)
        cd = ImageDraw.Draw(cut)
        f = ImageFont.truetype(MINCHO_BOLD, max(6, int(px * 0.60)))
        bbox = cd.textbbox((0, 0), glyph, font=f)
        cd.text(((px - (bbox[2] - bbox[0])) * 0.5 - bbox[0], (px - (bbox[3] - bbox[1])) * 0.5 - bbox[1]),
                glyph, font=f, fill=255)
        if px >= 48:
            cut = cut.filter(ImageFilter.MaxFilter(3))   # a hair thicker, so it survives at 48 px
        a = np.asarray(img.getchannel("A"), np.float32)
        k = np.asarray(cut, np.float32) / 255.0
        img.putalpha(Image.fromarray(np.clip(a * (1.0 - k * 0.97), 0, 255).astype(np.uint8), "L"))
    # Pigment missing in bites along the edge: pressed paper, not a rounded rectangle.
    d2 = ImageDraw.Draw(img)
    rng = np.random.default_rng(4)
    n = 30 if px > 60 else 14
    for k in range(n):
        ang = k * (2 * math.pi / n) + 0.3
        ex = pad + (0.5 + 0.5 * math.cos(ang)) * (px - 2 * pad)
        ey = pad + (0.5 + 0.5 * math.sin(ang)) * (px - 2 * pad)
        er = px * (0.0018 + 0.005 * rng.random())
        d2.ellipse([ex - er, ey - er, ex + er, ey + er], fill=(0, 0, 0, 0))
    return img


def mark(size, ink=INK, seal=VERMILION, glyph="浮", with_seal=True, ring=0.775, thick=0.185):
    """The mark: the ensō alone. The seal is placed by `lockup`/`splash`, where it lives in the ring's gap."""
    px = size * SS
    a = stroke_field(px, ring=ring, thick=thick)
    im = Image.new("RGBA", (px, px), ink + (0,))
    im.putalpha(Image.fromarray((a * 255).astype(np.uint8), "L"))
    return im.resize((size, size), Image.LANCZOS)


def mark_with_seal(size, ink=INK, seal=VERMILION, glyph="浮"):
    """The mark with the seal sitting in the ring's gap: the lockup's left element."""
    px = size * SS
    a = stroke_field(px)
    im = Image.new("RGBA", (px, px), ink + (0,))
    im.putalpha(Image.fromarray((a * 255).astype(np.uint8), "L"))
    # The gap is centred straight up. The stamp sits *in* it, on the centreline, and is sized to the gap's own width
    # plus a hair: it bridges the opening rather than floating above it.
    sw = int(px * 0.198)
    s = hanko(sw, glyph=glyph, seal=seal, ink=ink).rotate(-2.0, resample=Image.BICUBIC, expand=True)
    gap_centre = math.radians(-58.0 + 330.0 + 15.0)
    c = (px - 1) * 0.5
    # Normalised radius 1.0 is c pixels, so the ring's radius of `ring/2` normalised units is ring * c / 2 pixels.
    # (Writing px * ring * 0.5 put the stamp twice as far out as the ring and made it float above the mark.)
    Rr = 0.775 * c * 0.5
    x = c + math.cos(gap_centre) * Rr * 0.90 - s.width * 0.5
    y = c + math.sin(gap_centre) * Rr * 0.90 - s.height * 0.5 + px * 0.012
    im.alpha_composite(s, (int(x), int(y)))
    return im.resize((size, size), Image.LANCZOS)


# --------------------------------------------------------------------------------------------- type

def tracked(d, xy, text, f, fill, spacing):
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + spacing
    return x - spacing - xy[0]


def tracked_width(d, text, f, spacing):
    return sum(d.textlength(c, font=f) for c in text) + spacing * (len(text) - 1)


def stack_text(d, centre_x, y, text, f, fill, spacing):
    w = tracked_width(d, text, f, spacing)
    tracked(d, (centre_x - w * 0.5, y), text, f, fill, spacing)


def lockup(width, height, ink=INK, jp=True, subtitle="ENGINE"):
    """Mark on the left, the Latin name and its two sub-lines on the right.

    Mincho's serifs give the wordmark a top and a bottom line that a sans does not have; the sub-lines are tracked to
    the wordmark's optical left edge, which is the difference between a logo that looks set and one that looks
    assembled. No rule under UKIYO: the seal is the only accent on the page.
    """
    w, h = width * SS, height * SS
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    mark_h = int(h * 0.94)
    m = mark_with_seal(mark_h, ink=ink)
    img.alpha_composite(m, (int(h * 0.01), (h - mark_h) // 2))

    d = ImageDraw.Draw(img)
    x = mark_h + int(h * 0.16)
    f_word = ImageFont.truetype(MINCHO_BOLD, max(8, int(h * 0.40)))
    f_sub = ImageFont.truetype(PLEX.format("Medium"), max(6, int(h * 0.082)))
    f_jp = ImageFont.truetype(MINCHO, max(6, int(h * 0.105)))

    word_sp = int(h * 0.028)
    word_y = int(h * 0.14)
    tracked(d, (x, word_y), "UKIYO", f_word, ink + (255,), word_sp)

    sub_sp = int(h * 0.040)
    tracked(d, (x + int(h * 0.008), int(h * 0.615)), subtitle, f_sub, ink + (215,), sub_sp)
    if jp:
        tracked(d, (x + int(h * 0.006), int(h * 0.755)), "浮世エンジン", f_jp, MUTED + (235,), int(h * 0.012))
    return img.resize((width, height), Image.LANCZOS)


def splash_mark(width=820, height=680, ink=INK):
    """The stacked lockup the splash screen places over the artwork: mark, name, two sub-lines."""
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = mark_with_seal(340, ink=ink)
    img.alpha_composite(m, ((width - 340) // 2, 0))

    f_word = ImageFont.truetype(MINCHO_BOLD, 104)
    stack_text(d, width * 0.5, 352, "UKIYO", f_word, ink + (255,), 14)

    f_sub = ImageFont.truetype(PLEX.format("Medium"), 25)
    stack_text(d, width * 0.5, 502, "ENGINE", f_sub, ink + (225,), 13)

    f_jp = ImageFont.truetype(MINCHO, 29)
    stack_text(d, width * 0.5, 552, "浮世エンジン", f_jp, MUTED + (235,), 7)
    return img


def sheet(size=1200):
    """A review sheet: the mark alone, the horizontal lockup, and the mono lockup on paper."""
    h = int(size * 0.80)
    img = Image.new("RGB", (size, h), INDIGO)
    m = mark_with_seal(int(size * 0.26), ink=INK)
    img.paste(m, ((size - m.width) // 2, int(size * 0.045)), m)

    lk = lockup(int(size * 0.80), int(size * 0.175), ink=INK)
    img.paste(lk, ((size - lk.width) // 2, int(size * 0.330)), lk)

    # The mono lockup on paper: the same artwork in indigo, which is how the mark travels to print.
    paper_h = int(size * 0.21)
    paper = Image.new("RGB", (int(size * 0.80), paper_h), PAPER)
    lk2 = lockup(int(size * 0.70), int(size * 0.135), ink=INDIGO)
    paper.paste(lk2, ((paper.width - lk2.width) // 2, (paper_h - lk2.height) // 2), lk2)
    img.paste(paper, ((size - paper.width) // 2, int(size * 0.585)))
    return img


def readme_header(width=1600, height=460):
    """The plate at the top of the README: the lockup on the night ground, with the second name set vertically.

    The vertical column is the one typographic gesture that reads as Japanese publishing rather than as a Western
    wordmark with kana attached — and it lets the Latin keep the left edge, which is where a reader starts.
    """
    img = Image.new("RGB", (width, height), INDIGO)
    d = ImageDraw.Draw(img)
    # A flat ground is a poster; a shallow gradient is a plate. Four levels up the page, nothing anyone can name.
    for y in range(height):
        t = y / max(1, height - 1)
        d.line([(0, y), (width, y)], fill=tuple(int(c * (1.06 - 0.12 * t)) for c in INDIGO))

    # No second Japanese line here: the vertical column is the second name, and saying it twice in one plate is noise.
    lk = lockup(int(width * 0.72), int(width * 0.72 / 1400 * 380), ink=INK, jp=False)
    lk = lk.crop(lk.getbbox())  # the box reserved for the omitted kana is empty; crop so the lockup sits centred
    img.paste(lk, (int(width * 0.065), (height - lk.height) // 2), lk)

    f = ImageFont.truetype(MINCHO, 56)
    glyphs = "浮世エンジン"
    x = width - int(width * 0.105)
    step = 74
    top = height / 2 - (len(glyphs) - 1) * step / 2
    for i, ch in enumerate(glyphs):
        d.text((x, top + i * step), ch, font=f, fill=MUTED, anchor="mm")

    # A full-height hairline parts the type column from the plate: the gutter of a printed page, not a rule that
    # starts and stops for no reason.
    d.line([(x - int(width * 0.062), 0), (x - int(width * 0.062), height)], fill=(48, 62, 92), width=2)
    return img


def size_strip():
    """The mark at the sizes the program actually draws it: 16 to 256 px, on both grounds."""
    w, h = 760, 300
    img = Image.new("RGB", (w, h), INDIGO)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(PLEX.format("Regular"), 13)
    x = 30
    for s in (16, 24, 32, 48, 64, 96, 128):
        # Below 40 px the ring's gap closes up; the stamp carries the mark instead.
        im = hanko(s, glyph="" if s <= 24 else "浮") if s <= 32 else mark_with_seal(s)
        img.paste(im, (x, 60 + (100 - s) // 2), im)
        d.text((x, 190), str(s), font=f, fill=(120, 132, 152))
        x += s + 34
    return img


# --------------------------------------------------------------------------------------------- main

def main():
    os.makedirs(RES_DIR, exist_ok=True)
    os.makedirs(DOC_DIR, exist_ok=True)

    # The seal belongs to the mark: drawing the ring alone and adding the stamp later produced assets that read as a
    # letter C wherever they were used on their own. Only the 32 px mark stays bare, where the glyph is mud anyway.
    mark_with_seal(512).save(os.path.join(RES_DIR, "ukiyo-mark.png"))
    for s in (256, 128):
        mark_with_seal(s).save(os.path.join(RES_DIR, f"ukiyo-mark-{s}.png"))
    # At 64 px the character inside the seal is mud; the block still reads as a dot of pigment.
    hanko(64).save(os.path.join(RES_DIR, "ukiyo-mark-64.png"))
    mark(32).save(os.path.join(RES_DIR, "ukiyo-mark-32.png"))
    hanko(128).save(os.path.join(RES_DIR, "ukiyo-seal.png"))
    hanko(64).save(os.path.join(RES_DIR, "ukiyo-seal-64.png"))
    hanko(32, glyph="").save(os.path.join(RES_DIR, "ukiyo-seal-32.png"))

    lockup(1400, 380).save(os.path.join(RES_DIR, "ukiyo-lockup.png"))
    lockup(1400, 380, ink=INDIGO).save(os.path.join(DOC_DIR, "ukiyo-lockup-dark.png"))
    splash_mark().save(os.path.join(RES_DIR, "ukiyo-splash.png"))
    sheet().save(os.path.join(DOC_DIR, "logo-sheet.png"))
    size_strip().save(os.path.join(DOC_DIR, "logo-sizes.png"))
    readme_header().save(os.path.join(DOC_DIR, "readme-header.png"))
    print("wrote brand assets")


if __name__ == "__main__":
    main()
