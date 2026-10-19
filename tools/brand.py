#!/usr/bin/env python3
"""Builds the Ukiyo Engine brand marks from vector maths (Pillow, no other deps).

The mark is an ensō: one brush ring, drawn with a varying width and a small wobble, with a vermilion hanko seal
stamped into the break. The wordmark is set in IBM Plex Sans JP, the engine's bilingual typeface, so the same file
carries the Latin name and 浮世.

    python3 tools/brand.py            # writes editor/res/brand/*.png and docs/brand/*.png

Everything is drawn at 4x and box-filtered down, so the arcs stay clean at 32 px.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
FONT_DIR = os.path.join(ROOT, "editor/res/fonts")
RES_DIR = os.path.join(ROOT, "editor/res/brand")
DOC_DIR = os.path.join(ROOT, "docs/brand")

INK = (241, 235, 223)
VERMILION = (204, 72, 50)
INDIGO = (26, 40, 68)
MUTED = (196, 184, 166)

S = 4  # supersampling


def font(weight, px):
    path = os.path.join(FONT_DIR, f"IBMPlexSansJP-{weight}.ttf")
    return ImageFont.truetype(path, px)


def ensō(size, ink=INK, seal=True, ring=0.664, thickness=0.176):
    """Draws the mark into a transparent RGBA image of `size` px (before supersampling is applied by the caller)."""
    ss = size * S
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = ss * 0.5
    r0 = ss * ring * 0.5            # centreline radius
    w0 = ss * thickness * 0.5       # half-width at the thick end
    start, end = math.radians(-24.0), math.radians(-24.0 + 328.0)

    steps = 1600
    pts_out, pts_in = [], []
    width_at = []
    for i in range(steps + 1):
        t = i / steps
        th = start + (end - start) * t
        # A brush lays down more paint at the start and releases at the end, with a slow swell in between.
        w = w0 * (0.55 + 0.45 * math.sin(math.pi * min(1.0, t * 0.92 + 0.04)))
        width_at.append(w)
        wobble = ss * 0.0045 * math.sin(3.0 * th + 1.1) + ss * 0.0022 * math.sin(7.0 * th + 0.4)
        r = r0 + wobble
        # Dry-ish edge: a tiny high-frequency ripple on both sides, out of phase.
        eo = 1.0 + 0.018 * math.sin(29.0 * th)
        ei = 1.0 + 0.016 * math.sin(29.0 * th + 1.9)
        pts_out.append((cx + math.cos(th) * (r + w * eo), cy + math.sin(th) * (r + w * eo)))
        pts_in.append((cx + math.cos(th) * (r - w * ei), cy + math.sin(th) * (r - w * ei)))
    d.polygon(pts_out + pts_in[::-1], fill=ink + (255,))
    # Round caps, so the stroke reads as a loaded brush rather than a cut band.
    for th, w in ((start, width_at[0]), (end, width_at[-1])):
        r = r0 + ss * 0.0045 * math.sin(3.0 * th + 1.1)
        px, py = cx + math.cos(th) * r, cy + math.sin(th) * r
        cr = w * 0.86
        d.ellipse([px - cr, py - cr, px + cr, py + cr], fill=ink + (255,))

    if seal:
        # The hanko sits under the break, rotated a few degrees, as if stamped after the ring was drawn.
        sw = ss * 0.196
        stamp = Image.new("RGBA", (int(sw * 2), int(sw * 2)), (0, 0, 0, 0))
        sd = ImageDraw.Draw(stamp)
        pad = sw * 0.5
        sd.rounded_rectangle([pad, pad, pad + sw, pad + sw], radius=sw * 0.09, fill=VERMILION + (255,))
        # A carved character: 浮 (uku, "float"), the first character of 浮世.
        f = font("Medium", int(sw * 0.70))
        glyph = "浮"
        bbox = sd.textbbox((0, 0), glyph, font=f)
        sd.text((pad + (sw - (bbox[2] - bbox[0])) * 0.5 - bbox[0], pad + (sw - (bbox[3] - bbox[1])) * 0.5 - bbox[1]),
                glyph, font=f, fill=INK + (255,))
        stamp = stamp.rotate(-6.0, resample=Image.BICUBIC, expand=False)
        sa = math.radians(52.0)
        sx = int(cx + math.cos(sa) * r0 * 0.56 - stamp.width * 0.5)
        sy = int(cy + math.sin(sa) * r0 * 0.56 - stamp.height * 0.5)
        img.alpha_composite(stamp, (sx, sy))

    return img.resize((size, size), Image.LANCZOS)


def tracked(draw, xy, text, f, fill, spacing):
    """Draws text with letter spacing, returning the advance width."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill)
        x += draw.textlength(ch, font=f) + spacing
    return x - spacing - xy[0]


