"""Synthetic gradient banding test for the tonemap dither.

Reproduces the shader's dither exactly on the CPU (shaders/tonemap.frag):
    ign(p) = fract(52.9829189 * fract(dot(p, (0.06711056, 0.00583715))))
    s += (ign(gl_FragCoord.xy) - 0.5) / 255.0      # then 8-bit quantise
This is a CPU model of the shader arithmetic, not a GPU render.
Writes research/dither_synthetic_results.txt and renders/dither_synthetic_gradient.png.
"""
import numpy as np
from PIL import Image

W, H = 1024, 256

def ign(x, y):
    t = np.mod(x * 0.06711056 + y * 0.00583715, 1.0)
    return np.mod(52.9829189 * t, 1.0)

def gradient(w, h):
    # Smooth horizontal ramp, dark to mid grey: a classic banding stress case.
    u = np.linspace(0.0, 1.0, w)[None, :].repeat(h, axis=0)
    return 0.05 + 0.35 * u ** 1.6          # display-space value in 0..1

def quantise(v):
    return np.clip(np.round(v * 255.0), 0, 255)

def flat_run_lengths(row):
    runs, cur = [], 1
    for a, b in zip(row[:-1], row[1:]):
        if a == b:
            cur += 1
        else:
            runs.append(cur); cur = 1
    runs.append(cur)
    return np.array(runs)

def load_blue_noise_tile():
    """The 64x64 tile in src/engine/blue_noise_64.h (tools/make_blue_noise.py), as values in 0..1."""
    import re
    text = open("src/engine/blue_noise_64.h", encoding="utf-8").read()
    decl = text.index("kBlueNoise64[")
    body = text[text.index("{", decl) + 1: text.rindex("}")]
    vals = np.array([int(v) for v in re.findall(r"\d+", body)], dtype=np.float64)
    assert vals.size == 64 * 64, vals.size
    return (vals.reshape(64, 64) + 0.5) / 256.0

def main():
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    g = gradient(W, H)
    plain = quantise(g)
    dith = quantise(g + (ign(xs, ys) - 0.5) / 255.0)
    tile = load_blue_noise_tile()
    bn_field = tile[ys.astype(int) % 64, xs.astype(int) % 64]
    bn = quantise(g + (bn_field - 0.5) / 255.0)

    # 1. Flat-run length: long runs are visible bands. Measure on every row.
    def run_stats(q):
        runs = np.concatenate([flat_run_lengths(q[y]) for y in range(H)])
        return runs.mean(), np.percentile(runs, 95), (runs >= 32).mean()
    p_mean, p95, p_long = run_stats(plain)
    d_mean, d95, d_long = run_stats(dith)

    # 2. Local-mean error: average each block of 16x16 and compare with the true gradient.
    def block_err(q):
        b = q.reshape(H // 16, 16, W // 16, 16).mean(axis=(1, 3)) / 255.0
        t = g.reshape(H // 16, 16, W // 16, 16).mean(axis=(1, 3))
        return np.abs(b - t).mean() * 255.0
    p_err, d_err = block_err(plain), block_err(dith)
    b_mean, b95, b_long = run_stats(bn)
    b_err = block_err(bn)

    # 3. Spectrum: fraction of noise energy below 1/8 cycle/px (low frequency = visible grain).
    def low_fraction(noise):
        n = noise - noise.mean()
        P = np.abs(np.fft.fftshift(np.fft.fft2(n))) ** 2
        fy = np.fft.fftshift(np.fft.fftfreq(noise.shape[0]))[:, None]
        fx = np.fft.fftshift(np.fft.fftfreq(noise.shape[1]))[None, :]
        r = np.sqrt(fx ** 2 + fy ** 2)
        return P[r < 0.125].sum() / P.sum()
    rng = np.random.default_rng(1)
    white = rng.random((H, W))
    ign_field = ign(xs, ys)
    f_white = low_fraction(white)
    f_ign = low_fraction(ign_field)
    f_bn = low_fraction(bn_field)

    lines = [
        "Synthetic gradient banding test (CPU model of tonemap.frag dither)",
        f"gradient: {W}x{H}, display value 0.05 .. 0.40, 8-bit",
        f"flat-run length mean:  plain {p_mean:.2f} px   IGN {d_mean:.2f} px   blue noise {b_mean:.2f} px",
        f"flat-run length p95:   plain {p95:.1f} px   IGN {d95:.1f} px   blue noise {b95:.1f} px",
        f"runs >= 32 px:         plain {p_long*100:.1f}%   IGN {d_long*100:.1f}%   blue noise {b_long*100:.1f}%",
        f"16x16 block mean error vs true gradient: plain {p_err:.3f}   IGN {d_err:.3f}   blue noise {b_err:.3f} levels",
        f"low-frequency energy (<1/8 cyc/px): white noise {f_white*100:.1f}%   IGN {f_ign*100:.1f}%   blue noise {f_bn*100:.1f}%",
        "blue noise: void-and-cluster 64x64 tile, true blue noise (tools/make_blue_noise.py).",
    ]
    text = "\n".join(lines)
    print(text)
    with open("research/dither_synthetic_results.txt", "w") as f:
        f.write(text + "\n")
    Image.fromarray(dith.astype(np.uint8)).save("renders/dither_synthetic_gradient.png")
    Image.fromarray(bn.astype(np.uint8)).save("renders/dither_blue_noise_gradient.png")

if __name__ == "__main__":
    main()
