# Trails (item 27)

A sword arc, a drift smoke ribbon, a skate line in the snow. Godot 4.8 shipped these as **Trail3D** and **Line3D**
(GH-117107): a node that accumulates sections as something moves, then builds a strip of quads — a ribbon — whose width,
colour and UVs are all properties along the trail.

This is that feature in Ukiyo's idiom. There is no scene tree here and no `Node3D`; a trail is **geometry**: a list of
points, and a function that turns them into triangles. The CPU owns that function, the RHI draws its output, and the
same triangles can be rasterised offline for the figure below. Godot's code was not copied and its resource system was
not imitated; what carried over is the *feature set* — the limit modes, the width curve, the gradient, the alignment and
tiling modes, and the UV pinning — because that is the set a game actually authors against.

![Trails: an additive sweep, three UV modes, a ground ribbon, and what the CPU owns](trail-preview.png)

*Every ribbon in this sheet is built by `src/engine/trail.cpp`; the shading is a software rasteriser in
`tools/trail_preview.cpp`, because the machine that produced it has no GPU. **Top left:** a 190° sword arc,
billboarded, additive, aged out at 0.30 s — 32 sections, 3.67 m, 62 triangles. **Top right:** the same straight ribbon
under all three UV mappings, with `u` drawn as stripes instead of a texture, so the difference is the mapping and nothing
else. **Bottom left:** a dust trail on the ground with `localUp = +Y` and `PIN UV` off — the ribbon's width is the
same from every angle because it does not billboard. **Bottom right:** the state the CPU keeps and the two curves that
shape the strip.*

## 1. Where it lives

```
src/engine/trail.h            TrailSettings, TrailGradient, TrailWidthCurve, Trail, TrailVertex
src/engine/trail.cpp          emit/update/trim, tangents, and buildRibbon's two overloads
src/engine/trail_renderer.h   TrailRenderer: one pipeline, one vertex buffer, one draw
src/engine/trail_renderer.cpp upload + blend + soft edge
shaders/trail.vert            world-space strip, no camera work
shaders/trail.frag            gradient colour, soft edge, discard
tests/trail_test.cpp          53 checks, run in CI on Linux and Windows
tools/trail_preview.cpp       the figure above, without a GPU
```

`Trail` has no device, no clock and no allocation after the first frame. Time is a parameter — `update(dt)` — so a test
drives it deterministically and a replay produces the same ribbon twice.

## 2. The model

| Type | What it is |
| --- | --- |
| `TrailSettings` | the look: limit mode and its value, `minSectionLength`, width, width curve, gradient, alignment, tiling, `pinUv`, `maxSections`, `localUp` |
| `TrailGradient` | colour stops by `t` (0 at the tail, 1 at the emitter), linearly interpolated. Its **alpha is the fade** |
| `TrailWidthCurve` | piecewise-linear points `(t, multiplier)`, sampled into `halfWidth = 0.5 · width · curve(t)` |
| `Trail` | the sections, `emit`, `update`, `clear`, `buildRibbon` |
| `TrailVertex` | position, `uv`, colour — the three things `shaders/trail.vert` reads |
| `TrailRenderer` | pipeline, buffers, one indexed draw per blend mode |

Godot's `Curve` and `Gradient` resources are replaced by the two small classes above. A resource system that loads
curve presets is not what an entity's trail needs, and a straight-line interpolation between authored points is what the
presets in the reference were doing anyway.

## 3. A frame

```cpp
ukiyo::Trail trail(settings);              // billboard, lifetime, tile

// once per frame, after the object has moved:
trail.emit(objectWorldPosition);
trail.update(dt);

std::vector<const ukiyo::Trail*> trails{&trail};
renderer.setTrails(trails /* alpha */, {} /* additive */);
renderer.render(cam, models, time, frameIndex, settings);
```

`emit` does two things. When the head has moved at least `minSectionLength`, it appends a **section** and the ribbon
grows by one quad. When it has not, it *moves the newest point* instead, so the ribbon always reaches the object rather
than lagging a frame behind. That second half is what makes a slow object still look attached: without it the strip
would end short of the emitter until the next section was due.

`update(dt)` ages every section and trims from the tail:

* **Lifetime** drops sections older than `lifetime`;
* **MaxLength** drops sections from the tail until the path between the first and last point fits `maxLength`;
* both stop at two sections, and `maxSections` is a hard cap above them.

Two sections is the floor on purpose: one point has no ribbon, and an emitter that stops moving should leave a strip
that fades rather than a strip that pops out of existence. Godot's node has the same floor for the same reason.

## 4. Building the ribbon

For each section, the tangent is the central difference with its neighbours (one-sided at the ends), and the side is

```
billboard:  side = normalize(cross(tangent, eye − p))
local:      side = normalize(cross(tangent, localUp))
```

then two vertices at `p ± side · halfWidth`. That is the same construction Godot uses, and it has the same well-known
failure that the `Local` mode exists to fix: a billboarded ribbon whose tangent points at the eye has no defined cross
product, so the strip flips. A trail on the ground should be local — its width must not change as the camera orbits.
A trail around a sword should billboard.

The UVs are:

| Mode | `u` |
| --- | --- |
| `Stretch` | `t · tilingMultiplier`, where `t` is 0..1 along the trail |
| `Tile` | `(arc − arc₀) · tilingMultiplier` — repeats per metre of path, 1 repeat every `1/tilingMultiplier` m |
| any, with `pinUv` | `arc · tilingMultiplier`, measured from where the trail started, so the pattern stays put in space |