def lockup(width, height, ink=INK, seal_color=VERMILION, jp=True, subtitle="ENGINE"):
    ss_w, ss_h = width * S, height * S
    img = Image.new("RGBA", (ss_w, ss_h), (0, 0, 0, 0))
    mark_size = int(ss_h * 0.80)
    mark = ensō(mark_size // S, ink=ink)
    mark = mark.resize((mark_size, mark_size), Image.LANCZOS)
    img.alpha_composite(mark, (0, (ss_h - mark_size) // 2))

    d = ImageDraw.Draw(img)
    x = mark_size + int(ss_h * 0.14)
    f_word = font("Bold", int(ss_h * 0.40))
    f_sub = font("SemiBold", int(ss_h * 0.082))
    f_jp = font("Medium", int(ss_h * 0.095))

    word_y = int(ss_h * 0.14)
    tracked(d, (x, word_y), "UKIYO", f_word, ink + (255,), int(ss_h * 0.026))

    rule_y = int(ss_h * 0.635)
    d.rectangle([x + int(ss_h * 0.004), rule_y, x + int(ss_h * 0.30), rule_y + max(2, int(ss_h * 0.018))],
                fill=seal_color + (255,))
    tracked(d, (x + int(ss_h * 0.004), int(ss_h * 0.70)), subtitle, f_sub, ink + (235,), int(ss_h * 0.040))
    if jp:
        tracked(d, (x + int(ss_h * 0.004), int(ss_h * 0.83)), "浮世エンジン", f_jp, MUTED + (225,), int(ss_h * 0.012))
    return img.resize((width, height), Image.LANCZOS)


def sheet(mark, size=1200):
    """A review sheet: the mark on indigo, the lockup on indigo below it."""
    img = Image.new("RGB", (size, int(size * 0.70)), INDIGO)
    m = mark.resize((int(size * 0.26), int(size * 0.26)), Image.LANCZOS)
    img.paste(m, ((size - m.width) // 2, int(size * 0.05)), m)
    lk = lockup(int(size * 0.78), int(size * 0.19))
    img.paste(lk, ((size - lk.width) // 2, int(size * 0.40)), lk)
    return img


def main():
    os.makedirs(RES_DIR, exist_ok=True)
    os.makedirs(DOC_DIR, exist_ok=True)

    mark512 = ensō(512)
    mark512.save(os.path.join(RES_DIR, "ukiyo-mark.png"))
    for s in (256, 128, 64, 32):
        ensō(s).save(os.path.join(RES_DIR, f"ukiyo-mark-{s}.png"))

    lockup(1400, 380).save(os.path.join(RES_DIR, "ukiyo-lockup.png"))
    lockup(1400, 380, ink=INDIGO, subtitle="ENGINE").save(os.path.join(DOC_DIR, "ukiyo-lockup-dark.png"))

    # The splash mark: a stacked lockup, because the splash screen is centred and tall.
    w, h = 760, 640
    stack = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    m = ensō(300).resize((300, 300), Image.LANCZOS)
    stack.alpha_composite(m, ((w - 300) // 2, 0))
    d = ImageDraw.Draw(stack)
    f_word = font("Bold", 96)
    word = "UKIYO"
    tw = sum(d.textlength(c, font=f_word) for c in word) + 12 * (len(word) - 1)
    x = (w - tw) / 2
    for c in word:
        d.text((x, 344), c, font=f_word, fill=INK + (255,))
        x += d.textlength(c, font=f_word) + 12
    d.rectangle([(w - 120) // 2, 474, (w + 120) // 2, 482], fill=VERMILION + (255,))
    f_sub = font("SemiBold", 25)
    sub = "ENGINE"
    tw = sum(d.textlength(c, font=f_sub) for c in sub) + 11 * (len(sub) - 1)
    x = (w - tw) / 2
    for c in sub:
        d.text((x, 512), c, font=f_sub, fill=INK + (235,))
        x += d.textlength(c, font=f_sub) + 11
    f_jp = font("Medium", 27)
    jp = "浮世エンジン"
    tw = sum(d.textlength(c, font=f_jp) for c in jp) + 6 * (len(jp) - 1)
    x = (w - tw) / 2
    for c in jp:
        d.text((x, 566), c, font=f_jp, fill=MUTED + (220,))
        x += d.textlength(c, font=f_jp) + 6
    stack.save(os.path.join(RES_DIR, "ukiyo-splash.png"))

    sheet(mark512).save(os.path.join(DOC_DIR, "logo-sheet.png"))
    print("wrote brand assets")


if __name__ == "__main__":
    main()
