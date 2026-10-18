# Render pipeline backbone

Status: written from `src/engine/renderer.cpp` as it stands. The document describes what the code does now. The limits are listed at the end.

## Frame order (`Renderer::render`)

1. **Shadow atlas.** Cascaded shadow maps (`kShadowAtlas`, three cascades, `fitCascade`). The pass always runs so the atlas is cleared. Only opaque MToon primitives cast shadows. Shader Graph materials and blended materials do not.
2. **Scene pass** (one render pass, `hp`):
   - sky (`pSky_`), then infinite grid (`pGrid_`), both fullscreen;
   - opaque MToon, `pMToon_[0]`;
   - Shader Graph opaque materials, through each material's own pipeline;
   - outlines (`pOutline_`), MToon only;
   - blended MToon, `pMToon_[1]`;
   - Shader Graph blended materials (`graphBlend`), drawn last.
3. **Bloom.** Threshold plus a 13-tap downsample chain, then an additive dual-filter upsample.
4. **Tonemap.** ACES-like curve, saturation and contrast, bloom add, vignette, grain, sRGB encode.
5. **FXAA** (AA modes 1 and 3).

## Material paths

- **MToon** (`pMToon_`): the built-in toon path. Opaque and blended variants, with single and double-sided pipelines.
- **Shader Graph** (`GraphMaterial`, `src/engine/graph_material.*`): the `.ukshader` GLSL is compiled to SPIR-V at runtime with glslang. The material's `.ukmat` values are packed into the material UBO (set 1, binding 15). `blend: alpha` in the shader selects the `graphBlend` list.
- **Material previews** (`src/engine/material_preview.*`, `editor/src/material_thumbnail.*`): the same `GraphMaterial` drawn on a sphere or a model, rendered headlessly. The editor's `PreviewHub` runs these asynchronously for the Project panel.

## Editor side

- `RenderWorker` owns the `EngineSession`. Viewport and Game frames come from the worker; the GUI thread only shows them (`RenderView::setFrame`).
- The Game view's aspect and resolution presets set the render size (`updateRenderSize`).

## Limits (stated, not fixed)

- Blended draws are not depth-sorted. Overlapping transparent objects can blend in the wrong order.
- Shader Graph materials do not cast shadows.
- Shader Graph materials get no outlines.
- Textured graphs use a 1×1 white fallback texture when no texture is bound.
- No vertex-colour input exists in the graph, so per-vertex colour (particles, baked vertex colour) cannot be used.