`v` is 0 on one edge and 1 on the other in every mode. Indices are `6 · (n − 1)` for `n` sections, wound `(a, b, c)` and
`(c, b, d)`, and the renderer draws **no culling** — a ribbon can twist, and a back-face cull would make half of a
sweep disappear.

`buildRibbon` comes in two overloads: one filling `std::vector`s, one writing into caller-owned arrays with an explicit
capacity. The second exists because the renderer appends many trails into one buffer and must know the sizes up front;
both run the same code path, which is why the test checks one against the other.

## 5. Drawing

`TrailRenderer` follows `ParticleRenderer`: one pipeline per blend mode, built once, and one draw per frame for every
trail that shares it.

* Vertex format is the engine's standard `rhi::Vertex`, so the trail shader reads `inPos` (0), `inUV` (2) and
  `inColor` (5) exactly like a mesh and the RHI needs no new layout.
* Alpha and additive are two instances (`TrailBlend::Alpha`, `TrailBlend::Additive`), created on first use, so a scene
  with no trails pays nothing.
* The renderer draws them after the blended materials and after the water pass, over the HDR colour target with the
  scene depth attached — depth-tested, never depth-writing.
* The buffers grow geometrically and are capped at 1 M vertices; a frame that would exceed the cap skips the whole
  frame's drawn trails rather than drawing half of one.

`shaders/trail.frag` computes the soft edge per fragment: `v` runs 0..1 across the strip, and the outer band fades out
with `smoothstep`. Softening **per vertex** would zero both edges and leave nothing in between, because every vertex
sits on an edge — that mistake was made and caught while writing the figure.

## 6. What the tests check

`tests/trail_test.cpp` builds the geometry and **measures** it, rather than asserting numbers copied out of the
implementation:

| Group | What it pins |
| --- | --- |
| 1 | the first emit lays the head; a move shorter than `minSectionLength` moves it instead of adding a section; two sections are a ribbon |
| 2 | two vertices per section and six indices per pair, the array overload builds the same strip, and a buffer that is too small is refused rather than overrun |
| 3 | width at both ends of every quad against the width curve, measured from the geometry |
| 4 | the quad's side is perpendicular to the view ray at every vertex, from any camera |
| 5 | a `Local` ribbon stays in the `localUp` plane and does not twist when the camera moves |
| 6 | `u` in all three tiling modes, and that `pinUv` keeps a section's `u` fixed as the trail grows |
| 7 | the gradient's colour and alpha along the trail |
| 8 | lifetime trims old sections, two stay, a new section starts at age zero, `clear` empties |
| 9 | `MaxLength` trims by distance, not by section count, and keeps all of what fits |
| 10 | the section cap bounds memory whatever the lifetime says |
| 11 | degenerate input: a stationary emitter, a trail that doubles back on itself, a zero width |
| 12 | a shorter lifetime trims on the next `update` without a restart, which is what a settings slider does |

The run is 53 checks. It builds with only `glm` (no device, no glfw), so it runs everywhere CI runs.

## 7. What is different from Godot

| Godot 4.8 | Ukiyo | Why |
| --- | --- | --- |
| `Trail3D` is a `Node3D` with `top_level`, `global`, `local` transforms | `Trail` takes world positions from the caller | the engine has no scene graph for effects; a game already knows the object's world position |
| `Curve`, `Gradient` resources | `TrailWidthCurve`, `TrailGradient` | no resource database for effects; points and stops are what a preset is |
| `Mesh` generated per frame and handed to a `MeshInstance3D` | `TrailRenderer` uploads into its own buffers and draws them | the RHI has no dynamic mesh type, and one buffer for a frame's trails is one draw instead of N |
| the billboard binormal uses `CUSTOM0` (the previous point) in the vertex shader | the tangent is computed on the CPU | the CPU already has the neighbours, and this keeps the shader readable and the geometry testable without a GPU |
| tiling via `texture_repeat` | `tilingMultiplier` scales `u` | same effect, one fewer material field |
| `cast_shadow` | not implemented | a ribbon with alpha 0 at the tail casts a shadow of its geometry, not of its look; shadow casting for trails needs a dithered depth pass and is not part of this item |

The last row is a real gap rather than a decision to hide: a sword arc that casts a shadow will cast the *quad's* shadow
unless the depth pass samples the same gradient. That belongs with the alpha-coverage work (GH-104289) in item 28's
neighbourhood, not here.

## 8. The figure

`tools/trail_preview.cpp` is not part of the engine: it rasterises the ribbon triangles with a flat software shader so a
still can be made on a machine without GL or Vulkan, and CI compiles it on every push so it cannot rot. It is the same
`Trail`, the same settings, and the same `buildRibbon` the renderer uploads — the only thing that changes is who shades
the triangles.

```
./build/trail_preview docs/trail-preview.png 1600 900
```

## 9. Limits

* **`maxSections` is a cap, not a budget.** 64 sections is 128 vertices and 378 indices — trivial — but a game with 30
  trails at 60 Hz should drop the ones off-screen itself; the renderer draws what it is given.
* **No per-trail material.** A trail is untextured on purpose (§5); a textured trail needs a material set, which needs
  per-trail draws, which is a different batching decision than the one this item makes.
* **No collision or ground clamping.** A ground trail follows the points it is given; conforming it to terrain belongs to
  the terrain query, not to the ribbon.
* **The renderer's draw is one call for every trail in a list.** Two blend modes is two calls; a game that wants a
  hundred materials will want a different design.
