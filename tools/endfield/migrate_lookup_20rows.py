#!/usr/bin/env python3
"""One-off migration of packed sets from the 4-row ramp atlas to the 20-row layout (see build_endfield.py REGIONS).

Old: ramp y 0..3, spec 4..19, LUT 20..35, matcap 36..163.   New: ramp 0..19, spec 20..35, LUT 36..51, matcap 52..179.
The old ramp rows must be identical (they are, for the Nahida sets); they are replicated down the 20 new rows.
Optionally sets the ILM alpha (normalStyle.a) to a value, for the Nahida body and hair sets.

Usage:  migrate_lookup_20rows.py <packedDir> [--ilm-alpha N]
"""
import argparse
import os

import numpy as np
from PIL import Image


def relayout(old):
    assert old.shape == (256, 256, 4), old.shape
    ramp = old[0:4]
    assert (ramp == ramp[0:1]).all(), "ramp rows differ; cannot replicate"
    new = np.zeros_like(old)
    new[0:20] = ramp[0:1]              # identical rows, replicated
    new[20:36] = old[4:20]             # spec
    new[36:52] = old[20:36]            # LUT
    new[52:180] = old[36:164]          # matcap (128 wide, rest of row copied as-is)
    new[..., 3] = 255
    return new


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("packed")
    ap.add_argument("--ilm-alpha", type=int, default=None, help="set normalStyle alpha (Nahida ILM)")
    args = ap.parse_args()
    lookup = os.path.join(args.packed, "lookup.png")
    old = np.asarray(Image.open(lookup).convert("RGBA"))
    Image.fromarray(relayout(old), "RGBA").save(lookup)
    if args.ilm_alpha is not None:
        ns = os.path.join(args.packed, "normalStyle.png")
        arr = np.asarray(Image.open(ns).convert("RGBA")).copy()
        arr[..., 3] = args.ilm_alpha
        Image.fromarray(arr, "RGBA").save(ns)
    print("migrated", args.packed)


if __name__ == "__main__":
    main()
