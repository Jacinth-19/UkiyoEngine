#!/usr/bin/env python3
"""Fetch and pin the third-party models Afterglow ships.

The four original props (torii, lanterns, platform) are written by tools/lantern_assets.cpp because code should
regenerate code. Everything else in the village is real modelling work by Kenney — trees, rocks, fences, stalls, a
windmill — and Kenney licenses it all CC0, so the honest way to ship it is verbatim, byte-pinned, with the source
recorded here:

    git clone https://github.com/shorepine/kenney /path/to/kenney     # Kenney.nl's packs, CC0
    tools/fetch_afterglow_assets.py --src /path/to/kenney             # copy the pinned set in
    tools/fetch_afterglow_assets.py --src /path/to/kenney --check     # verify the committed bytes still match

Every destination below carries its sha256, so an upstream re-upload cannot drift the game silently: --check fails
the moment a byte changes. Audio sources are pinned the same way by tools/afterglow_audio.py, which also builds
the three wavs from them (the Oculus recordings are CC BY 4.0 and are credited in games/afterglow/CREDITS.md).

The fantasy-town pieces share one palette texture, Textures/colormap.png; their GLBs reference it as an external
image, which is why the packer reads glTF JSON for image uris (src/pack/pack.cpp) instead of trusting the scene
to name every file.
"""

import argparse
import hashlib
import os
import sys

# (destination under assets/, sha256, bytes)
FILES = [
    # Kenney Nature Kit — flat-colour pieces, no textures, every mesh self-contained.
    ("tree_oak.glb", "d7fd8773674928c50c11b66d12c636d49bdcc15a8b1c7fbb98e6f63a3439a3f3", 14644),
    ("tree_pineTallA.glb", "e0a56eb196d8a64ba86c7304d607136e17e6f9ad748dffcf86bd53b18b91b196", 7200),
    ("tree_fat.glb", "84b262c5dda3a91ac6c95f9d8b23a0ebbb1951d678c0e1375d3e32623cc43ee2", 5576),
    ("tree_detailed.glb", "c041daf2f0fb1d49e4325227cbcd58667adbe51e9b55e8c1f0a94b74cc521b3b", 31412),
    ("plant_bushLarge.glb", "10e1d9fbf29d96d1d1a56fb2ce25ae66537505101759229afe01ae22c6c45e02", 6436),
    ("plant_bushSmall.glb", "a9620a7d1fb165ae68d323676f80dbb2a9e3aafb21110d18d18d90b07026b91d", 3212),
    ("rock_largeA.glb", "6dd15390fd96501dcd1454765a17ba61dbbd8d47705dfe5149c8dd92b353ce25", 7552),
    ("rock_largeC.glb", "23aee8b727adeaf260207af3e0615dc69aa766c6d87de1d44cbc8c2711d74dd1", 7004),
    ("rock_smallF.glb", "51099549e8dd6a68db2afe2e5c514a8601ee59e11766d5975c540314f9b7deb8", 5700),
    ("fence_simple.glb", "ecaf6c29532aa9fd305a8ef71df769d60748bd797f2eb1c63bdefc4edb8062a7", 5712),
    ("fence_corner.glb", "f37060de68c5680e518183d44e7ac498e0bfe7b04f3fe71e355d8a4670ef83f1", 8644),
    ("fence_gate.glb", "34d10d5620179b8a211a01eea837caa828c0782422f0b408e1a5838788888436", 27652),
    ("fence_planks.glb", "2db68f60d24443dbeab05dedd92fcc665d0e21bd261c6a668ea9022a732a06a9", 7720),
    ("grass_large.glb", "af49a595624abca7249824eeefb944d4baf974abeeb5b16110d175da3563d6cd", 18504),
    ("flower_redB.glb", "1ed430a4ee2be9e86649e1ce22ec23162814a3f8c2ad133117315db19293fc16", 8080),
    ("flower_yellowA.glb", "8a3b08cd2ca411c21f9c581d5bda651d78b13c50cba45ad8fa0389398ead6d1d", 7120),
    ("log_stack.glb", "90eaae4a01ba1f25c2f0c6d83087e4a7a8fac684f2452615245333968d118d68", 10896),
    ("stump_round.glb", "e9b0f385f0ef98493ef716a055aa51e0b99efb8445279be7bb8bd83329a4c4af", 5644),
    ("campfire_stones.glb", "54869db23243a3f4905bc00bd4cd93b1037b28556ab3ef93e7a893a093791e35", 17344),
    ("campfire_logs.glb", "98d87cdcf9095b94fd348d0946e911dd4ca6c0956bac3318cfd2b0030b86055a", 9284),
    ("path_stone.glb", "06f2eb4f914df4dd419b7fb33e81059bc0b1a5b941a193b7c9f8ffd8851146c5", 12392),
    ("path_stoneCircle.glb", "d3f2044acdaeb7100d82aca702207e9c2f198aa0f03b9aef79a714e3a4a563f5", 13100),
    ("crops_bambooStageB.glb", "238c4cbeedf747dea96d579cf63301d35bb8709310f0405596c8f988dcc366e6", 40144),
    ("pot_large.glb", "57e60214bc6ab1dc3764c9ffe80091ff45f2bfd49fbc313c1b68dcaab1537aef", 6168),
    # Kenney Fantasy Town Kit — textured against the shared palette below.
    ("tree.glb", "e8d81b97297de62699b92559cee7b247da84b1815df2515e42fd5a318a30b8d7", 18188),
    ("tree-high.glb", "2dbb718a81d9c3697c069495f58840f6c2002c8cdd13388be560c2199f6d03e7", 20700),
    ("stall-red.glb", "aa20a630f417032c46aaefbc779fd2b3355b451bf05d31acc36fd664146866b1", 24304),
    ("stall-green.glb", "2da2f7b31cde5d943d7c08c457bbb39797ef0a607921fde0b8455f338ce40205", 27120),
    ("lantern.glb", "f270637e3083d6e84a30626ee05df2b09597a642985a7f5c3e4ab5bb1c7a8d55", 14984),
    ("cart.glb", "63503cd0a7398310e01aabc8e3663f711a7517394d23d179d0695157fee99d40", 52920),
    ("wheel.glb", "c049e479d0fa31d7cae7a3f4fe239172e7117789fdd10be4be5bddde6239d59f", 17184),
    ("windmill.glb", "72562df164f5fd36670521462da8f4161cf2c4c1c889799443f0fb44532ad5db", 71160),
    ("hedge.glb", "12001b57dbd3ccaddd217c49e541620137d3e835d873ce0bcedc0e01180a08a9", 6524),
    ("Textures/colormap.png", "4aac939dc33195e35caf8c382ee3cb170054da763577f22d3c22692ec6afccdf", 11143),
]

