# Profiler

Where a frame went, on two clocks. The CPU stages come from the world; the GPU brackets come from the RHI. Both
appear in the editor's Game view stats overlay while playing, and both are switches rather than a permanent tax —
with one deliberate exception noted below.

## The CPU side: `FrameTiming`

`World::tick` reads the clock around each stage when `setProfiling(true)` is on (Play turns it on; a shipped game
does not), and stores one frame in `World::lastTiming()`:

```
input · scripts · physics · characters · navigation · animation · particles · entities · total
```

`FrameTiming::share(stage)` is each stage's fraction of `total` — the bar chart's numbers, and a test's way of
proving the breakdown adds up rather than repeating the total. Scripts see it as `ukiyo.frameTiming()`. The
editor's overlay shows `Update:` plus every stage over 0.01 ms.

Cost when the profiler is off: nothing — the clock reads are inside the switch. Cost when on: four clock reads per
stage, `std::chrono::steady_clock`, under a microsecond total.

## The GPU side: timestamp brackets

The RHI records GPU time in **numbered brackets** (`src/rhi/rhi.h`):

```cpp
device.beginGpuTime(SLOT);   // outside every render pass
...
device.endGpuTime(SLOT);
const std::vector<float> ms = device.gpuTimesMs();   // kGpuTimeSlots values, slot order
```

Two timestamp queries per slot, one pair per frame. The renderer's names and where each bracket starts and stops:

| Slot | Name | Covers |
| --- | --- | --- |
| 0 | `shadows` | the cascaded shadow atlas pass (always runs — it clears) |
| 1 | `depth` | the depth prepass (near zero when the prepass is off) |
| 2 | `scene` | scene pass, water, trails inside it, motion vectors, view-space normals |
| 3 | `post` | GI trace/accumulate/denoise, screen-space effects, exposure, TAA, bloom, tonemap, stylize, FXAA, present |
| 4 | `hud` | the HUD pass — the whole `drawHud` call, so an empty HUD measures zero rather than dangling |

`Renderer::lastGpuStats()` pairs the names with the numbers; the editor overlay prints `GPU: total ms` and the
per-slot line. The blend of the two views is the point: `Frame: 16 ms` (whole frame), `Update: 3 ms` (the game's
own systems), `GPU: 11 ms` (what the hardware was doing about it).

### One frame of latency, on purpose

`gpuTimesMs()` returns the frame the device **resolved at the last `beginFrame`** — never the one still on the
screen. That is what makes the read free:

* **Vulkan** — the frame fence (the backend runs one frame in flight) has already retired the previous frame
  before the resolve, so `vkGetQueryPoolResults` cannot block. The reset is recorded for this frame's submission,
  after the host read.
* **OpenGL** — `GL_QUERY_RESULT_AVAILABLE` is checked per slot and an unfinished query keeps its old value; the
  read never waits. The swap after each frame is what gets the driver far enough along in practice.

A slot the frame did not write (a HUD that was not drawn, a backend reporting NOT_READY) keeps its previous value,
which reads as "whatever that bracket last cost" — and a device with no timestamps at all returns an empty vector,
which the profiler and the test treat as a skip rather than as zeros.

### Cost

Vulkan: two `vkCmdWriteTimestamp` per written slot, outside render passes — sub-microsecond of queue work, not
gated by the profiler switch (the read is already free, and a profiler that is only sometimes on is a profiler
nobody trusts). GL: two `glQueryCounter` calls, same placement. The CPU `FrameTiming` remains switch-gated because
it is read every tick.

## What is not here

* **Per-draw timings.** The brackets are pass groups; per-draw would be a query per draw (hundreds), a pipeline
  barrier per draw, and a postmortem tool rather than a live overlay. The renderer already exposes draw counts and
  triangle counts per frame, which is what per-draw time is usually wanted for.
* **Timestamps on compute.** There are no compute queues in this RHI yet; when they arrive they reuse the same
  pool (`kGpuTimeSlots` has room) with stage flags appropriate to the dispatch.
* **Memory or bandwidth counters**, and vendor-specific counters (pipelines/vertices) — those want a vendor
  extension per vendor, and none of them have a caller in this engine's games.
* **A Chrome trace dump.** The overlay and the numbers are the product; a trace file writer would be a new tool
  with its own review story.

## Reading it in tests

`tests/wireframe_test.cpp` brackets every frame it renders and asserts, per backend, that the slot vector is
whole, that nothing went negative, and that a real draw measured real time — skipping honestly (a `SKIP` line,
not a pass) when a device reports no timestamps. That runs in CI's device-test step on lavapipe for Vulkan and
llvmpipe for OpenGL, so both backends' query paths are exercised on every push.
