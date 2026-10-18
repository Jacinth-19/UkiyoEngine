# Prefab system design

Status: stages 1 to 5 are built and tested (see Status below). It follows the Unity
model where it fits and states where Ukiyo differs.

## Goals

- A prefab is a reusable hierarchy of objects saved as an asset, instanced into scenes.
- Editing a prefab updates every instance, except for properties an instance has overridden.
- Scenes stay plain JSON (`.ukscene`), and prefabs use the same JSON conventions (`.ukprefab`).
- Nothing new is needed in the renderer. An instance is just nodes with a source link.

## File format: `.ukprefab`

JSON, one root object, versioned like the other assets.

```json
{
  "format": "ukiyo-prefab",
  "version": 1,
  "name": "Crate",
  "root": {
    "id": "p0",
    "name": "Crate",
    "translation": [0, 0, 0],
    "rotationDeg": [0, 0, 0],
    "scale": [1, 1, 1],
    "components": { "mesh": { "model": "Models/Crate.glb", "node": "Crate" }, "material": { "ukmat": "Materials/Wood.ukmat" } },
    "children": []
  }
}
```

- `id` is stable and unique inside the prefab. It is never reused after deletion. It is how
  overrides find their target, so names may change without breaking instances.
- `components` holds typed data. Only the kinds listed below exist at first. Each new kind adds
  one entry to the schema, and unknown kinds are kept on round-trip so older editors do not drop them.
- `children` is the same node shape, recursively.

## Scene format change

A `.ukscene` node gains two optional keys. Nothing else changes, so existing scenes still load.

```json
{ "name": "Crate_1", "prefab": "Prefabs/Crate.ukprefab", "prefabId": "p0",
  "overrides": { "translation": [2, 0, 0], "components.material.ukmat": "Materials/Metal.ukmat" } }
```

- `prefab` links the instance to its source. `prefabId` says which prefab node this scene node
  represents; the instance root uses the prefab root's `id`.
- `overrides` is a flat map from a dotted property path to a value. A key present in `overrides`
  always wins. A key absent from it inherits from the prefab. This is the same rule Unity uses for
  property overrides, and the flat map makes diffs and merges simple.
- Child nodes of an instance are not written to the scene. They are regenerated from the prefab.
  Only added children (`"added": [...]`) and removed children (`"removed": ["p3"]`) are stored.

## Instantiation

1. Load the prefab JSON. Build nodes depth-first, assigning each a scene index.
2. Apply the scene overrides on top of the prefab values, property by property.
3. Load referenced models and materials through the existing loaders.
4. Record `prefab`, `prefabId`, and the override map on each generated node, so the editor can
   show which values are overridden.

Nested prefabs are allowed. A prefab node may reference another prefab (`"prefab": ...`). Cycles
are rejected at load time. Depth is capped at 8 to bound the work.

## Editing and applying

- **Edit an instance:** writes into the instance's `overrides`. The Inspector marks overridden
  fields in bold, as Unity does.
- **Revert:** removes the key from `overrides`. The field goes back to the prefab's value.
- **Apply:** writes the instance's overrides into the prefab file, then clears them on the
  instance. Every other instance updates on the next reload or immediately, when the prefab
  watcher fires.
- **Edit the prefab:** opens it in prefab mode, a separate view with its own hierarchy. Saving
  rewrites the `.ukprefab`, and the scene windows refresh their instances from the file watcher.

## Undo and dirty state

Every edit goes through one command object that stores the old and new values for one path. Undo
and redo replay these commands. Dirty tracking compares the scene against its last saved state, so
the existing unsaved-changes dialog covers prefab edits too.

## Editor surfaces

- Hierarchy: prefab instances show a cube icon and a blue name. Overridden values show in bold.
- Inspector: an "Overrides" section lists every overridden path with Revert and Apply buttons.
- Project view: `.ukprefab` files get a live preview, a 3D render of the root through the same
  backbone as the model previews (`renderModelThumbnail`). Drag a prefab into the Hierarchy to
  instance it.
- Create: "Create Prefab" on a scene object writes its subtree to a new `.ukprefab` and turns the
  object into an instance of it.

## Engine side

The engine loads a scene into `Model` nodes as it does now. Prefab instancing is a load-time step
that produces ordinary nodes. The renderer and the animation system do not know about prefabs.
Skinned meshes stay inside the model file they came from, so an instance of a skinned prefab
references its model rather than copying it.

## Out of scope for the first version

- Variant prefabs (a prefab of a prefab with its own overrides). The nested design above covers
  reuse, and variants can come later on the same override map.
- Per-instance animation overrides.
- Prefab-level scripts. Ukiyo has no scripting yet.

## Implementation order

1. Schema and loader for `.ukprefab`, with round-trip tests in `ukiyo_shader_tool`-style
   headless checks.
2. Scene instancing and the override map, read path first.
3. Editor: instance, override display, Apply and Revert.
4. Prefab mode, the live preview, and drag-and-drop.
5. Nested prefabs with the depth and cycle checks.

## Decision (scope: full)

The user chose the full Unity-style scope:
- Prefab assets are `.ukprefab` documents, edited in their own prefab edit mode (a multi-object
  stage separate from the open scene).
