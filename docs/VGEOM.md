# Virtual geometry for static meshes (item 21)

A static mesh no longer has to be drawn as one index buffer. It is split into 128-triangle clusters, four clusters are
welded into a coarser one, and that repeats until a single root is left. At draw time the renderer walks that tree and
keeps the clusters whose geometric error is small enough for the pixels they cover — close geometry stays fine, distant
geometry is replaced by its parents, and the vertex buffer never changes.

This is the idea behind Nanite, and the implementation here follows the reference project
[`Jacinth-19/nanite-at-home`](https://github.com/Jacinth-19/nanite-at-home) (a fork of `P1ben/nanite-at-home`): METIS
k-way partitioning of the face graph into 128-triangle clusters, a cluster hierarchy with per-cluster error, and a cut
selected against a view-dependent metric. It is **not** a port. The reference is an OpenGL/Windows program built on
METIS, OpenMesh and SSBO-driven draws; none of those exist in Ukiyo. The stages were re-cut for what the engine
actually has, and the differences are written down in [§7](#7-what-is-different-from-nanite).

![The Terrain Lab tile, source and two cuts](vgeom-levels.png)

*The 131 072-triangle Terrain Lab tile built into one tree, from the same camera. **Left:** every source triangle.
**Middle:** the cut at a 1 px error budget — 41 407 triangles in 322 clusters, almost all leaves. **Right:** the cut at
8 px — 17 632 triangles in 93 clusters, mostly level 1–2. Clusters are tinted by level (green leaf → blue root). Made
offline by `tools/vgeom_preview.cpp`; the cut, the ranges and the counts are the engine's, only the shading is the
tool's. A pixel-exact comparison against the source leaves 5 of 367 742 pixels uncovered, all on the tile's border.*

## 1. What the reference does, and what carries over

| Stage | nanite-at-home | Ukiyo |
| --- | --- | --- |
| Cluster the faces | METIS `METIS_PartGraphKway` on the face-adjacency graph, `BASE_CLUSTER_SIZE` 128, with METIS' balance and cut objectives | Own BFS over welded edges, 128 triangles a leaf, plus a pocket merge. **METIS is not vendored** and its `libraries/` folder is MSVC-only, so the dependency is gone rather than shipped |
| Build the hierarchy | Parent groups, `children`, `neighbours`, `_parental_siblings`, per-cluster `error`, `parental_area`, `parental_triangle_density` | Groups of four in Morton order, per-parent `meshopt_simplify`, error = max(children, measured residual). Area and density metrics are not kept: the cut uses projected error directly |
| Simplify | `StaticDecimator`/`Decimator` on an OpenMesh copy, per cluster | `meshopt_simplify` on a compacted `rhi::Vertex` array, border locked, absolute error budget. OpenMesh is not used; `meshoptimizer` is already part of the engine for item 15 |
| Choose the cut | `LodTree::FillFaceVertRecursive`, `ShouldShow(metric)`: leaves always, else `error * childsib_triangle_density < metric² / 25` (the reference's own comment calls this "prone to error") | Screen-space error: `error · scale · pixelScale / distance`, refine while it exceeds a pixel budget. A non-positive budget forces the leaves |
| Draw | SSBO cluster/face/vertex buffers, compute passes, a traversal that concatenates selected clusters into a fresh vertex and face list | The cut is a list of index ranges into **one** index buffer that indexes the primitive's **existing** vertex buffer. No vertex duplication. The RHI has storage buffers, compute dispatch and `drawIndexedIndirect` (`compute_test`), and the renderer uses them: `vgeom_cut.comp` assembles the cut on the queue — one dispatch per draw — and each pass issues one indirect draw over the region it wrote (`vgcut_test` pins the kernel to this document's CPU algorithm; the CPU walk remains the fallback) |

## 2. The tree

`buildVGeom` (`src/engine/vgeom.h`, `src/engine/vgeom.cpp`) is plain CPU code: no device, no RHI, so it is testable
without a GPU and the offline tools can use it.

1. **Weld and adjacency.** Position-only welding at 1 mm (`llround(coord · 1000)`, 21 bits per axis) builds an
   edge → triangle map; two triangles sharing an edge are neighbours. This also closes the seams a glTF loader leaves
   when it splits a vertex for normals or UVs, which is what keeps cluster borders shared.
2. **Leaves.** Grow breadth-first from the lowest unassigned triangle until 128 triangles are claimed. A BFS always
   leaves pockets — triangles whose neighbours are all claimed — and those would become one-triangle clusters with
   every vertex on the border. `mergePockets` folds any cluster under `max(8, clusterTriangles/8)` into its largest
   neighbouring cluster before the tree is built.
3. **Parents.** Each level sorts its clusters by a Morton key (21 bits per axis over the model bounds) and groups four
   consecutive ones. Morton order keeps a group's patch compact and its locked border short. Group sizes are spread so
   that no group is left with a single member: a one-child parent is a duplicate LOD, and a chain of them is how a tree
   stops getting coarser near the root.
4. **Simplify.** A group's triangles are gathered over the original vertex ids, the vertices are compacted into a local
   array, and `meshopt_simplify` runs with `meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute`. The border is
   never moved, so a child's edge and its neighbour's edge are still the same vertices at every level. The target is
   `max(minTriangles, groupTriangles · targetRatio)` — a quarter of the group at the default, which is what makes the
   triangle count per level actually halve. The error budget is `max(levelError, 2 · worstChildError)` where
   `levelError` is 0.2 % of the model's diagonal. The stored error is `max(worstChildError, meshopt's result)`: a
   parent is never allowed to look better than the children it replaces.
5. **Ranges, not copies.** A cluster stores `firstIndex`/`indexCount` into `VGeomMesh::indices`. Leaves come first and
   their ranges are contiguous, so the leaves as a set are exactly the primitive's source index buffer.

For the Terrain Lab mesh (131 072 triangles, 66 049 vertices, 256 m across), the build takes **109 ms** and produces:

| Level | Clusters | Triangles | Average |
| --- | --- | --- | --- |
| 0 (leaves) | 1024 | 131 072 | 128 |
| 1 | 256 | 37 856 | 148 |
| 2 | 64 | 27 894 | 436 |
| 3 | 16 | 16 748 | 1 047 |
| 4 | 4 | 9 638 | 2 410 |
| 5 (root) | 1 | 2 408 | 2 408 |
| **Total** | **1365** | **225 616** | |

The root holds 1.8 % of the source triangles. The first level barely shrinks (148 against 128) because almost all of a
four-leaf patch's vertices are on its locked border; level 2 is where the border stops dominating.

## 3. The cut

`selectVGeom` walks from the root. For each cluster it tests the bounding sphere against five frustum planes (left,
right, bottom, top, far — the near plane is left out on purpose, because its convention differs between zero-to-one and
minus-one-to-one clip spaces and a cluster behind the camera only costs a clipped draw), then computes

```
pixels = error · scale · pixelScale / max(|eye − centre| − radius, 0.05)
```

and refines while `pixels > errorPixels && childCount > 0`. `pixelScale` is `height / (2 tan(fovY/2))`, so the result
is in pixels of the current viewport; the renderer passes `0.5 · intH · P11` from the projection it is about to use.
A zero or negative budget forces the leaves: that is what a shadow pass or a hero shot wants.

Culling is conservative and that matters: the planes come from the view-projection's rows, **normalized** before the
sphere test. The raw rows carry the projection's scale, which differs per plane; without the normalization the side
planes over-cull by two to three times and the edges of every coarse patch drop out. That was a real hole in the first
image of this page, and `tests/vgeom_test.cpp` now checks that no triangle whose three vertices project inside the clip
cube is missing from a leaf cut.

The traversal writes into a caller-provided vector (the renderer keeps one for the frame) and a returning overload
exists for tools and tests. A full call over 1365 clusters takes **12 µs**. This is also the algorithm the compute
kernel runs: `shaders/vgeom_cut.comp` performs the same plane test and the same refinement on the GPU, and
`tests/vgcut_test.cpp` requires the two to emit byte-identical indirect commands for every camera and budget it
tries — the CPU walk is the kernel's pinned reference, not just its fallback.

## 4. In the renderer

`Renderer` (`src/engine/renderer.cpp`) computes one cut per draw per frame, at gather time, and every pass that frame
reuses it. Two paths, selected once when the pipelines are built:

* **GPU cut (the default).** When `vgeom_cut.comp` compiles (`vgGpu_`), gather only carves the draw a region of the
  frame's work buffer — `[idCount, cmdCount][ids][commands]` in uints — and after the gather the renderer uploads the
  buffer with those headers and command tails zeroed, uploads the selection view (the five normalized planes, eye,
  pixel scale, budget, cull flag) into a storage buffer, and records **one dispatch per vgeom draw**. The kernel
  walks, sorts (cluster id order *is* `firstIndex` order — the builder appends indices in creation order), merges
  contiguous runs and writes indirect commands with `atomicAdd`. `Device::dispatch` brackets every call with
  barriers, so the next item's atomics and the draws both see each region's writes; a growth of the work buffer
  waits for idle first, because the per-primitive descriptor sets bind it by pointer.
* **CPU fallback.** If the shader is missing or fails to build the pipeline, the flag stays false and the original
  `selectVGeom` walk runs at gather — the behaviour before this section existed, kept for tools and harnesses.
* **Colour, depth prepass, outlines, graph materials** draw the cut. The CPU path sorts the clusters by `firstIndex`
  and merges adjacent ranges into `drawIndexed` calls. The GPU path issues **one `drawIndexedIndirect` per draw per
  pass** over its command region: every entry is a merged run, every unwritten slot a zero command that draws
  nothing, so no readback is needed to size the draw.
* **Shadows** keep the classic path. The shadow pipeline binds the distance-LOD index buffer it bound before the
  feature existed, so a cut never changes a shadow silhouette and a coarse parent never opens a gap in one.
* **Distance LOD** (`buildModelLods`, item 15) still picks that shadow buffer; the colour passes ignore it for a
  primitive with a tree, so the two do not fight.

The renderer exposes `lastVGeomStats()` (clusters, leaves, triangles, culled). It fills only on the CPU path: the
GPU path cannot report a cluster count without a mid-frame readback, which would cost a stall it exists to avoid.

**Building and caching.** `buildModelVGeom` (`src/engine/model.cpp`) runs from the editor's model load and writes the
tree's index stream into one GPU index buffer per primitive, alongside the existing per-LOD buffers, plus two
storage buffers — the clusters as three vec4s (48 bytes each, matching the kernel's std430 struct) and the children
stream — which the compute path reads and the CPU path never touches. It skips primitives under 256 triangles, any
primitive with morph targets, and every primitive of a model with a skinned draw: the error is measured on the rest
pose, which is wrong once a skin deforms it. Virtual geometry is a static-mesh feature here, as in the reference.

The tree is cached beside the model as `<model>.vgeom` — one small versioned binary (`UKVGSET`) holding one `UKVG1`
blob per primitive plus a hash of the build settings. `loadVGeomSet` accepts the entries whose `sourceHash` still
matches the primitive and returns the rest empty, so a changed mesh rebuilds only itself. A read-only asset folder
simply rebuilds every load; nothing breaks. The hash is FNV-1a over positions and indices, and the cache is not written
unless something was actually rebuilt.

**Editor.** Project Settings → Quality has **Virtual geometry** and **VGeom error (px)** (0.25–8, step 0.25). Both are
written to the scene's `settings` object as `vgeom` and `vgeomPixels`, so a scene keeps the look it was tuned with.

## 5. What it costs, and what it buys

On the Terrain Lab mesh, from the camera in the figure:

| Budget | Clusters | Triangles | Share of source |
| --- | --- | --- | --- |
| source | — | 131 072 | 100 % |
| 0.25 px | 360 | 46 193 | 35.2 % |
| 1 px | 322 | 41 407 | 31.6 % |
| 8 px | 93 | 17 632 | 13.5 % |

The response is gentle because a 256 m tile seen from 300 m is *large on screen*: at a 1 px budget most of it still
needs leaf-level detail. The same tree from a 400 m hover at 8 px is 19 360 triangles in 31 clusters (levels 2–3); at
24 px it is the root, 2 408 triangles, which is exactly what a static prop that fills a small part of the frame
collapses to.

Costs, measured on this machine (single thread, `-O2`):

* build: **109 ms / 131 k triangles** (0.83 µs a triangle), once per mesh, cached afterwards — a million triangles is
  about a second, a load hitch rather than a frame cost;
* selection: **12 µs / 1 365 clusters**, per draw per frame (0.07 % of a 60 fps frame), and that upper bound is the
  whole tile; a normal prop is a few microseconds;
* memory: the index stream is 1.7× the source index buffer (225 616 vs 131 072 triangles, 4 bytes an index) and the
  vertices are shared with the ordinary path.

## 6. Tests and the preview tool

`tests/vgeom_test.cpp` runs without a device and is built by CMake as `vgeom_test`; CI runs it on every push. It builds
a 9 216-triangle sphere and checks 29 things: the leaves partition the source triangles exactly (no hole, no duplicate),
parent error is at least the worst child, a parent is coarser than its children together, every cluster range is valid,
triangles fall with distance and with the budget, a zero budget inside the frustum equals the source, the plane test
never drops a fully visible triangle, `cull = false` never culls, the build is deterministic, and the cache round-trips
with stale-hash and stale-setting rejection.

```sh
./build/vgeom_test       # 29 checks, exit code 0
```

`tests/vgcut_test.cpp` is the kernel's counterpart: it re-implements the compute path's phases — same planes, same
stack walk, same bitonic sort, same run merge, same command layout — and compares the result to `selectVGeom` plus
the renderer's merge for six cameras × six budgets × six model transforms (including non-uniform scale), cull on
and off, and a fully culled view where both paths must produce zero commands. It runs on every OS in CI, no device
required; the GPU jobs then exercise the shader itself through `lod_gpu_test`.

```sh
./build/vgcut_test        # 227 checks (223 kernel/cpu comparisons), exit code 0
```

`tools/vgeom_preview.cpp` renders the figure on this page without a GPU: it welds the Terrain Lab heightmap into one
mesh, builds the real tree, selects real cuts and rasterizes them with a flat Lambert, tinting each cluster by level.

```sh
cmake --build build --target vgeom_preview
./build/vgeom_preview docs/vgeom-levels.png . 1800 900
```

`VG_EYE=x,y,z` and `VG_TARGET=x,y,z` move the camera, and `VG_BUDGETS=a,b,c` replaces the three panels' error budgets.
The tool prints every panel's triangles and clusters, and the percentage of source pixels its cut left uncovered —
which is how the frustum bug above was found (5 % before the fix, 0.001–0.005 % after, and those few pixels are the
tile's own border).

## 7. What is different from Nanite

* **Selection runs on the compute queue, but serially.** `vgeom_cut.comp` is one thread per draw: walk, sort, merge,
  all phases inside a single invocation. That moves the cut off the CPU for the price of a kernel that scales with the
  tree (`pow2 · log² pow2` compares), but there is no Hi-Z, no two-pass occlusion and no per-workgroup parallelism —
  frustum and screen-space error only, exactly what `selectVGeom` tests. The CPU walk remains the fallback when the
  shader is absent and the reference every comparison test asserts against.
* **The renderer draws the GPU cut with `drawIndexedIndirect`.** One indirect call per vgeom draw per pass over a
  pre-sized command region; the kernel merges contiguous clusters into runs, and unwritten slots are zero commands
  that draw nothing — the no-readback publish contract `compute_test` proved on both backends. The CPU path still
  calls `drawIndexed` directly when it is the one running.
* **No streaming.** Nanite pages cluster data in and out of a virtual texture; here the whole tree lives in RAM and in
  one index buffer. A 1 M-triangle mesh costs ~12 MB of indices.
* **Border vertices are locked rather than stitched.** Neighbouring clusters at different levels share the same border
  vertices, so the cut has no T-junction cracks and nothing needs stitching; the price is that the first level barely
  simplifies (see the table in §2).
* **Static meshes only.** Skinned and morph-targeted primitives keep the ordinary path, because their error cannot be
  measured on the rest pose.
* **No two-pass occlusion, no cluster compression, no mesh shaders.**

Those are the honest gaps, and they are the order in which the system grows. The cut has taken its step: the RHI
primitives (compute dispatch, storage buffers, `drawIndexedIndirect`) ship under `compute_test`, the selection runs
on top of them under `vgcut_test` and the GPU jobs' `lod_gpu_test`, and what remains is page streaming — then Hi-Z,
when a scene needs occlusion the frustum cannot give.

## 8. Turning it off

`RenderSettings::vgeom` (Quality → Virtual geometry) is read every frame. With it off, a primitive that has a tree is
drawn from its ordinary index buffer and its distance LOD, exactly as before item 21. The tree stays resident; nothing
is rebuilt when it is switched back on.
