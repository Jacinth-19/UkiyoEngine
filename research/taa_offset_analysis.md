# TAA alignment offset: analysis (step 6a)

Measured earlier (see research/anime_post_stage3_taa.md): converged TAA aligns best with the
2x SSAA reference when shifted by about (+0.2, +0.2) px. The cause was not known.

## What the code does

- Jitter: Halton(2,3) over 8 frames, `haltonJitter` in src/engine/renderer.cpp, range -0.5..0.5 px.
- Projection offset: `f.proj[2][0] -= 2*j.x/W`, `f.proj[2][1] -= 2*j.y/H`. In RH perspective the NDC
  x shift equals -P20, so this moves the image by +j.x px in x (and +j.y px in y).
- Reprojection: `prevViewProj_` is the previous *unjittered* viewProj, applied to the current
  pixel's NDC (vUV*2-1) with the current depth.

## What the sequence explains

The converged history averages the 8 jitter positions, so the resolved image is sampled at
pixel centre + mean(jitter). Computed exactly from the sequence (tools/ is not needed, this is
arithmetic on the 8 Halton points):

- mean x jitter = -0.0547 px
- mean y jitter = 0.0000 px

So the sequence alone predicts a shift of (-0.055, 0), with the sign relative to the reference
still to be checked. It does NOT predict (+0.2, +0.2).

## What is still unexplained

The remaining offset of about (+0.25, +0.2) px is not accounted for by the jitter mean. Candidate
causes, none verified yet:
1. Half-pixel convention in the SSAA reference downsample (2x box filter centre vs. pixel centre).
2. A sign or convention error in the NDC-to-pixel mapping used by reprojection (vUV vs gl_FragCoord).
3. The FXAA pass that runs on reset frames shifting the first frames of the history.

## Planned experiment (not run)

Render the static scene at aa 4 (TAA converged) and aa 2 (SSAA 2x) with the jitter sign flipped,
and with the jitter forced to zero. If the best shift follows the jitter sign or goes to zero,
the cause is the projection convention. If it stays at (+0.2, +0.2), the cause is in the reference
downsample or the FXAA reset path.

Status: the offset is partly explained (x -0.055 px from the jitter mean); the rest is open.

## Experiment run (step 6a, measured)

Headless renders, 640x640, 16 frames, luma. Best sub-pixel shift against 2x SSAA
(tools/taa_offset_experiment.py, results in research/taa_offset_experiment_results.txt):

| Variant | Best shift (px) |
|---|---|
| No AA | (-0.05, +0.05) |
| TAA, jitter x1 (shipped) | (-0.30, -0.25) |
| TAA, jitter x-1 (sign flipped) | (+0.30, +0.35) |
| TAA, jitter x0 (no jitter) | (0.00, 0.00) |
| TAA, jitter x0.5 | (-0.10, 0.00) |

What this establishes:
- The reference and no-AA image are aligned. The offset is not in the SSAA reference.
- With no jitter, TAA is aligned. The offset is caused by the jitter.
- Flipping the jitter sign mirrors the shift, so the sign convention of the jitter is what sets its direction.

What it does not establish:
- The size of the offset (about 0.3 px) is about five times what the 8-sample jitter mean predicts
  (0.055 px in x, 0 in y).
- It does not scale linearly with jitter amplitude (x0.5 gives x -0.10, not -0.15; y goes to 0).
  So a single linear bias (the mean jitter) is not the whole story.
- The likely contributors are the history blend and the neighbourhood clamp, which weight recent
  jittered samples unevenly. This is not yet isolated.

Status: cause partly located (the jitter accumulation), mechanism not isolated. The shipped renderer
is unchanged; the temporary experiment knob was removed and the build was rebuilt.


## Update: experiment results (existing file)

`research/taa_offset_experiment_results.txt` (640x640, 16 frames, luma, vs 2x SSAA) records:
- no AA: error 2.71, best shift (-0.05, +0.05) px
- TAA jitter +1: error at 0 shift 3.44, best shift (-0.30, -0.25) px
- TAA jitter -1: error 3.55, best shift (+0.30, +0.35) px
- TAA jitter 0: error 3.08, best shift (0, 0) px

The shift flips sign with the jitter sign. The jitter-0 run has no shift, so the offset comes from the
jitter path, not a fixed bias. The earlier note saying the three renders had not run is out of date.
