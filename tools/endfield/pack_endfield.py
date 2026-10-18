#!/usr/bin/env python3
"""Packs an Endfield-style texture set into the 8 slots EndfieldHybrid reads.

The shader has 8 texture slots (GraphMaterial::kTextureSlots). The look needs 14 inputs, so the
channels are packed as below. The layout is exact: each packed channel is the one the unpacked shader
read, so nothing is lost. The lookup tables share one atlas (see build_endfield.py REGIONS).

  base.png           rgb base colour, a alpha                (source: base)
  normalStyle.png    rgb tangent-space normal, a style.r     (source: normal, style)
                     profile nahida: a = ILM alpha; normal x = red, y = green, z rebuilt
  param.png          rgba param (metal, smoothness, AO, spec-ramp v)  (source: param)
  maskFlow.png       r mask.r, g mask.g, b flow.r, a flow.g  (source: mask, flow)
  faceSdf.png        rgba face SDF                           (source: faceSdf)
  emission.png       rgba emission colour and mask           (source: emission)
  highlightHair.png  r highlight.r, g highlight.g, b hair.r, a hair.g  (source: highlight, hairSpec)
  lookup.png         256x256 atlas: ramp, spec ramp, LUT, matcap regions (sources: ramp, specRamp, lut, matCap)

Source files are PNGs named by their Unity property, e.g. base.png, normal.png, param.png, style.png,
ilm.png, ramp.png, specRamp.png, lut.png, matCap.png, mask.png, faceSdf.png, highlight.png, hairSpec.png,
emission.png, flow.png. A missing source uses the Unity default for that property.

Profiles: "endfield" (default) puts style.r in normalStyle.a for EndfieldHybrid. "nahida" puts the ILM alpha
(ilm.png, alpha channel) there for NahidaHybrid, which picks the ramp row and outline colour from it.

Usage:
  pack_endfield.py <srcDir> <outDir> [--size N] [--profile P]   pack a source set
  pack_endfield.py --demo <outDir> [--profile P]                write stand-in sources, then pack them

Personal test use only. The Endfield textures are not in the repo.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_endfield import ATLAS, REGIONS  # noqa: E402  (one source of truth for the atlas)

# Unity default for each source when the file is missing (from the shader's Properties block).
DEFAULTS = {
    "base": (255, 255, 255, 255), "normal": (128, 128, 255, 255), "param": (255, 255, 255, 255),
    "style": (255, 255, 255, 255), "ramp": (255, 255, 255, 255), "specRamp": (255, 255, 255, 255),
    "lut": (255, 255, 255, 255), "matCap": (0, 0, 0, 255), "mask": (255, 255, 255, 255),
    "faceSdf": (0, 0, 0, 255), "highlight": (0, 0, 0, 255), "hairSpec": (0, 0, 0, 255),
    "emission": (0, 0, 0, 255), "flow": (0, 0, 0, 255),
    # Unity default for the ILM map is black, so the alpha (material type) is 0.
    "ilm": (0, 0, 0, 0),
}
PROFILES = ("endfield", "nahida")


def load_source(src_dir, name, size):
    """RGBA float array (H, W, 4) in 0..255 at `size` (w, h). Missing files use the default colour."""
    path = os.path.join(src_dir, name + ".png") if src_dir else None
    if path and os.path.isfile(path):
        im = Image.open(path).convert("RGBA")
        if im.size != size:
            im = im.resize(size, Image.BILINEAR)
        return np.asarray(im, dtype=np.float32)
    col = np.array(DEFAULTS[name], dtype=np.float32)
    return np.broadcast_to(col, (size[1], size[0], 4)).copy()


def pack_channels(channels):
    """channels: list of 4 (array, channel index) pairs -> uint8 RGBA image array."""
    out = np.stack([arr[..., ch] for arr, ch in channels], axis=-1)
    return np.clip(out, 0, 255).astype(np.uint8)


def build_atlas(src_dir):
    """The 256x256 lookup atlas. Each region is resized from its source; the rest is black."""
    atlas = np.zeros((ATLAS, ATLAS, 4), dtype=np.uint8)
    atlas[..., 3] = 255
    for key, src in (("ramp", "ramp"), ("spec", "specRamp"), ("lut", "lut"), ("matcap", "matCap")):
        x, y, w, h = REGIONS[key]
        region = load_source(src_dir, src, (w, h))
        atlas[y:y + h, x:x + w, :3] = np.clip(region[..., :3], 0, 255).astype(np.uint8)
    return atlas


def pack(src_dir, out_dir, size=None, profile="endfield"):
    assert profile in PROFILES, profile
    os.makedirs(out_dir, exist_ok=True)
    # Per-pixel slots share one size: the base texture's, unless --size is given.
    if size is None:
        base_path = os.path.join(src_dir, "base.png") if src_dir else ""
        size = Image.open(base_path).size if base_path and os.path.isfile(base_path) else (256, 256)
    w, h = size
    S = lambda name: load_source(src_dir, name, (w, h))  # noqa: E731

    base = S("base")
    normal = S("normal")
    style = S("style")
    mask = S("mask")
    flow = S("flow")
    highlight = S("highlight")
    hair = S("hairSpec")
    # Fourth channel of normalStyle: style.r (Endfield) or the ILM alpha (Nahida).
    fourth = S("ilm") if profile == "nahida" else style
    if profile == "nahida" and src_dir and os.path.isfile(os.path.join(src_dir, "normal.png")):
        # The NPR normal maps store x in red and y in green (blue is about 0, not z). Rebuild z from the unit length.
        x = normal[..., 0] / 255.0 * 2.0 - 1.0
        y = normal[..., 1] / 255.0 * 2.0 - 1.0
        z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))
        normal = np.stack([(x * 0.5 + 0.5) * 255.0, (y * 0.5 + 0.5) * 255.0, (z * 0.5 + 0.5) * 255.0,
                           np.full_like(x, 255.0)], -1)
    images = {
        "base": pack_channels([(base, 0), (base, 1), (base, 2), (base, 3)]),
        "normalStyle": pack_channels([(normal, 0), (normal, 1), (normal, 2), (fourth, 3 if profile == "nahida" else 0)]),
        "param": pack_channels([(S("param"), 0), (S("param"), 1), (S("param"), 2), (S("param"), 3)]),
        "maskFlow": pack_channels([(mask, 0), (mask, 1), (flow, 0), (flow, 1)]),
        "faceSdf": pack_channels([(S("faceSdf"), 0), (S("faceSdf"), 1), (S("faceSdf"), 2), (S("faceSdf"), 3)]),
        "emission": pack_channels([(S("emission"), 0), (S("emission"), 1), (S("emission"), 2), (S("emission"), 3)]),
        "highlightHair": pack_channels([(highlight, 0), (highlight, 1), (hair, 0), (hair, 1)]),
        "lookup": build_atlas(src_dir),
    }
    for name, arr in images.items():
        Image.fromarray(arr, "RGBA").save(os.path.join(out_dir, name + ".png"))
    return sorted(images)


def write_demo_sources(src_dir):
    """Stand-in sources with visible structure, so the packed output can be checked by eye."""
    os.makedirs(src_dir, exist_ok=True)
    W, H = 256, 256
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    def save(name, arr):
        Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA").save(os.path.join(src_dir, name + ".png"))

    check = (((xx // 16) + (yy // 16)) % 2).astype(np.float32) * 0.15 + 0.85
    base = np.stack([225 * check, 180 * check, 160 * check, np.full_like(check, 255)], -1)
    save("base", base)
    u = xx / W
    normal = np.stack([128 + 40 * np.sin(u * 6.2831853 * 4), np.full_like(u, 128),
                       np.full_like(u, 255), np.full_like(u, 255)], -1)
    save("normal", normal)
    t = np.tile(np.linspace(0, 1, W, dtype=np.float32), (H, 1))
    ramp = np.stack([110 + 145 * t, 100 + 130 * t, 130 + 120 * t, np.full_like(t, 255)], -1)
    save("ramp", ramp)
    save("specRamp", np.stack([255 * t, 255 * t, 255 * t, np.full_like(t, 255)], -1))
    save("lut", np.full((H, W, 4), 255, np.float32))
    nx = (xx + 0.5) / W * 2 - 1
    ny = (yy + 0.5) / H * 2 - 1
    r = np.minimum(1.0, np.sqrt(nx * nx + ny * ny))
    s = np.sqrt(np.maximum(0.0, 1.0 - r * r))
    save("matCap", np.stack([60 + 170 * s, 60 + 170 * s, 70 + 160 * s, np.full_like(s, 255)], -1))
    save("faceSdf", np.stack([255 * t, 255 * t, 255 * t, np.full_like(t, 255)], -1))
    # ILM alpha: five equal bands across u, so each Nahida material type shows once.
    save("ilm", np.stack([np.full_like(u, 255), np.full_like(u, 255), np.full_like(u, 255), (u * 255)], -1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--size", type=int, default=None, help="edge length of the per-pixel slots")
    ap.add_argument("--demo", metavar="DIR", help="write stand-in sources to DIR/src and pack to DIR/packed")
    ap.add_argument("--profile", choices=PROFILES, default="endfield", help="which shader the set is for")
    args = ap.parse_args()
    if args.demo:
        src = os.path.join(args.demo, "src")
        write_demo_sources(src)
        names = pack(src, os.path.join(args.demo, "packed"), size=(256, 256), profile=args.profile)
    else:
        if not args.src or not args.out:
            ap.error("need <srcDir> <outDir>, or --demo DIR")
        size = (args.size, args.size) if args.size else None
        names = pack(args.src, args.out, size, profile=args.profile)
    print("packed:", ", ".join(names))


if __name__ == "__main__":
    main()
