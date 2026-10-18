# Tonemapping operators: what the engine ships and why

Source: the operator catalogue supplied by the user (60+ entries, grouped in six sections),
plus the recommended set of twelve. Attributions in that catalogue were not checked against
primary sources; treat author and product names as the catalogue's claims.

## Shipped (`shaders/tonemap.frag`, selectable in Project Settings > Graphics > Tonemap)

| Index | Name                     | Catalogue # | Notes |
|-------|--------------------------|-------------|-------|
| 0     | ACES fit                 | 17          | Common real-time ACES approximation. |
| 1     | PBR Neutral              | 36          | Khronos. Keeps base colour; highlights desaturate toward white. |
| 2     | AgX                      | 31          | Polynomial approximation with inset/outset matrices. Pale, low saturation. |
| 3     | Custom Anime             | 58/59       | Hue-preserving shoulder (knee 0.62) with a small toe. **Default.** Not yet tuned: at default settings it is close to Linear and PBR Neutral. |
| 4     | Linear clamp (debug)     | 1, 2        | No curve, clamp to [0, 1]. |
| 5     | Reinhard extended        | 4, 5        | Applied to luminance, white point 4. Hue and ratios kept. |
| 6     | Hejl-Burgess-Dawson      | 15, 16      | Output is display-encoded, so it is linearised with pow 2.2 before the sRGB step. |
| 7     | Hable (Uncharted 2)      | 13, 14      | Exposure bias 2.0, white point 11.2. |
| 8     | Lottes                   | 26          | Per channel. Constants give 0.18 -> 0.267 and 8.0 -> 1.0 (checked numerically). |
| 9     | Uchimura (GT)            | 27          | Per channel. Default parameters (P 1, a 1, m 0.22, l 0.4, c 1.33, b 0). |

Stored scenes keep their indices: 0 to 3 are unchanged, and the new operators sit at 4 to 9.
The CLI accepts `--tonemap 0-9`. Look (cel, painterly, cinematic) is packed separately as
`look = floor(w / 10)`.

## Recommended but not shipped

- **Extended Reinhard in its own right**: shipped as index 5, with the luminance variant.
- **Custom HDR output transform**: needs an HDR display path (PQ or HLG output, peak
  luminance, a separate encode). The engine has no HDR output yet, so this is not built.
- **ACES RRT + ODT, ACES 1.0 SDR, ACES 1000/2000/4000-nit**: the full ACES transform
  chain and HDR targets. Not built; only the fitted approximation (index 0) ships.
- **AgX variants (Punchy, Golden, Saturated)**: look presets on top of AgX. Not built.

## Not tonemappers (excluded on purpose)

- **PQ (ST 2084) and HLG**: transfer functions for HDR output, not scene tonemappers.
- **OpenColorIO display transforms**: a configuration system, not one curve.
- **BT.2390, BT.2446**: HDR-to-SDR conversion workflows. Relevant only with an HDR output path.
- **Local tone mapping**: spatially varying. A different class of operator; not built.
- **Blender Filmic / Blender AgX**: Blender's colour-management configuration around the
  same family as the entries above.

## Not built, and the reason

- **Unity Neutral, Watch Dogs Filmic, Source Engine Filmic, Gran Turismo-style Uchimura,
  Jim Hejl variants, Unreal Engine filmic and UE3/UDK**: the catalogue gives no formula, and
  I did not verify them. Uchimura (GT) is built from the published parametric form.
- **Custom Rational / S-curve, Custom Anime HDR Curve with artist toe and shoulder**: the
  Custom Anime knee and toe are fixed constants. Exposing them is the next step for item 59.
- **Film-emulation LUT**: covered by the grading LUT (`assets/LUTs/`, 256x16 strip).
- **Maximum-chroma, RGB ratio-preserving, gamut-aware, per-channel and luminance-only
  variants**: broad families with no single canonical formula. Only the hue-preserving
  behaviour is represented, through Custom Anime, PBR Neutral and Reinhard extended.

## Verification

- Self-test (`ukiyo_editor --selftest`): 56 passed, 0 failed. Includes all ten operators
  rendering a plausible mean level, and no pair of operators within 0.05 levels of each other.
- Headless renders at 640x640: `renders/tonemap_operators_10.png`. Mean levels: ACES 200.7,
  Neutral 182.2, AgX 212.8, Anime 188.7, Linear 189.8, Reinhard 156.2, Hejl 182.5,
  Hable 146.4, Lottes 201.5, Uchimura 187.4.
- Hejl and Uchimura run here in their documented forms; they were not compared against a
  reference implementation frame by frame.

## ACES fit and Lottes: measured duplicate (resolved)

The comparison dialog's thumbnails for operator 0 (ACES fit) and operator 8 (Lottes) differ
by only 0.80 levels on average per pixel, below the 1-level distinctness check.

Cause, measured with `tools/tonemap_curve_check.py` (exact ports of the shader curves):
- ACES fit is the Narkowicz rational fit, per channel. Lottes is the 2016 curve with its published
  constants (a 1.6, d 0.977, HDR max 8, mid grey 0.18 to 0.267), also per channel.
- Both map mid grey 0.18 to 141/255 sRGB.
- Over the full HDR range 0 to 8 their 8-bit outputs differ by at most 2 levels (mean 0.77).

Decision: both operators stay, because the brief asks for Lottes. They are a measured duplicate,
not a bug. The self-test checks the curve match directly (within 2 levels) and exempts only this
pair from the thumbnail distinctness check. Every other pair must differ by at least 1 level.
If Lottes should be visibly different, the choice is a different published variant, which is a
product decision, not a test change.
