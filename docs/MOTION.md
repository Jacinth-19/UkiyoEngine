# Motion matching

A character that walks, runs, turns and stops without a state machine. Every pose of every clip is a row in a
database; every few frames the engine describes the pose it is in and the path it wants to take, finds the row that
fits best, and jumps there. Locomotion is not "enter the run state" — it is "this is the 412th frame of `walk1`".

The implementation follows [`Jacinth-19/MotionMatchingInUnity`](https://github.com/Jacinth-19/MotionMatchingInUnity),
the reference project that ports the method to Unity over LAFAN1 motion capture. It is a port, not a copy: the
reference's Unity-specific parts are replaced with what this engine has, and the search is a KD-tree instead of a
linear scan over every frame.

## 1. What the reference does

| Stage | MotionMatchingInUnity | Ukiyo |
| --- | --- | --- |
| Motion data | LAFAN1 BVH imported into Unity `BVHAnimationClip` assets | A BVH reader that produces `MotionClip` directly; LAFAN1 files load as they are |
| Features | trajectory prediction plus pose descriptor | 24 numbers: 4 future root positions, facing, foot positions and velocities, root velocity |
| Query | player input becomes a predicted trajectory | a target velocity and heading become the same trajectory shape |
| Search | a linear scan over the clip frames | a static, balanced KD-tree, 8 nearest, pruned |
| Playback | pick and blend in Unity's animator | frame advance, crossfade of joint positions, then a retarget onto the model's humanoid rig |

## 2. The feature vector

All 24 values are in the character's own frame — the root position is subtracted and the root yaw is divided out — so
the same database serves any heading and a walk that curves left is the same data as a walk that curves right.

| Index | Meaning |
| --- | --- |
| 0–7 | root position at +0.15, +0.30, +0.45, +0.60 s, as (x, z) pairs |
| 8–9 | facing direction at +0.60 s |
| 10–12 | left foot offset from the root |
| 13–15 | right foot offset from the root |
| 16–18 | left foot velocity |
| 19–21 | right foot velocity |
| 22–23 | root velocity |

The trajectory is *relative*: two frames at different points of a straight walk have the same future trajectory. That
is deliberate — the trajectory says where the player wants to go, not where the character has been — and it is why the
player compares a candidate against walking **on**, not against standing still (§4).

## 3. Building the database

`MotionDatabase::build` takes any number of `MotionClip`s and samples every frame:

* the root is the clip's root joint; its yaw is unrolled across frames, so a capture that spins past 180° does not
  flip the local frame;
* feet are found by name (`LeftFoot`, `leftFoot`, …), so LAFAN1, CMU and Mixamo skeletons all work;
* velocities are central differences over ±0.1 s, clamped at the ends;
* every frame becomes a `MotionEntry` with its clip, frame number and 24 floats.

The entries are then sorted into a KD-tree: each node splits on the dimension with the widest spread, at the median,
and a query walks the near subtree first and prunes the far one when the split plane is already further away than the
current worst of the k best. With feature weights folded into the distance, 12 000 entries build in **8.7 ms** and a
query takes **4.5 µs** on this machine.

```cpp
MotionClip walk;
loadMotionBvhFile("assets/motion/walk1_subject1.bvh", &walk, &error);

std::vector<MotionClip> clips{walk, run, turn};
MotionDatabase database;
database.build(std::move(clips), MotionSettings{}, &error);
```

`MotionSettings` carries the trajectory step, the velocity window, the feature weights and the continuity price. The
defaults (trajectory 1.0, facing 0.6, feet 0.55, foot velocity 0.4, root velocity 0.5) are the ones the tests and the
reference's tuning agree on.

## 4. Choosing a frame

`MotionPlayer::update` advances the current frame at the clip's own rate, then every 0.16 s:

1. builds a query from the target velocity and heading, and from the pose of the frame the character is on **now**;
2. asks the KD-tree for eight candidates;
3. prices each candidate by weighted feature distance plus a continuity term — a jump to another clip costs more than
   a step along this one;
4. compares the winner with the cost of simply walking on for another match interval. A candidate that is not better
   by `switchMargin` is discarded, which is what keeps the walk from twitching between two equally good frames.

Step 4 compares against *walking on*, not against the current frame. The trajectory feature is relative, so the current
frame always looks like a perfect local fit for a straight walk; comparing with it would freeze the character in place.
That mistake is exactly what the first version of the playback did, and the test that walks for three seconds now
catches it.

On a jump, the player snapshots the pose it was showing, switches to the new clip and frame, and crossfades the joint
positions over 0.18 s with a smoothstep. The root rotation is slerped with the same weight.

## 5. Driving a model

`applyMotionFrame(model, clip, frame)` writes the frame into the pose arrays the rest of the engine already consumes
(`poseActive`, `poseRot`, `poseTrans`), so `updateModel` then runs IK, spring bones and the skin palette as usual.

The retarget is **direction-based**: for each joint the reader finds a humanoid node by name, computes the swing from
the model's rest bone direction to the motion's bone direction, converts that swing into the parent's space, and
multiplies it into the node's rest rotation. Bone lengths stay the model's own, so a 1.5 m rig and a 1.8 m capture
both read. It is the right retarget for locomotion and the wrong one for a hand plant: foot and hand positions are
not solved for, only directions.

## 6. Tests

`tests/motion_test.cpp` runs without a device and is built as `motion_test`; CI runs it on every push.

```sh
./build/motion_test     # 20 checks, exit code 0
```

It parses a three-joint BVH (including `End Site`), rejects a malformed one, checks that a straight walk has no
lateral trajectory and that its root-velocity feature equals the clip speed, compares the KD-tree's four nearest
against a brute-force scan over forty random queries, verifies that a 1.4 m/s query never matches a 4.2 m/s run, and
plays three seconds of walking: the character must move forward and sample one position per joint.

## 7. Limits

* **No inertialization.** Jumps crossfade positions over 0.18 s; a production matcher would blend the *velocities*
  and decay them with a spring, which removes the softness at the seam.
* **No foot locking.** The database keeps the capture's feet; there is no IK pass that plants them on slopes.
* **No tags or events.** LAFAN1 has footsteps and jumps marked in its own tooling; this reader ignores annotations.
* **The search is static.** Clips added at runtime need a rebuild (8.7 ms for 12 000 frames, so that is cheap).
* **Directional retarget only** (§5).
* **No editor page yet.** The API is in `src/engine/motion_match.h`; the animation panel still plays clip libraries.

Those are the honest gaps, and they are the order the system would grow: foot locking first, then inertialization,
then annotations.
