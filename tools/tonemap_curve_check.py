"""Transfer-curve comparison of two tonemap operators, as a scene-independent check.

Ports the per-channel curves from shaders/tonemap.frag exactly:
  acesFilm (the Narkowicz ACES fit), lottesCurve, then linearToSRGB.
Reports the 8-bit output difference over the full HDR input range (0..8) and over the
mid-tone band that most scene pixels fall in.
"""
import numpy as np

def aces_fit(x):
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0)

def lottes(x):
    a, d, hdrMax, midIn, midOut = 1.6, 0.977, 8.0, 0.18, 0.267
    ad = a * d
    b = (-midIn ** a + hdrMax ** a * midOut) / ((hdrMax ** ad - midIn ** ad) * midOut)
    c = (hdrMax ** ad * midIn ** a - hdrMax ** a * midIn ** ad * midOut) / ((hdrMax ** ad - midIn ** ad) * midOut)
    return x ** a / (x ** ad * b + c)

def srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)

def codes(curve, x):
    return np.round(np.clip(srgb(np.clip(curve(x), 0.0, 1.0)), 0.0, 1.0) * 255.0)

def main():
    x = np.linspace(0.0, 8.0, 8001)
    a = codes(aces_fit, x)
    l = codes(lottes, x)
    diff = np.abs(a - l)
    print(f"Lottes check: lottes(0.18)={lottes(np.array([0.18]))[0]:.4f} (design 0.267), lottes(8)={lottes(np.array([8.0]))[0]:.4f} (design 1.0)")
    print(f"ACES fit(0.18)={aces_fit(np.array([0.18]))[0]:.4f}")
    print(f"full range 0..8:      max {diff.max():.0f} levels at x={x[diff.argmax()]:.2f}, mean {diff.mean():.2f}")
    band = (x >= 0.02) & (x <= 1.0)
    print(f"mid-tone band 0.02..1: max {diff[band].max():.0f} levels, mean {diff[band].mean():.2f}")
    hi = x >= 2.0
    print(f"highlights 2..8:      max {diff[hi].max():.0f} levels, mean {diff[hi].mean():.2f}")
    for v in (0.05, 0.18, 0.5, 1.0, 2.0, 4.0):
        i = np.argmin(np.abs(x - v))
        print(f"  x={v:4.2f}: ACES {a[i]:5.0f}  Lottes {l[i]:5.0f}")

if __name__ == "__main__":
    main()
