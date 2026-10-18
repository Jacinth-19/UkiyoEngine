"""Best sub-pixel alignment of TAA variants against the 2x SSAA reference.

For each render, search shifts (dx, dy) in [-0.5, 0.5] px (bilinear) and report the shift that
minimises the mean absolute luma error over the interior (32 px margin removed).
Inputs are the headless renders in /tmp/taa_exp (640x640, 16 frames).
"""
import sys
import numpy as np
from PIL import Image

def load(p):
    return np.asarray(Image.open(p).convert("L")).astype(np.float64)

def shift_bilinear(img, dx, dy):
    h, w = img.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float64)
    sx = np.clip(xs + dx, 0, w - 1)
    sy = np.clip(ys + dy, 0, h - 1)
    x0 = np.floor(sx).astype(int); y0 = np.floor(sy).astype(int)
    x1 = np.minimum(x0 + 1, w - 1); y1 = np.minimum(y0 + 1, h - 1)
    fx = sx - x0; fy = sy - y0
    return ((1 - fy) * ((1 - fx) * img[y0, x0] + fx * img[y0, x1])
            + fy * ((1 - fx) * img[y1, x0] + fx * img[y1, x1]))

def best_shift(img, ref, m=32):
    best = (1e9, 0.0, 0.0)
    grid = np.arange(-0.5, 0.5001, 0.05)
    for dx in grid:
        for dy in grid:
            s = shift_bilinear(img, dx, dy)
            err = np.abs(s - ref)[m:-m, m:-m].mean()
            if err < best[0]:
                best = (err, dx, dy)
    return best

def main():
    ref = load("/tmp/taa_exp/ssaa2.png")
    cases = [("no AA", "/tmp/taa_exp/none.png"),
             ("TAA jitter +1 (current)", "/tmp/taa_exp/taa_j1.png"),
             ("TAA jitter -1 (sign flipped)", "/tmp/taa_exp/taa_j-1.png"),
             ("TAA jitter 0 (no jitter)", "/tmp/taa_exp/taa_j0.png")]
    lines = []
    for name, path in cases:
        img = load(path)
        err0 = np.abs(img - ref)[32:-32, 32:-32].mean()
        err, dx, dy = best_shift(img, ref)
        lines.append(f"{name:30s} error at 0 shift {err0:6.2f}   best shift ({dx:+.2f}, {dy:+.2f}) px   error {err:6.2f}")
    text = "\n".join(lines)
    print(text)
    with open("/home/user/ukiyo/research/taa_offset_experiment_results.txt", "w") as f:
        f.write("TAA offset experiment: best shift vs 2x SSAA (640x640, 16 frames, luma)\n" + text + "\n")

if __name__ == "__main__":
    main()
