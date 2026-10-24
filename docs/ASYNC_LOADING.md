# Loading with a frame budget

Flax 1.11's async scene loading, in the form this engine can actually use: not a thread, but a scheduler that stops
early. `src/engine/async_load.{h,cpp}` queues a scene as steps and runs as many as one frame's budget pays for.

Opening a scene loads one model per instance, and the naive loop loads all of them before the next frame: a scene with a
dozen models freezes for as long as the slowest pile takes. The fix that matters here is not a thread — most of this
loading is file parsing and buffer creation, and the GPU upload still happens on the main thread — it is to **stop early**.

![What each frame pays, with and without a budget](async-loading.png)

`ukiyo::BudgetedLoader` queues a scene's load as steps, one per model, and `tick(budgetMs)` runs steps until the budget
is spent:

- **A step runs whole or not at all.** A frame that runs out of budget never leaves a half-built model behind, and the
  next frame carries on from the instance it stopped at.
- **The budget is measured, not estimated.** A step starts only if this tick has not already spent the budget, so a frame
  pays the budget plus at most one step.
- **A step longer than the whole budget is counted**, in `overruns()`. That number is the honest part of the feature: on
  the twelve-model scene in the figure, 8 of 12 steps cost more than 4 ms on their own, so the "budget" is really "one
  model a frame". A loader that hid that would be lying to whoever tuned the number.
- **A step that throws does not stop the queue.** One corrupt model costs its own row in the log, not the rest of the
  scene.
- **`runAll()` is still there.** A budget of 0 is the synchronous path — the scene is ready before the first frame, which
  is what a still frame and the self-test want.

The player takes `--load-budget <ms>`; a CI job boots the fixture project with a 2 ms budget and fails if the load never
completes, which is what a budget that is never spent would do.

The figure is the shipping scheduler with a fake clock and a fixed table of step costs, so it is deterministic: 95 ms of
loading lands in one frame without a budget, and across 9 frames with one, worst frame 21 ms.

## What the three cost together

Two of them are about a hundred lines each, and the third is one field in a pipeline description. What took the time
was the part that is not code: deciding that the Inspector's search should be the *browser's* grammar rather than a
fourth variant of it, that a wireframe must not touch the fullscreen passes, and that a loading budget which hides its
overruns is worse than no budget at all.
