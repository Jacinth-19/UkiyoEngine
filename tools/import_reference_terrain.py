#!/usr/bin/env python3
"""Imports the terrain textures of the MagicGrass reference project into the engine.

The reference (github.com/Jacinth-19/UnityGrassIndirectRenderingExample, MIT, (c) 2024 EricHu) ships HDRP terrain
sample textures as 2048 x 2048 uncompressed TIFFs. They are the ground truth for how grass, heather and mud should
look, so the engine vendors them instead of asking every project to invent a meadow.

What this script does, for each layer:
  * BaseColor.tif  -> <layer>_albedo.png, 512 x 512, sRGB kept as authored
  * Normal.tif     -> <layer>_normal.png, 512 x 512, RGB kept (tangent space, +Y up)
  * MaskMap.tif    -> <layer>_ao.png,     512 x 512, the green channel (HDRP's ambient occlusion)
and from Textures/Details/Plant_BaseColor.tif:
  * plant_detail.png, 512 x 512, used as a near-field detail overlay by shaders/terrain.frag

512 is deliberate: the terrain atlas cell is 512 (kTerrainAtlasCell), and the reference's own files are 2048, so
the engine downsamples once, at import, to exactly the size the shader wants; the files also stay small enough to
live in the repository.

Run from the repository root:
    python3 tools/import_reference_terrain.py [path/to/UnityGrass]

The default source path is /tmp/UnityGrass, where the review checkout lived. Output goes to editor/res/terrain/.
"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "editor", "res", "terrain")
SIZE = 512

# Vendored layer -> (file stem in the reference, engine name)
LAYERS = [
    ("Grass_Moss", "grass_moss"),
    ("Heather", "heather"),
    ("Muddy", "muddy"),
]


def save(img, name):
    path = os.path.join(OUT, name)
    img.save(path, optimize=True)
    print("%-28s %4d x %-4d %7.1f kB" % (name, img.width, img.height, os.path.getsize(path) / 1024.0))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/UnityGrass"
    tex = os.path.join(src, "Assets", "TerrainSampleAssets", "Textures", "Terrain")
    if not os.path.isdir(tex):
        sys.exit("the reference textures are not at " + tex)
    os.makedirs(OUT, exist_ok=True)

    for stem, name in LAYERS:
        albedo = Image.open(os.path.join(tex, stem + "_BaseColor.tif")).convert("RGB")
        save(albedo.resize((SIZE, SIZE), Image.LANCZOS), name + "_albedo.png")

        normal = Image.open(os.path.join(tex, stem + "_Normal.tif")).convert("RGB")
        save(normal.resize((SIZE, SIZE), Image.LANCZOS), name + "_normal.png")

        # HDRP mask map: R metallic, G ambient occlusion, B detail mask. The engine already derives roughness from
        # the layer's constant and carries a normal map, so the one channel worth keeping is the occlusion.
        mask = Image.open(os.path.join(tex, stem + "_MaskMap.tif")).convert("RGB")
        ao = mask.split()[1].resize((SIZE, SIZE), Image.LANCZOS).convert("L")
        save(ao, name + "_ao.png")

    detail = Image.open(os.path.join(src, "Assets", "TerrainSampleAssets", "Textures", "Details",
                                     "Plant_BaseColor.tif")).convert("RGB")
    save(detail.resize((SIZE, SIZE), Image.LANCZOS), "plant_detail.png")


if __name__ == "__main__":
    main()
