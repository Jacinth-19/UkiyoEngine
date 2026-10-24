# Wireframe view

Flax 1.13's wireframe view: the geometry passes drawn as the edges of the triangles the renderer actually submits. One
flag on the RHI's pipeline description, one setting in `RenderSettings`, and a device feature that is optional in
Vulkan and handled where it is missing.

`View > Wireframe` (`Ctrl+Shift+W`) turns the geometry passes into their edges — so the triangle budget, the LOD levels
and the virtual-geometry cuts are visible instead of guessed at.

![Shaded and wireframe, same triangles](wireframe.png)

Three decisions:

- **Pipeline state, not a shader.** Vulkan wants `VK_POLYGON_MODE_LINE` with the optional `fillModeNonSolid` device
  feature enabled; OpenGL wants `glPolygonMode`. Both are core, and both are one line in the RHI
  (`rhi::PipelineDesc::wireframe`). A device without the feature can still run everything else, so a wireframe pipeline is
  refused rather than the device.
- **Only geometry.** Every fullscreen pass stays filled — a post shader in line mode draws two long triangles across the
  frame — and so does the shadow map, so the picture keeps its contact shadows. The test asserts the split by drawing a
  quad through the HUD pipeline and counting lit pixels: 67.5 % of the panel filled, 3.15 % as edges.
- **Rebuilt once per change.** The mode is `rhi::PipelineDesc` state, so toggling it rebuilds the pipelines once, on the
  frame the mode changes, rather than costing anything on the frames in between.

`wireframe_test` runs on both backends in the `gpu` CI job: it draws one quad filled and again as edges, captures both,
and checks that the edges are a fraction of the filled area and that **every lit pixel lies on the geometry's own
boundary** — the quad's four sides or the diagonal its two triangles share. That last check is what distinguishes a
wireframe from a scatter of debug lines. The CPU figure above is the same idea with hidden lines removed by the scene's
own depth buffer.

The player takes `--wireframe` for a screenshot; the editor keeps it as a view setting, so it is never written into a
scene and does not follow the project.

## What the three cost together

Two of them are about a hundred lines each, and the third is one field in a pipeline description. What took the time
was the part that is not code: deciding that the Inspector's search should be the *browser's* grammar rather than a
fourth variant of it, that a wireframe must not touch the fullscreen passes, and that a loading budget which hides its
overruns is worse than no budget at all.
