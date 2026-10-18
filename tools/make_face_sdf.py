#!/usr/bin/env python3
"""Face shadow SDF atlas from nine shadow masks.

Input: nine binary-ish PNG masks of the face UV layout, one per light angle, from 0 degrees (light
straight in front of the face) to 180 degrees (light from behind), in 22.5 degree steps. White is lit.
Output: one greyscale PNG, a 3x3 atlas of signed distance fields. Tile k (row-major) holds mask k.

Encoding (the same as the SDFGeneratorUE convention, so the two tools agree):
    inside the lit region  -> above 128, brighter farther inside
    on the boundary        -> 128
    in shadow              -> below 128, darker farther out
    value = round(128 + clamp(signedDistancePixels / range, -1, 1) * 127), range default 64 px

The shader samples two neighbouring tiles by light angle and blends them, then compares with 128.

Usage:
    make_face_sdf.py mask0.png ... mask8.png -o face_sdf.png [--range 64]
    make_face_sdf.py --demo DIR          write nine synthetic masks (a shadow edge that sweeps across the face)
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt

ANGLES = 9


def signed_distance(lit: np.ndarray) -> np.ndarray:
    """Signed distance in pixels, positive inside the lit region."""
    inside = distance_transform_edt(lit)          # distance from each lit pixel to the nearest shadow pixel
    outside = distance_transform_edt(~lit)        # distance from each shadow pixel to the nearest lit pixel
    return inside - outside


def encode(sdf: np.ndarray, rng: float) -> np.ndarray:
    v = np.clip(sdf / rng, -1.0, 1.0)
    return np.round(128.0 + v * 127.0).astype(np.uint8)


def build_atlas(masks, rng: float) -> np.ndarray:
    if len(masks) != ANGLES:
        raise SystemExit(f"need exactly {ANGLES} masks, got {len(masks)}")
    h, w = masks[0].shape
    for m in masks:
        if m.shape != (h, w):
            raise SystemExit("all masks must have the same size")
    atlas = np.zeros((3 * h, 3 * w), dtype=np.uint8)
    for k, m in enumerate(masks):
        r, c = divmod(k, 3)
        atlas[r * h:(r + 1) * h, c * w:(c + 1) * w] = encode(signed_distance(m > 127), rng)
    return atlas


def load_mask(path: str) -> np.ndarray:
    return np.asarray(Image.open(path).convert("L"))


def demo(out_dir: str, size: int = 256) -> None:
    """Nine masks where a straight shadow edge sweeps across the face as the light turns.

    At 0 degrees the whole face is lit; at 180 degrees the whole face is in shadow.
    """
    os.makedirs(out_dir, exist_ok=True)
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    cx = cy = (size - 1) / 2.0
    for k in range(ANGLES):
        t = k / (ANGLES - 1)                                  # 0 .. 1 across the nine angles
        edge = cx + (0.6 - 1.2 * t) * size                    # terminator x: far right (all lit) .. far left (all shadow)
        lit = xs < edge
        Image.fromarray((lit * 255).astype(np.uint8), "L").save(os.path.join(out_dir, f"mask_{k}.png"))
    print(f"wrote {ANGLES} demo masks to {out_dir}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("masks", nargs="*", help="nine mask PNGs, 0 to 180 degrees")
    ap.add_argument("-o", "--out", default="face_sdf.png")
    ap.add_argument("--range", type=float, default=64.0, help="distance (px) that maps to 0 or 255")
    ap.add_argument("--demo", metavar="DIR", help="write nine synthetic masks to DIR and exit")
    args = ap.parse_args(argv)
    if args.demo:
        demo(args.demo)
        return 0
    if len(args.masks) != ANGLES:
        ap.error(f"give exactly {ANGLES} masks (got {len(args.masks)})")
    masks = [load_mask(p) for p in args.masks]
    atlas = build_atlas(masks, args.range)
    Image.fromarray(atlas, "L").save(args.out)
    print(f"wrote {args.out}: {atlas.shape[1]}x{atlas.shape[0]}, 3x3 tiles of {masks[0].shape[1]}x{masks[0].shape[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
