# Alpha-coverage-preserving mipmaps (item 30)

An alpha-tested material — a fence, a leaf card, a hair card, a chain-link mesh — gets its shape from which texels pass
the cutoff. A mip chain averages alpha the same way it averages colour, and averaging a mask is the one thing a mask
cannot survive: a 2-texel strand averaged over 4, then 16, then 64 texels sinks below the threshold, and the geometry
that was there simply stops being drawn. The object was never too small to see; its alpha was.

Godot 4.8 shipped the fix as GH-104289: after building each level, rescale that level's alpha so the fraction of the
surface that survives the alpha test is still the fraction level 0 had. RGB is untouched — a surviving texel keeps the
colour it always had — so the chain still filters normally and only the mask is corrected.

![Plain mipmaps against coverage-preserving mipmaps](alpha-mips.png)

*A ground plane of cut-out texture — a 12-texel strand grid, about a third opaque — alpha tested at 0.50 and sampled from
one fixed mip level per column, which is what the GPU does for a surface whose footprint sits at that level. **Top row:**
the plain box filter. Coverage falls with every level, and by LOD 4 only **0.007** of the surface is left: the mesh has
dissolved into the ground. **Bottom row:** the same chain with each level's alpha scaled to hold level 0's **0.374**
— 0.371, 0.372, 0.365, 0.391 — so the mesh stays as dense at distance as it is close up. Made by
`tools/alpha_mips_preview.cpp`, which runs the engine's `buildMipChain` and a trilinear sampler.*

## 1. The measurement

Coverage is not the mean alpha, and it is not the count of texels above the threshold. A minified texture is sampled
between texel centres, so what the alpha test sees is the *bilinearly filtered* alpha over the texel's own footprint. The
measurement therefore samples each texel 4 × 4 with bilinear weights and counts the samples above the threshold:

```
coverage(level, scale) = mean over texels of  (1/16) · Σ samples  [ bilinear(alpha · scale) > threshold ]
```

Using the mean alpha instead would over-correct: a texture that is 20 % opaque has a mean alpha of 0.2, below the 0.5
cutoff, so no single scaling of its *mean* would ever make it pass — but its coverage at level 0 is 0.2 and that is
exactly the number every level should hold.

## 2. The search

```
desired  = coverage(level 0, 1)
for each level after 0:
    box-filter the previous level
    if desired > 0:
        scale = binary search in [0, 4] for coverage(level, scale) ≈ desired     // 10 steps, stops at 2e-3
        alpha = clamp(alpha · scale)
```

Ten steps of bisection is enough because the function is monotone in `scale` and the target is a scalar; Godot's
`Image::_scale_alpha_to_coverage` runs the same loop. The fast path returns 1.0 unchanged when the level already matches,
which is the common case for a texture whose mask is dense.

Raising alpha can bring a texel above the cutoff that was below it; that is the point — it restores surface that the
average had eaten. It can never invent surface where the level has no alpha at all: `scale · 0 = 0`.

## 3. Where it runs

```
src/engine/texture_mips.h|cpp    the chain builder, the coverage measurement and the search, on the CPU
src/rhi/rhi.h                    MipChain: an optional CPU-built chain passed to createTexture
src/rhi/vk_device.cpp            uploads each level with one buffer-to-image copy
src/rhi/gl_device.cpp            uploads each level with glTextureSubImage2D, skipping glGenerateMipmap
src/engine/model.cpp             builds the chain for the base colour of an alpha-masked material, at that material's cutoff
editor/src/main_window.cpp       "Preserve alpha-test coverage in mipmaps (on load)" and the cutoff
```

The chain has to be built on the CPU: the coverage search is a loop of measurements, and no blit or `glGenerateMipmap`
can run it. So the RHI grew one optional argument — a vector of levels — and both backends upload it as-is when it is
present. Textures that do not ask for it keep the backend's own filtered chain, unchanged.

The loader asks for it only where it matters: **alpha-masked** materials (`alphaMode: MASK` in glTF, `mat->mask`), for
the base-colour texture, at that material's own `alphaCutoff`. The same image used by an opaque material and by a cutout
gets two chains, so the cutoff is part of the texture cache key.

## 4. What the test pins

`tests/alpha_mips_test.cpp` — 38 assertions, no device, runs on both CI platforms:

1. **shape** — one level per halving, ending at 1 × 1, level 0 byte-identical to the source, every level the right size;
2. **the filter** — a 2 × 2 of known colours becomes its rounded average;
3. **opaque and empty images are left alone** — the search does not touch what it cannot improve;
4. **the effect** — on a sparse cut-out, preserved coverage stays within 0.04 of level 0 down to 4 × 4, while the plain
   chain is already at half by level 1 and below 1 % by level 4;
5. **only alpha moves** — every RGB byte of every level matches the plain chain, a transparent texel stays transparent,
   and no level gains detail;
6. **the search** — the identity when asked for what a level has, monotone in the target, bounded in `[0, 4]`;
7. **a tighter threshold** — preservation holds at 0.75 as well;
8. **degenerate shapes** — 1 × 1, a single column, and an odd 13 × 7 whose levels never divide evenly;
9. **determinism** — two builds of the same image are byte-identical, and the caller's image is never modified.

## 5. Limits

- **It is a per-level scalar.** One multiplier per level holds the *average* coverage, not the pattern: a strand that
  crosses a texel diagonally is drawn as a blob at that level, because the detail is gone. The figure shows exactly that
  at LOD 5 — dense, but coarse. `bicubic` or coverage-aware downsampling would keep more shape and costs more; Godot does
  not do it either.
- **It only helps alpha testing.** Alpha *blending* does not threshold, so there is no coverage to preserve.
- **The bottom levels give up.** At 2 × 2 and 1 × 1 there is no scaling that can hold a 37 % coverage, and the search
  returns its best try rather than lying. Nothing is visible there anyway.
- **It costs load time.** About 100 ms for a 1024² chain in the tool's build, once, at load. Nothing at runtime.
