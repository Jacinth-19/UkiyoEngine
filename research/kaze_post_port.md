# Kaze UnityGenshinPostProcessing: port notes

Source: https://github.com/kaze-mio/UnityGenshinPostProcessing (MIT)

## Bloom design (rewritten to Kaze's)

Kaze's design, as ported:

1. **Prefilter.** A 13-tap downsample into level 0 (`bloom_down.frag`), then the threshold.
   Kaze uses one bilinear sample here, so the filter differs slightly, but the order matches: threshold before blur.
2. **Blur per level** (`bloom_blur.frag`). A 9-tap separable Gaussian, with Kaze's kernel. Level 0 blurs in place. Level i reads level i-1 at twice the spacing, as Kaze's H2x pass does. Each pass uses a vertical step of 1.
   Blur radius = `bloomBlurRadius * bloomScale * (frame height / 1080)`.
3. **Combine** (`bloom_combine.frag`). The weighted sum of the four blurred levels, with `bloomWeights` (Kaze default 0.25 each), then the glow tint. The intensity (`bloomStrength`) is applied in tonemap, as in Kaze's ColorGrading.

The old dual-filter upsample (`bloom_up.frag`) is no longer used. The file is still in `shaders/`.

## Ported

| Kaze feature | Engine field | Where |
| --- | --- | --- |
| Bloom colour | `bloomTint` | `bloom_combine.frag` |
| Threshold "Color" (per channel) | `bloomThresholdMode = 1` | `bloom_down.frag` |
| Threshold "Brightness" (max channel) | `bloomThresholdMode = 2` | `bloom_down.frag` |
| Per-level weights | `bloomWeights` (vec4) | `bloom_combine.frag` |
| Blur radius | `bloomBlurRadius` | `bloom_blur.frag` |
| Downsample scale (0.1..1) | `bloomScale` (default 0.5) | `renderer.cpp`, rebuilds bloom targets |
| Intensity | `bloomStrength` | `tonemap.frag` |
| Genshin rational tonemap | tonemap operator 10 | `tonemap.frag` |
| Contrast in LogC around ACEScc mid grey | `contrastLogC` | `tonemap.frag` |
| Saturation as a linear-light lerp | `saturationLinear` | `tonemap.frag` |

Tonemap mode bits: `op + 16*look + 64*logC + 128*linearSat`.

## Preset: Kaze Unity match

`applyKazeUnityPost()` in `renderer.h` applies the values from the Unity screenshot (Bloom Volume and Color Grading Volume). It is available as `--post-preset kaze-unity` on the CLI and as the "Apply Kaze Unity match" button on the Bloom page of the editor.

The preset sets: Color mode, threshold 0.7, intensity 1.5, weights 0.1/0.2/0.3/0.4, white tint, blur radius 2, downsample scale 0.5, Genshin tonemap, exposure 1.05, contrast and saturation neutral (linear saturation on).

## Behaviour change

The default bloom is now Kaze's design, so **every scene's default glow changes**. This was approved. The default values for the other settings are unchanged.

## Not ported

- **Unity Volume components.** The settings stay in `RenderSettings`, which already reaches the Inspector, the scene file and the CLI.

## Verification

- Shader test 60/0, GPU test 11/0, scene test 23/0. The bloom checks cover the tint, the threshold modes, the bloom scale (including rebuild and reproduce), the per-level weights, the blur radius, the preset, and the tonemap and grade options.
- `ukiyo` app builds. Headless renders with `--post-preset kaze-unity` look correct.
- **Not verified:** the editor. Its new Bloom-page rows, the preset button, and the save and load code have not been compiled, because Qt6 isn't installed here.
