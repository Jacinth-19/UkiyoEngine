# Flax 1.13, and what of it belongs in this engine

Flax released 1.13 on 21 October 2026 (999 commits, 130 pull requests), a year after 1.12's spring bones and MSDF fonts,
1.11's custom shading models and 1.10's variable vertex layouts. This is the survey the request asked for: what the
release contains, which parts this engine already has, which parts it can take, and in what order — with the first one
built, measured and shipped.

The engine is a stylised (anime) renderer with a cascaded sun, a toon material model, a screen-space pass, virtual
geometry cut on the CPU, and an editor. A feature is worth importing here when it makes a picture visibly better or a
frame cheaper **without** needing hardware this engine does not target: it runs on iGPUs, so RT cores and DLSS are out,
and the audit's RHI limits (no compute, no indirect draws, no GPU queries, no compressed formats) decide what is
reachable today.

## 1. The release

| Flax 1.13 | What it is | In this engine |
| --- | --- | --- |
| **PCSS shadows** | the penumbra's width comes from the light's size, the blocker's distance and the receiver's, for all light types | **built** — [docs/PCSS.md](PCSS.md) |
| **DDGI specular reflections** | ~50 000 probes, +49 MB VRAM, 0.5–0.9 ms on a 2080 Ti for diffuse + specular | the diffuse half of this engine's GI is a screen-space bounce (`src/engine/gi_*`); probes are the next step |
| **HZB + hardware occlusion queries** | a pluggable occlusion-culling pipeline over a depth pyramid, dynamic shadows and terrain included | blocked: the RHI has no queries and no indirect draws, so a culled set cannot be built on the GPU |
| **Split-screen local co-op** | several cameras into one frame, per-viewport GUI | reachable; the post pass already carries a split direction for the debug view, which is not the same thing |
| Reversed-Z | depth precision rearrangement | not now: it touches every depth test, both backends and the shadow comparison |
| .NET 10 / C# 14 | the scripting runtime | not applicable — this engine's scripts are Lua |
| Properties panel search | filter the inspector | small and worth doing when the inspector next changes |
| Wireframe view | a new debug view | the engine has a depth/normal/outline view set; a wireframe pass is cheap to add |
| Audio device detection | enumeration at run time | miniaudio already enumerates; presenting it in the editor is UI work |
| GI half-res + bilateral upscale | cheaper probe/trace resolution | the traced bounce already runs at half resolution |

## 2. What the earlier releases still owe this engine

| Flax | Feature | State here |
| --- | --- | --- |
| 1.12 | **Spring bones** | already present (secondary motion on the character rig) |
| 1.12 | **MSDF fonts** | **done**: one generated page, three distance channels, sharp at 12-96 px ([docs/MSDF.md](MSDF.md)) |
| 1.12 | **Box-projected environment probes** | not present |
| 1.12 | `ProfilerGPU.Dump`-style debug commands | not present; the engine's debug views are fixed |
| 1.12 | Optional linear colour space | not present: the pipeline is display-referred after the tonemap |
| 1.11 | **Custom shading models** | partly: the toon model is fixed, the shader graph composes fragments, not BRDFs |
| 1.11 | Decal layers via stencil | not present |
| 1.11 | Async scene loading with a time budget | not present |
| 1.10 | Variable mesh vertex layout, 4 UVs, 65 535 bones | the loader is fixed-layout, 2 UVs, 256 joints |

## 3. The order this engine's situation argues for

1. **PCSS shadows** — done. The sun is the only shadow caster, so one implementation covers every shadow the engine
   draws, and contact hardening is the difference between a stylised frame that reads as flat paper and one that reads
   as lit. Measured in [docs/PCSS.md](PCSS.md): 0.055 mean error against 0.066 for a fixed filter handed its best
   radius, 0.266 for the map's own edge.
2. **DDGI specular reflections** — the biggest visual jump left. The screen-space bounce dies at the screen edge and
   in a reflection, and a probe grid fixes both. It needs a storage buffer and a dispatch per probe update, which is
   the RHI work below, and it is the reason that work is worth doing.
3. **RHI compute, indirect draws and GPU queries** — the audit's top gap, and the gate on 2, on HZB culling and on
   grass. Vulkan and GL both have the pieces; the descriptor model needs a storage-buffer set type, and the frame
   graph needs a dispatch node.
4. **HZB occlusion culling** — follows from 3, and pays for itself on the terrain, which is the densest thing the
   engine draws.
5. **MSDF fonts** — done. One page generated at 40 px serves every size the HUD draws, and the measurement that came
   out of it is worth more than the feature: the textbook two-pixel alpha ramp puts 21-64% more ink than a rasteriser
   on 12-13 px text, and a one-pixel ramp fixes it (48 checks, [docs/MSDF.md](MSDF.md)).
6. **Split-screen local co-op**, **box-projected probes**, **linear colour space**: each is a contained feature, and
   none is on the critical path of a prettier frame.

## 4. What PCSS cost, and what it taught

Three commits' worth of work, of which the algorithm was the smallest part:

- the CPU reference (`src/engine/pcss.h`) is 140 lines and is what the test and the figure run;
- the shader path is 50 lines and reads its two scales from the cascade's own matrix, so a cascade whose box changes
  size with the camera keeps the penumbra the same number of metres;
- the two bugs that mattered were both receiver geometry: a slanted receiver counting its own plane as a blocker, and
  a world distance measured on the light's plane without the `|N·L|` foreshortening. Before them PCSS lost to a fixed
  filter; after them it wins at equal tap count.

The second is worth remembering for DDGI: the same mistake — measuring a world distance in a buffer's own parameter
space — is exactly what makes probe sampling look "washed out" at grazing angles, and it is invisible in a test that
only compares against another approximation.
