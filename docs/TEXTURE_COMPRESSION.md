# Compressed textures: the BC family (batch 7, first half)

Every texture the engine drew was RGBA8 — 32 bits per pixel whether it held a photograph, a flat toon colour or a
normal map. Batch 7 teaches the engine to store and sample them compressed: **BC1** (4 bits per pixel, opaque
colour), **BC3** (8 bits, colour plus a real 8-bit alpha path), **BC5** (8 bits, two channels, for normal maps).
The second half of the batch adds ASTC for the phone; the plumbing below was written so ASTC is another entry in the
same tables.

The work has three layers, each with its own test:

## 1. The codecs are ours (`src/engine/texture_compress.{h,cpp}`)

The engine ships its own BCn encoders rather than a vendored one, for the same reason it ships its own toon shader:
the bytes are ours to measure. The algorithms are the standard ones from the Khronos Data Format Specification:

* **BC1 colour** — per 4×4 block, the bounding box of the texels in RGB565, endpoints refined by a greedy ±8 walk
  per channel that keeps whatever lowers the block's total squared error, then the two interpolated midpoints
  (2·c0+c1)/3 and (c0+2·c1)/3, and the nearest-palette index per texel. The encoder always keeps c0 > c1 (the
  four-colour mode): the engine only emits BC1 for opaque textures, so punchthrough transparency never bites.
* **BC4 alpha / channels** — an 8-value palette (two endpoints, six interpolated steps of (7−s)/7), 3-bit indices.
* **BC3** = alpha block then BC1; **BC5** = red block then green block.

Both an encoder **and a decoder** live in the module, which is what makes the quality a test and not a hope:
`texture_compress_test` (34 checks) round-trips gradients, flat colours, alpha ramps and odd sizes, and bounds what
was lost — RMSE < 3 on a gradient for BC1, the alpha-0.5 crossing of a cut-out preserved on all but 1/16 of texels,
BC5 channels within 3. It also pins the exact block arithmetic the RHI allocates by (8/16 bytes per 4×4 block,
partial edge blocks rounded up to whole blocks, so a 1×1 tail mip costs a full block), exact 8:1 on full-size BC1
levels, and byte-identical determinism, because a bake that differs between runs would make every screenshot
comparison meaningless.

Two bugs the tests caught during development, recorded because they are the two classes of bug this format family
produces: 3-bit indices cross byte boundaries, so a one-byte read silently drops the high bit (the read assembles
two bytes); and a `memcpy(dst, src, 4)` from an `int*` palette copies 4 **bytes**, not 4 channels — the computed
alpha never reached the output and the round-trip "passed" on colour alone.

## 2. The RHI grew the formats

`Format::BC1, BC1_SRGB, BC3, BC3_SRGB, BC5` — 4×4 blocks, sample-only (no attachments, no `update()`, no
generate-mipmap: each level of the CPU-built chain is compressed *before* upload, which is also how the
alpha-coverage-preserving chains keep their coverage).

* **OpenGL** — `glTextureStorage2D` with the compressed internal format, one `glCompressedTextureSubImage2D` per
  level. S3TC is not core GL; it is on every desktop GL 4.x driver this engine targets (Mesa and proprietary), and
  the enums are defined locally so the build never depends on the header having them.
* **Vulkan** — the `*_BLOCK` formats, six-block-aware upload maths (the copy extent stays the level's virtual size;
  a whole-mip copy from the origin is legal at any size), and `textureCompressionBC` queried from the physical
  device: a device without it **refuses** the format at creation instead of sampling garbage.
* `Device::supportsBlockCompression()` tells the loaders whether to bother.

## 3. The loader uses it

Model textures (`src/engine/model.cpp`) are compressed when the option is on (it is, by default,
`setCompressedModelTextures`) **and** the device supports BCn. Every mip level — the alpha-coverage-preserving chain
included, which ran first — compresses to BC3 when the texture uses alpha anywhere and BC1 when it does not,
matching the sRGB flag of the source. Textures smaller than 8×8 stay raw: a block of metadata saves nothing.

The phone gate is not a stub: Android GPUs have no BCn, so the loader checks `supportsBlockCompression()` and keeps
RGBA8 there. ASTC (second half of the batch) replaces that with a format phones do support; the frame path, chain
contract and tests are already written to take it.

## What it saves

A model with 20 MB of RGBA8 texture data draws from ~5 MB of BC1/BC3 — and the saving is on the sampling path too:
4 or 8 bits per pixel of texture fetch traffic instead of 32. The decoded image is not bit-identical, by design; the
bounds in §1 are the contract for how far from it the games may drift, and the CI frame photos are checked on every
push with the compressor on.
