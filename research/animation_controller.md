# Animation controller: Unity comparison and gap list

Research only. No code changed. Sources are Unity's manual and Unity Learn pages, and our code as of this writing.

## What Unity has (summary)

- **Parameters**: Float, Int, Bool, Trigger.
- **States and transitions**: a state plays one motion (a clip or a blend tree). A transition has a duration (seconds or a fraction of the source clip), an offset into the target, an exit time, and conditions. Interruption source and ordered interruption decide which transitions can interrupt a crossfade that is already running. Any State transitions can fire from any state. Self-transitions are optional. Sub-state machines group states.
- **Blend trees**: 1D; 2D Simple Directional; 2D Freeform Directional; 2D Freeform Cartesian; Direct (one parameter per motion, several motions can have weight 1). Motions can be blend trees themselves. Each motion has a threshold (or position), a time scale, and a mirror flag. Automate Thresholds spaces them evenly. Compute Thresholds (1D) and Compute Positions (2D) set them from root motion: speed, velocity, or angular speed. Adjust Time Scale with Homogeneous Speed makes the root-motion speeds match across motions. ([Unity 1D blending](https://docs.unity3d.com/Manual/BlendTree-1DBlending.html); [Unity Learn, blend trees](https://learn.unity.com/course/the-animator/tutorial/3-4-creating-and-configuring-blend-trees?version=2019.4))
- **Animation layers**: each layer has a weight, a blending mode (Override or Additive), an optional avatar mask (the bones it drives), and a Sync option (copies another layer's state machine, with or without Timing). The top layer's weight is fixed at 1 in Unity. ([Unity, Animation Layers](https://docs.unity.cn/540/Documentation/Manual/AnimationLayers.html); [Unity Learn, layers](https://learn.unity.com/course/introduction-to-3d-animation-systems/unit/the-animator/tutorial/3-7-creating-and-configuring-animator-layers?version=2019.4))
- **Interruption**: transitions out of Any State are queued first. Interruption source is checked in that order when Ordered Interruption is on. Unity users report that the combination is hard to predict. ([Unity Discussions, case 893600](https://discussions.unity.com/t/case-893600-any-state-transitions-to-sub-state-machines-ignore-interruption-source/664860))

## What we have

| Area | Unity | Ours | Where |
|---|---|---|---|
| Float, Bool, Trigger parameters | yes | yes | `src/systems/anim_state.h`, editor state graph |
| Int parameter | yes | no | |
| Transitions: duration (seconds or fraction), offset, exit time, self-transition | yes | yes | `AnimTransitionOptions` in `anim_state.h` |
| Transition conditions | yes | Greater, Less, Equal, IsTrue, IsFalse, Trigger | editor state graph |
| Any State | yes | yes (from the graph's "Any state" source) | `anim_graph.h` |
| Interruption source, ordered interruption | yes | no interruption setting. How a second transition behaves during a crossfade was not checked in the code. | |
| Sub-state machines | yes | no | |
| Blend: 1D | yes | yes, linear between neighbours | `blendWeights` |
| Blend: 2D Simple Directional | yes | yes | `blendWeights` |
| Blend: 2D Freeform Directional | yes | yes. Our own weighting. Unity's formula is not verified. | `blendWeights` |
| Blend: 2D Freeform Cartesian | yes | yes, inverse-squared distance | `blendWeights` |
| Blend: Direct | yes | no | |
| Nested blend trees (a blend as a motion) | yes | no. A motion is a clip name. | `BlendMotion` |
| Per-motion time scale | yes | no | `BlendMotion` (clip, x, y only) |
| Per-motion mirror | yes | no | |
| Automate thresholds (1D evenly spaced) | yes | no | |
| Compute thresholds / positions from root motion | yes | no | |
| Homogeneous speed | yes | no | |
| Layers with weight | yes | no. The player has two slots that crossfade (`layers_[2]` in `anim.h`). This is a crossfade, not a Unity layer. | `anim.h` |
| Override and Additive blending | yes | no | |
| Avatar masks (bone subsets per layer) | yes | no | |
| Layer Sync | yes | no | |
| Root motion (take root translation from a clip) | yes | no | none in the code |
| Animation events | yes | yes, authored in the timeline; read with `eventsBetween` | `anim.h`, `anim.cpp` |
| IK pass | yes | partial: hand-on-hip goal and foot/hand IK in the player | `anim.h` |
| Time scale per state | yes | not found in the code read (a blend motion has none either) | |

## Gaps, in build order

Each step is independent. Order is by cost against how much it unblocks.

1. **Blend tree motion options** (small, self-contained). Add time scale and mirror to `BlendMotion`. Add a Direct blend kind (one parameter per motion, weights used as given). Add Automate thresholds for 1D, and Compute thresholds from speed, which needs root motion (step 4), so do speed later. Files: `anim_state.h`, `blendWeights`, `.ukanim` save and load, editor motion table.

   **Status (done):** time scale, mirror (by name pairs `_l/_r`, `_L/_R`, `Left/Right`; a bone with no partner is central), Direct, Automate thresholds (1D only; keeps the current order and range), save and load, validator, and motion table columns. Editor selftest 548 passed, 0 failed. Full build and `systems_test` pass. Not yet checked: mirror output on the UAL1 rig (the math assumes a symmetric rest pose), and Compute thresholds (waits for step 4).
2. **Int parameter.** Small. Parameter type in the state machine, editor list, and the Equal and comparison conditions.
3. **Animator layers with weight and an avatar mask** (large). A layer is a state machine plus a weight, a blend mode (override or additive), and a bone mask. The player must blend per bone with the mask. The current two-slot player becomes the base of this. Sync (same state machine, different clips) comes after the mask works. This is the step that makes "upper body wave while walking" possible.
4. **Root motion** (medium). Read the root translation and rotation from clips. Add a flag per clip state: apply to the entity, or keep in place. Needed for step 1's speed-based thresholds, and for locomotion that moves with physics or navigation.
5. **Interruption source and ordered interruption** (medium). Define the rule in our terms (the queue order is Any State first, then the rest), then test it against cases like Unity's: a trigger from Any State while a crossfade is running.
6. **Sub-state machines** (medium). Nested state machines with entry and exit nodes. Depends on step 5, because a transition into a sub-machine needs an interruption rule.
7. **Nested blend trees** (small once step 1 is in). A motion may be a blend tree. This needs the weight of the child tree to multiply the parent's weight.

## Notes on our own code

- `Layer` in `anim.h` is a crossfade slot, not a Unity layer. The name is easy to confuse with step 3. Rename it or document it before step 3.
- The Freeform Directional weighting is our design (see `editor_panel_audit.md`, section 17). Unity's own formula is not public in the manual we read. If exact Unity match matters, check its output against Unity directly.
- Blend states ignore the start-offset option, which applies to clip states only. This is listed as a limit in `editor_panel_audit.md`.

## Not checked

- Unity's exact transition rules when an Any State transition and a running crossfade compete. The cases above come from user reports, not from the manual alone.
- Whether the state graph's Any State has priority over other transitions. The code was not read for that.
