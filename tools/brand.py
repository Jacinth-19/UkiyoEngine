#!/usr/bin/env python3
"""Builds the Ukiyo Engine brand marks from vector maths (Pillow, no other deps).

The mark is an ensō (円相): one brush ring drawn in a single breath, heavy where the brush lands and tapering to a
dry tail, with the gap at the top. A vermilion hanko (印) is stamped beside it. Both are Japanese print idioms, and
the seal is the only red on the page, which is how a Japanese identity usually spends its accent.

The wordmark is set in IBM Plex Sans JP, the engine's bilingual typeface, so one file carries the Latin name and
浮世エンジン. Latin leads and stays the default; the Japanese line is the second name, set small and tracked out.

    python3 tools/brand.py [repo-root]      # writes editor/res/brand/*.png and docs/brand/*.png

Everything is drawn at 4x and box-filtered down, so the ring stays clean at 32 px.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
FONT_DIR = os.path.join(ROOT, "editor/res/fonts")
RES_DIR = os.path.join(ROOT, "editor/res/brand")
DOC_DIR = os.path.join(ROOT, "docs/brand")

INK = (241, 235, 223)        # warm off-white: the paper of a woodblock print
VERMILION = (204, 72, 50)    # 朱, the seal
INDIGO = (26, 40, 68)        # 藍, the night ground
MUTED = (196, 184, 166)
PAPER = (243, 238, 228)

S = 4  # supersampling


def font(weight, px):
    return ImageFont.truetype(os.path.join(FONT_DIR, f"IBMPlexSansJP-{weight}.ttf"), max(1, int(px)))


def smoothstep(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a))) if b != a else 1.0
    return t * t * (3.0 - 2.0 * t)


def brush_profile(t):
    """The width of the stroke at `t` (0 = landing, 1 = tail), relative to the widest point.

    A brush lands with a loaded head, swells, and releases: the release is faster than the swell, and the last
    fifth is thin enough to break up. A plain sine profile, which an earlier version used, reads as a cut band.
    """
    head = 0.62 + 0.38 * smoothstep(0.0, 0.16, t)          # press in over the first sixth
    body = 1.0 - 0.10 * smoothstep(0.30, 0.55, t)          # slight easing through the body
    tail = 1.0 - 0.94 * smoothstep(0.55, 1.0, t) ** 0.85   # release, thinning to a dry point
    return head * body * tail


def ensō(size, ink=INK, seal=True, ring=0.688, thickness=0.142, seal_glyph="浮", seal_color=VERMILION):
    """Draws the mark into a transparent RGBA image of `size` px (before supersampling is applied by the caller).

    Angles are screen angles: 0 is +x, 90 is down. The stroke runs clockwise from -70 deg, round the bottom, to
    250 deg, which leaves the gap over the top of the ring — the way a hand draws a circle without closing it.
    """
    ss = size * S
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = ss * 0.5
    r0 = ss * ring * 0.5
    w0 = ss * thickness * 0.5           # half-width at the landing; the tail thins from here
    start = math.radians(-70.0)
    sweep = math.radians(320.0)

    # A hand-drawn circle is a little wider than it is tall, and never quite concentric.
    ax, ay = 1.0, 0.968
    steps = 720
    pts = []
    for i in range(steps + 1):
        t = i / steps
        th = start + sweep * t
        # Press in, ease through the body, release: no swell at either end, so the ring never reads as a comma.
        w = w0 * (0.94 - 0.56 * t + 0.16 * math.sin(math.pi * t))
        wobble = ss * 0.0048 * math.sin(3.0 * th + 1.4) + ss * 0.0021 * math.sin(7.0 * th + 0.3)
        r = r0 + wobble
        # The loaded edge carries a hair of chatter; the trailing edge is smoother, as a brush drags.
        eo = 1.0 + 0.012 * math.sin(23.0 * th)
        ei = 1.0 + 0.007 * math.sin(23.0 * th + 2.1)
        cth, sth = math.cos(th), math.sin(th)
        pts.append((cx + cth * (r * ax + w * eo), cy + sth * (r * ay + w * eo),
                    cx + cth * (r * ax - w * ei), cy + sth * (r * ay - w * ei)))

    for i in range(len(pts) - 1):
        x0o, y0o, x0i, y0i = pts[i]
        x1o, y1o, x1i, y1i = pts[i + 1]
        d.polygon([(x0o, y0o), (x1o, y1o), (x1i, y1i), (x0i, y0i)], fill=ink + (255,))

    # Round caps, at exactly the stroke's own half-width: a cut band otherwise.
    for i, th in ((0, start), (len(pts) - 1, start + sweep)):
        t = 0.0 if i == 0 else 1.0
        w = w0 * (0.94 - 0.56 * t + 0.16 * math.sin(math.pi * t))
        wobble = ss * 0.0048 * math.sin(3.0 * th + 1.4) + ss * 0.0021 * math.sin(7.0 * th + 0.3)
        px, py = cx + math.cos(th) * (r0 + wobble) * ax, cy + math.sin(th) * (r0 + wobble) * ay
        d.ellipse([px - w, py - w, px + w, py + w], fill=ink + (255,))

    if seal:
        # The hanko sits in the ring's lower right, clear of the gap, tilted as if stamped last.
        sw = int(ss * 0.152)
        stamp = Image.new("RGBA", (sw * 2, sw * 2), (0, 0, 0, 0))
        sd = ImageDraw.Draw(stamp)
        pad = sw * 0.5
        sd.rounded_rectangle([pad, pad, pad + sw, pad + sw], radius=max(2, int(sw * 0.05)), fill=seal_color + (255,))
        if seal_glyph:
            f = font("Medium", int(sw * 0.72))
            bbox = sd.textbbox((0, 0), seal_glyph, font=f)
            sd.text((pad + (sw - (bbox[2] - bbox[0])) * 0.5 - bbox[0],
                     pad + (sw - (bbox[3] - bbox[1])) * 0.5 - bbox[1]), seal_glyph, font=f, fill=INK + (255,))
        # Ink erosion on the edge, so it reads as pressed paper and not as a rectangle.
        for k in range(12):
            a = k * 0.911
            ex = pad + (0.5 + 0.5 * math.cos(a)) * sw
            ey = pad + (0.5 + 0.5 * math.sin(a)) * sw
            er = sw * (0.010 + 0.008 * ((k * 37) % 5) / 4.0)
            sd.ellipse([ex - er, ey - er, ex + er, ey + er], fill=(0, 0, 0, 0))
        stamp = stamp.rotate(-6.0, resample=Image.BICUBIC, expand=False)
        sa = math.radians(48.0)
        sx = int(cx + math.cos(sa) * r0 * ax * 0.56 - stamp.width * 0.5)
        sy = int(cy + math.sin(sa) * r0 * ay * 0.56 - stamp.height * 0.5)
        img.alpha_composite(stamp, (sx, sy))

    return img.resize((size, size), Image.LANCZOS)


def tracked(draw, xy, text, f, fill, spacing):
    """Draws text with letter spacing, returning the advance width."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill)
        x += draw.textlength(ch, font=f) + spacing
    return x - spacing - xy[0]