- Linked instances in scenes, with per-instance overrides (transform, material, active state).
  Apply and Revert for one instance, and Apply all and Revert all.
- Nested prefabs (a prefab may instance other prefabs), with cycle detection.
- Scene format versioning. Scenes written before the change keep loading; instance nodes and
  override tables are new fields under a bumped `version`.

Order of work: the scene format and versioning first, then linked instances with overrides,
then the prefab edit stage, then nesting and Apply/Revert all. Each step gets self-test checks.

## Status (current)

Step 1 (multi-model scene): done and tested. The engine session holds a list of instances, each with its
own model and placement (`applyPlacement` sets `Model::root` from the placement and the VRM0 turn). The
Hierarchy shows the active instance. Scenes are version 2 with `models` and `active`. A version 1 `model`
key loads as one instance. Self-test: a two-instance scene saves, reopens, and renders both instances.

Step 2 (`.ukprefab` and linked instances): done and tested. `editor/src/prefab_file.*` reads and writes
`.ukprefab` documents and keeps unknown keys. A prefab instance is stored as `prefab`, `prefabId`, and
`overrides` (only the keys it sets, with values). Keys absent from `overrides` inherit from the prefab.
Prefab menu: Instance Prefab, Edit Transform (records an override only when the value differs from the
prefab), Apply Overrides to Prefab (writes the file and moves the other linked instances on keys they do
not override), Revert Overrides. Self-test: 10 prefab checks (read back, placement, override isolation,
Apply, Revert, inherit-on-equal, scene round trip).

Step 3 (editor surfaces, stage 3): done and tested. The Hierarchy shows one row per instance. Prefab rows
have a cube icon and blue text, and are bold when they have overrides. The active instance's nodes sit under
its row. Selecting another instance row makes it active. The Inspector instance page has a Position group
(X, Y, Z) and a Yaw row, each with a Revert button that shows when the key is overridden. An Overrides list
gives Apply and Revert per key, plus Apply All and Revert All. `applyOverridesFor` and `revertOverridesFor`
take an instance index and a key list. A .ukprefab dropped on the Hierarchy instances it. Content Browser
items can be dragged. Self-test section 1d: 22 checks (markers, per-key revert, per-key apply with the
sibling following, Apply All, Revert All, row selection, drop). Full selftest: 135 passed, 0 failed.
Position and yaw labels and Revert buttons refresh when a spin box finishes editing. The Hierarchy bold
updates on every change.

Step 4 (prefab mode, preview, drop): done and tested. Prefab mode: Prefab > Edit Prefab opens a .ukprefab as its
own set of objects, with its own Hierarchy. The scene is held in a temporary snapshot until the stage exits. Save
Prefab writes each object's transform back to the node that owns it. Exit Prefab Mode restores the scene. Linked
instances follow the saved prefab on exit. Closing, opening, or creating a scene asks to save the open prefab first.
The Inspector's Edit Prefab button opens an instance's prefab. A .ukprefab's Content Browser icon is a live render
of its first model (PreviewHub announces it). Drag a prefab into the Hierarchy to instance it. In prefab mode the
same drop adds a reference to the open prefab. Create Prefab from Scene (Prefab menu) makes a multi-object prefab
from the scene's unlinked objects, and links those objects in place.

Step 5 (nesting): done and tested. A child node with a "prefab" key references another .ukprefab, placed at the
child's pose. `prefab::flatten` expands references recursively. Each node has a stable key: the ids from the top
root joined with '/'. Instances link to the top prefab with the node's key as prefabId. Cycles are refused on load
and before a reference is written. Nesting is capped at 8 levels (kMaxNestDepth). Apply on a nested object writes
to the file that owns that node (the inner prefab), so every prefab that uses it changes. Apply All and Revert All
work on every key.

Self-test: section 1e, 25 checks. Covers flattening and the composed poses (checked against glm's placement
matrix), cycle and depth refusal, instancing, Apply on top-level and nested objects, prefab mode (edit, save,
exit, sync), Create Prefab, adding a reference (cycle refused, valid one added), and the preview render.
Full selftest: 165 passed, 0 failed.

Format: a root with no mesh is valid if it has children. A child node may carry "components.mesh.model" (a model)
or "prefab" (a reference), and "translation" and "yawDeg" are relative to its parent.

Limits of this version:
- Prefab mode edits transforms only. Adding models directly in prefab mode is not built. Add them with Create Prefab
  from Scene, or add a reference with Add Prefab to Open Prefab.
- Deleting an object from a prefab is not built.
- Apply on a nested object writes to the inner prefab's file, so other prefabs that use it change too. Unity
  steers nested edits into the nested prefab. Here the Apply is allowed from a scene.
- Variant prefabs are not built.
- A prefab's preview is the render of its first model. It does not refresh when the model file changes, only when the
  prefab file changes.

Known limits of the current state:
- Only the active instance is animated. Other instances draw in their last pose.
- Rest-pose edits and per-node graph materials are saved for the active instance only. Other instances keep
  their graph materials in the engine, but their node edits are not written to the scene.
