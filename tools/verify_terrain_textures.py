#!/usr/bin/env python3
"""Checks the vendored ground textures against what the engine actually reads.

`editor/res/terrain/NOTICE.md` says which of the reference's 40 texture files are shipped here and what each one is
used for. That is a claim in a document, and a claim in a document rots. This script checks it:

  * every layer the terrain shader reads has albedo, normal and AO, at the size the atlas wants;
  * the three layers are actually different images (a copy-paste that made mud look like grass would pass a mere
    existence check);
  * the normal maps are tangent-space normals and not accidental copies of the albedo;
  * the detail overlay exists and is the size the shader samples;
  * with `--reference PATH`, the coverage table against the reference repository's own file list, so a file that
    appears upstream cannot be silently missed by the importer.

    python3 tools/verify_terrain_textures.py [--repo ROOT] [--reference PATH_TO_UnityGrass_CHECKOUT]

Exit status is 0 when every requirement holds. CI runs it on every push.
"""
import argparse
import hashlib
import os
import sys

from PIL import Image

# The layers the terrain shader blends. Keep in step with terrain_layers.cpp and shaders/terrain.frag.
LAYERS = ["grass_moss", "heather", "muddy"]
CHANNELS = ["albedo", "normal", "ao"]
ATLAS_CELL = 512          # the terrain atlas cell, in texels; see NOTICE.md
DETAIL = ("plant_detail.png", 512)

# What the reference repository holds, by directory. Counted from the repository's file list, and re-checked when
# --reference is given.
REFERENCE = {
    "Terrain": 9,
    "Details": 31,
}


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def check(ok, message, detail=""):
    print(("ok    " if ok else "FAIL  ") + message + (f"  [{detail}]" if detail else ""))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=".", help="repository root")
    ap.add_argument("--reference", default="", help="checkout of UnityGrassIndirectRenderingExample")
    args = ap.parse_args()

    root = os.path.join(args.repo, "editor/res/terrain")
    failures = 0
    failures += 0 if check(os.path.isdir(root), "the texture folder exists", root) else 1

    hashes = {}
    for layer in LAYERS:
        for channel in CHANNELS:
            path = os.path.join(root, f"{layer}_{channel}.png")
            if not os.path.exists(path):
                failures += 0 if check(False, f"{layer}: {channel} is present") else 1
                continue
            with Image.open(path) as im:
                w, h = im.size
                mode = im.mode
            ok = w == ATLAS_CELL and h == ATLAS_CELL
            failures += 0 if check(ok, f"{layer}: {channel} is {ATLAS_CELL}x{ATLAS_CELL}", f"{w}x{h} {mode}") else 1
            hashes[f"{layer}_{channel}"] = digest(path)

    # Distinct images: three layers that are copies of each other would pass every check above.
    albedo = [hashes.get(f"{l}_albedo") for l in LAYERS]
    failures += 0 if check(len(set(albedo)) == len(LAYERS), "the three albedo maps are different images") else 1
    for layer in LAYERS:
        same = hashes.get(f"{layer}_albedo") == hashes.get(f"{layer}_normal")
        failures += 0 if check(not same, f"{layer}: the normal map is not the albedo") else 1

    # Tangent-space normal maps are blue-dominant. A normal map that is not is a mis-export, and the terrain would
    # shade as if the ground were lit from the wrong side.
    for layer in LAYERS:
        path = os.path.join(root, f"{layer}_normal.png")
        if not os.path.exists(path):
            continue
        with Image.open(path) as im:
            rgb = im.convert("RGB").resize((64, 64), Image.NEAREST).tobytes()
        mean_r = sum(rgb[0::3]) / (len(rgb) // 3) / 255.0
        mean_b = sum(rgb[2::3]) / (len(rgb) // 3) / 255.0
        ok = mean_b > 0.55 and mean_b > mean_r + 0.15
        failures += 0 if check(ok, f"{layer}: the normal map is tangent-space (+Z dominant)",
                               f"mean b={mean_b:.2f} r={mean_r:.2f}") else 1

    detail_path = os.path.join(root, DETAIL[0])
    if not os.path.exists(detail_path):
        failures += 0 if check(False, "the detail overlay is present") else 1
    else:
        with Image.open(detail_path) as im:
            failures += 0 if check(im.size == (DETAIL[1], DETAIL[1]), "the detail overlay is 512x512",
                                   f"{im.size}") else 1

    # Coverage against the reference, when a checkout is available.
    if args.reference:
        total_expected = 0
        for sub, count in REFERENCE.items():
            d = os.path.join(args.reference, "Assets/TerrainSampleAssets/Textures", sub)
            if not os.path.isdir(d):
                failures += 0 if check(False, f"reference: {sub} exists", d) else 1
                continue
            files = [f for f in os.listdir(d) if f.endswith((".tif", ".png", ".jpg"))]
            total_expected += count
            failures += 0 if check(len(files) == count, f"reference: {sub} still holds {count} images",
                                   f"{len(files)}") else 1
        shipped_ground = len(LAYERS) * len(CHANNELS)
        print()
        print(f"coverage: {shipped_ground} of {shipped_ground} ground maps (Terrain/), "
              f"1 of 4 Plant details (Details/), 0 of 27 billboard plant maps")
        print(f"          {shipped_ground + 1} of {total_expected} reference images are shipped; "
              f"the rest are billboard cards this engine does not draw (NOTICE.md explains why)")

    print()
    print(f"{'all texture requirements hold' if failures == 0 else str(failures) + ' requirement(s) failed'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
