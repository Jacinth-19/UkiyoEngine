# Anime post, stage 3: TAA (status)

Implemented (AA mode 4, "TAA (FXAA on reset frames)"):
- Camera jitter: Halton(2,3), 8 samples, applied to the projection in the frame UBO.
- Resolve pass (`shaders/taa.frag`): reprojects the previous resolved frame using scene depth
  (binding 4) and the previous unjittered viewProj; clamps history to the 3x3 YCoCg neighbourhood;
  blends 10% current, raised to 50% where the clamp moved the history far (colour rejection).
- Ping-pong history targets (RGBA16F). Bloom and tonemap read the resolved target.
- Fallback: reset frames (first frame, after a resize, after switching the mode on) take the
  current frame and run FXAA. Off-screen reprojection takes the current sample.

Measured (headless, 640x640, tonemap 3):
- Static camera, 16 frames: converged TAA differs from no-AA by 2.45 levels mean, 1.0% of pixels
  by more than 40 levels. Visually the edges are smoother with no visible ghosting.
- Moving camera (orbit 0.12 turns, 16 frames): TAA vs no-AA 2.6 levels mean, 1.0% above 40.
  No excess ghosting over the static case.
- OpenGL and Vulkan TAA outputs match (mean difference 0.01).
- Full-frame error against the 2x SSAA reference is not a usable quality gate. TAA blurs
  edges by design, and the reference is sharp, so the metric rises even when the result looks
  right. Edge quality was judged visually.

Not done:
- Per-object motion vectors. Camera motion only. A moving character (skinned mesh) has no
  velocity, so it relies on the clamp and colour rejection and may ghost. Needs previous-frame
  joint matrices and a velocity output from the MToon shaders.
- Depth-based disocclusion rejection. The current rejection uses colour only.
- No self-test check for TAA yet.