def tracked_width(draw, text, f, spacing):
    return sum(draw.textlength(c, font=f) for c in text) + spacing * (len(text) - 1)


def stack_text(draw, centre_x, y, text, f, fill, spacing):
    w = tracked_width(draw, text, f, spacing)
    tracked(draw, (centre_x - w * 0.5, y), text, f, fill, spacing)


def lockup(width, height, ink=INK, jp=True, subtitle="ENGINE"):
    """Mark on the left, the Latin name and its two sub-lines on the right.

    No rule under UKIYO: the seal is the only accent, and a second red mark competed with it. The sub-lines start
    on the wordmark's optical left edge, not its letterform edge, which is the small difference between a logo that
    looks set and one that looks assembled.
    """
    ss_w, ss_h = width * S, height * S
    img = Image.new("RGBA", (ss_w, ss_h), (0, 0, 0, 0))
    mark_size = int(ss_h * 0.88)
    mark = ensō(mark_size, ink=ink)
    mark = mark.resize((mark_size, mark_size), Image.LANCZOS)
    img.alpha_composite(mark, (int(ss_h * 0.01), (ss_h - mark_size) // 2))

    d = ImageDraw.Draw(img)
    x = mark_size + int(ss_h * 0.10)
    f_word = font("Bold", int(ss_h * 0.375))
    f_sub = font("Regular", int(ss_h * 0.088))
    f_jp = font("Medium", int(ss_h * 0.098))

    word_sp = int(ss_h * 0.016)
    word_y = int(ss_h * 0.175)
    tracked(d, (x, word_y), "UKIYO", f_word, ink + (255,), word_sp)

    sub_sp = int(ss_h * 0.040)
    tracked(d, (x + int(ss_h * 0.006), int(ss_h * 0.610)), subtitle, f_sub, ink + (225,), sub_sp)
    if jp:
        tracked(d, (x + int(ss_h * 0.004), int(ss_h * 0.752)), "浮世エンジン", f_jp, MUTED + (225,), int(ss_h * 0.014))
    return img.resize((width, height), Image.LANCZOS)


def splash_mark(width=760, height=640, ink=INK):
    """The stacked lockup the splash screen places over the artwork."""
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = ensō(300, ink=ink)
    img.alpha_composite(m, ((width - 300) // 2, 0))

    f_word = font("Bold", 92)
    stack_text(d, width * 0.5, 348, "UKIYO", f_word, ink + (255,), 11)

    f_sub = font("Regular", 24)
    stack_text(d, width * 0.5, 486, "ENGINE", f_sub, ink + (225,), 12)

    f_jp = font("Medium", 26)
    stack_text(d, width * 0.5, 536, "浮世エンジン", f_jp, MUTED + (225,), 6)
    return img


def sheet(size=1200):
    """A review sheet: mark on indigo, the horizontal lockup, and the mono lockup on paper."""
    h = int(size * 0.78)
    img = Image.new("RGB", (size, h), INDIGO)
    m = ensō(int(size * 0.22), ink=INK).resize((int(size * 0.22), int(size * 0.22)), Image.LANCZOS)
    img.paste(m, ((size - m.width) // 2, int(size * 0.045)), m)

    lk = lockup(int(size * 0.80), int(size * 0.175), ink=INK)
    img.paste(lk, ((size - lk.width) // 2, int(size * 0.315)), lk)

    # The mono lockup on paper: the same artwork in indigo, which is how the mark travels to print.
    paper_h = int(size * 0.21)
    paper = Image.new("RGB", (int(size * 0.80), paper_h), PAPER)
    lk2 = lockup(int(size * 0.70), int(size * 0.135), ink=INDIGO, jp=True)
    paper.paste(lk2, ((paper.width - lk2.width) // 2, (paper_h - lk2.height) // 2), lk2)
    img.paste(paper, ((size - paper.width) // 2, int(size * 0.545)))
    return img


def main():
    os.makedirs(RES_DIR, exist_ok=True)
    os.makedirs(DOC_DIR, exist_ok=True)

    ensō(512).save(os.path.join(RES_DIR, "ukiyo-mark.png"))
    for s in (256, 128):
        ensō(s).save(os.path.join(RES_DIR, f"ukiyo-mark-{s}.png"))
    # At 64 px the character inside the seal is mud, but the red block still reads as a dot of pigment; at 32 the
    # ring alone carries the mark.
    ensō(64, seal_glyph=None).save(os.path.join(RES_DIR, "ukiyo-mark-64.png"))
    ensō(32, seal=False).save(os.path.join(RES_DIR, "ukiyo-mark-32.png"))

    lockup(1400, 380).save(os.path.join(RES_DIR, "ukiyo-lockup.png"))
    lockup(1400, 380, ink=INDIGO).save(os.path.join(DOC_DIR, "ukiyo-lockup-dark.png"))
    splash_mark().save(os.path.join(RES_DIR, "ukiyo-splash.png"))
    sheet().save(os.path.join(DOC_DIR, "logo-sheet.png"))
    print("wrote brand assets")


if __name__ == "__main__":
    main()