SOURCE_PATHS = {
    "Textures/colormap.png": "3d/fantasy-town/Textures/colormap.png",
}


def source_for(dest):
    if dest in SOURCE_PATHS:
        return SOURCE_PATHS[dest]
    if dest.startswith("Textures/"):
        return f"3d/fantasy-town/{dest}"
    # nature kit and fantasy-town props share the assets folder; the kits decide by name:
    # fantasy-town pieces are the ones listed in SOURCE_TOWN.
    return f"3d/{KIT[dest]}/{dest}"


# Kits by file, so the mapping stays explicit rather than guessed from the folder.
KIT = {name: "nature" for name, _sha, _n in FILES}
for _name in ("tree.glb", "tree-high.glb", "stall-red.glb", "stall-green.glb", "lantern.glb", "cart.glb",
              "wheel.glb", "windmill.glb", "hedge.glb", "Textures/colormap.png"):
    KIT[_name] = "fantasy-town"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", help="checkout of https://github.com/shorepine/kenney")
    ap.add_argument("--out", default="games/afterglow/assets", help="destination directory")
    ap.add_argument("--check", action="store_true", help="verify committed files against the pinned hashes")
    args = ap.parse_args()
    if not args.src and not args.check:
        ap.error("--src is required unless --check is given")

    status = 0
    total = 0
    for dest, want, size in FILES:
        path = os.path.join(args.out, dest)
        if args.check:
            if not os.path.exists(path):
                print(f"FAIL {dest}: missing")
                status = 1
                continue
            got = sha256(path)
            if got != want:
                print(f"FAIL {dest}: sha256 {got}")
                status = 1
                continue
            n = os.path.getsize(path)
            if n != size:
                print(f"FAIL {dest}: {n} bytes, pinned {size}")
                status = 1
                continue
            total += n
            continue
        src = os.path.join(args.src, source_for(dest))
        if not os.path.exists(src):
            print(f"FAIL {dest}: no source {src}")
            status = 1
            continue
        data = open(src, "rb").read()
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            print(f"FAIL {dest}: upstream sha256 {got}, pinned {want} — re-pin deliberately if this is intended")
            status = 1
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(data)
        total += len(data)
    if args.check and status == 0:
        print(f"fetch_afterglow_assets: {len(FILES)} files, {total} bytes match the pins")
    elif status == 0:
        print(f"fetch_afterglow_assets: wrote {len(FILES)} files, {total} bytes")
    return status


if __name__ == "__main__":
    sys.exit(main())
