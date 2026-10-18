#!/usr/bin/env python3
"""Packs the Nahida body, hair and face sets (512 px) from the NPR repo's texture folders.

Needs the NPR repo's Assets/charactors/nahida/tex and universals folders. Personal test use only: the textures
are not in this repo. Writes body/, hair/ and face/ under the output directory. The plain set is not touched.

Usage:  build_nahida_sets.py <nahida/tex dir> <nahida/universals dir> <outDir>
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pack_endfield import pack  # noqa: E402

PREFIX = "Avatar_Loli_Catalyst_Nahida_Tex_"


def stage(links, tmp):
    """Symlinks each source file under the property name the packer reads (base.png, ilm.png, ...)."""
    for name, path in links.items():
        if not os.path.isfile(path):
            raise SystemExit(f"missing source: {path}")
        os.symlink(path, os.path.join(tmp, name + ".png"))
    return tmp


def ramp_strip(src, dst):
    """Face shadow ramp: the face shader reads column U = 0.003 at V = row / 10 - 0.05 (day) and + 0.5 (night).
    A 256x20 strip takes the source's left columns at each of the 20 rows' centres, so the ramp atlas row k
    sits at V = (k + 0.5) / 20, as the shader expects."""
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(src).convert("RGB")).astype(np.float32)
    h = a.shape[0]
    col = a[:, 0:2, :].mean(axis=1)
    rows = [col[min(h - 1, int((k + 0.5) / 20.0 * h))] for k in range(20)]
    strip = np.repeat(np.stack(rows)[:, None, :], 256, axis=1)
    Image.fromarray(np.clip(strip, 0, 255).astype(np.uint8), "RGB").save(dst)
    return dst


def rgb_copy(src, dst):
    """Opaque copy of an ILM map. The body and hair param slots read ILM R (metal), G (AO / shadow gate) and
    B (highlight mask); alpha is forced to 1 so the spec-ramp V the packer puts in param.a stays at its default."""
    from PIL import Image
    Image.open(src).convert("RGB").convert("RGBA").save(dst)
    return dst


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    tex, uni, out = sys.argv[1:4]
    sets = {
        "body": {
            "base": os.path.join(tex, "体1.png"),
            "normal": os.path.join(tex, PREFIX + "Body_Normalmap.png"),
            "ilm": os.path.join(tex, PREFIX + "Body_Lightmap.png"),
            "ramp": os.path.join(tex, PREFIX + "Body_Shadow_Ramp.png"),
            "specRamp": os.path.join(tex, PREFIX + "Body_Specular_Ramp.png"),
        },
        "hair": {
            "base": os.path.join(tex, "髮1.png"),
            "normal": os.path.join(tex, PREFIX + "Hair_Normalmap.png"),
            "ilm": os.path.join(tex, PREFIX + "Hair_Lightmap.png"),
            "ramp": os.path.join(tex, PREFIX + "Hair_Shadow_Ramp.png"),
        },
        "face": {
            "base": os.path.join(tex, "颜.png"),
            "faceSdf": os.path.join(uni, "Avatar_Loli_Tex_FaceLightmap.png"),
            "ramp": None,   # built below as a strip (see ramp_strip)
            "matCap": os.path.join(tex, "toon_defo.bmp"),
        },
    }
    for name, links in sets.items():
        tmp = tempfile.mkdtemp(prefix="nahida_" + name + "_")
        try:
            if "ilm" in links:
                links["param"] = rgb_copy(links["ilm"], os.path.join(tmp, "ilm_as_param_src.png"))
            if links.get("ramp") is None:
                links["ramp"] = ramp_strip(os.path.join(uni, "Avatar_Tex_Face_Shadow.png"),
                                           os.path.join(tmp, "face_ramp_strip.png"))
            src = stage(links, tmp)
            names = pack(src, os.path.join(out, name), size=(512, 512), profile="nahida")
            print(name, "->", os.path.join(out, name), ":", ", ".join(names))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
