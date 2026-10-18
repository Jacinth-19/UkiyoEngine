# Unity Asset Store editor packages: what they change in the panels

Research date: 2026-10-10. Scope: Hierarchy, Inspector, Console, Project browser.
Screenshots are in `research/images/`. Several screenshots came from third-party pages, not official store pages, so each one is marked with how well it matches its package.

## 1. Hierarchy

### Hierarchy Plus (Kinwailo)
- Store: https://assetstore.unity.com/packages/tools/utilities/hierarchy-plus-97887
- Forum thread (features): https://forum.unity.com/threads/hierarchy-plus-expands-the-hierarchy-window-with-many-essential-features.495317
- Discussions thread: https://discussions.unity.com/t/hierarchy-plus-expands-the-hierarchy-window-with-many-essential-features/677020
- Store page: latest version 1.9i, released Jan 11, 2019. A store review says it is no longer maintained for Unity 2019.1 and later.
- Features listed by the developer:
  - Custom icon, icon tint, highlight, text colour, and bold for each GameObject label.
  - Row shading and separators, colourised tree lines, level highlight, main component shown as an icon, child count.
  - Click the icon to toggle a GameObject. "Back to parent" collapses the tree to the selected parent.
  - Function buttons: components and scripts, tag, layer, sorting layer. Quick state toggles: selectable, non-editable, favourite, selection base, hierarchy base, static flags, prefab state, gizmo icon, missing references, log messages.
  - Topbar: recent GameObjects, recent scenes, save/load selection, favourites, notes, quick settings.
  - Right-click the icon for an all-in-one menu. Batch actions on children or selection. Component copy and paste.
  - Scene View highlight: hovering a row highlights the object in the Scene view.
- Screenshot: `research/images/hierarchy_plus_community_hierarchy.png`. It is a Reddit post about a similar hierarchy tool called "Hierarchy Easy", not an official Hierarchy Plus screenshot.

### Extended Hierarchy (GameCodeAssets)
- Store: https://assetstore.unity.com/packages/tools/gui/extended-hierarchy-305323
- Version 1.0.1, released Feb 20, 2025. Works with Built-in, URP, and HDRP. Reviews say it groups items and colour-codes them.

### vHierarchy 2 (kubacho lab)
- Store: https://assetstore.unity.com/packages/tools/utilities/vhierarchy-2-253397
- Version 2.1.11, released Sep 2, 2026. Adds custom icons, separators, rainbow colours, folder and shortcut tools. A review warns that some default shortcuts delete or deactivate objects when a key is pressed by accident. Our editor should not bind destructive keys to hover.

### Unity default hierarchy (baseline)
- Screenshot: `research/images/unity_default_hierarchy_docs.png` (Unity 2021.3 docs).

## 2. Inspector

### Odin Inspector and Serializer (Sirenix)
- Store: https://assetstore.unity.com/packages/tools/utilities/odin-inspector-and-serializer-89041
- Site: https://odininspector.com/pricing (Validator add-on €40.00, per-seat licence)
- Latest version listed: 4.0.2.4.
- Features (from the Odin product text):
  - Serializes dictionaries, interfaces, and polymorphic types without writing a custom editor.
  - Powerful lists: drag and drop, insert and delete items, multi-dimensional arrays, tables, paging, cross-list dragging.
  - Attributes for value dropdowns, colour palettes, inline editors, asset lists, button groups, validation.
  - Editor-only mode, custom editor windows, customisable layouts.
  - Input validation for scenes and assets.
- Screenshots:
  - `research/images/odin_inspector_github_example.png`: GitHub project screenshot, not from the Odin store page.
  - `research/images/odin_inspector_game_manager.png`: blog post on a Game Manager built with Odin.

### What to adopt for our Inspector
- Fold-out sections with a header that can be dragged.
- Table and list drawing for arrays with add, remove, and reorder.
- Dropdowns for enums and asset references, with object pickers.

## 3. Console

### Editor Console Pro (FlyingWorm)
- Store listing (via mirrors): free-game-assets.com and gfx-hub.co list version 3.984 (15 Sep 2026). Use the official store page for the current price.
- Features from the developer description:
  - Search and filter by text, file name, class, and GameObject name. Regular expressions supported.
  - Custom filter groups with their own colours, icons, and toggle buttons.
  - Columns for timestamp, file, namespace, GameObject, class, method, frame, and filter.
  - Double-click opens the code editor at the right method. Source preview around a stack frame.
  - Collapse duplicate messages, including variables that change content.
  - Colourised log lines, custom fonts and colours, copy full stack traces, export to .txt, remote logging from builds.
  - Ignore individual logs. Parse Editor.log.
- Screenshots:
  - `research/images/editor_console_pro_reddit.jpg`: Reddit post for the asset. Small image.
  - `research/images/editor_console_pro_pinterest.jpg`: Pinterest pin for the store listing.
- A smaller free clone, "ConsolePro" by ted10401 on GitHub, adds a search filter and a resizable detail panel. Its title bar has Clear, Collapse, Clear on Play, Error Pause, Lock to Bottom, Search, and Show Logs/Warnings/Errors toggles.

### What to adopt for our Console
- Search box, Collapse, Error Pause, Lock to Bottom, and per-type toggles with counts in the toolbar.
- Resizable split between the log list and a detail pane that shows the full stack trace.
- Double-click a stack frame to open it in the code editor.

## 4. Project browser

### Rainbow Folders (Borodar)
- Source and docs: https://github.com/Borod4r/unity3d-rainbow-folders
- Forum: https://forum.unity.com/threads/rainbow-folders-custom-folder-icons-in-the-project-browser-easy.411902/
- Features: custom icons and backgrounds for any folder. Alt-click a folder to open the configuration dialog. Small icon for the tree and large icon for the grid. Apply to subfolders. Row shading and tree outlines. More than 70 icons. Right-click menu presets for colour, tag, type, and platform.
- Screenshots:
  - `research/images/rainbow_folders_github.png`: small banner, not a full screenshot.
  - `research/images/rainbow_folders_2_store.jpg`: Rainbow Folders 2 store screenshot via gameassetdeals.com.

### Neon Folders (unity_assetstore.gitlab.io)
- Presets are stacks of textures with an offset, scale, and tint. Rules match folders by name, path, or glob. Default rulesets cover Animations, Meshes, Scripts, Textures, and platform folders.

### SimpleFolderIcon (SeaeeesSan, GitHub)
- Folder icons are chosen by folder name. Drop a PNG named after the folder into the icon directory.

### What to adopt for our Project browser
- Folder icons chosen by name rule: Scripts, Textures, Materials, Models, Prefabs, Scenes, Shaders, Animations, and Audio, each with its own colour.
- Alt-click or context-menu icon picker, stored per folder.
- Row shading and a separator option.

## 5. Summary table

| Panel | Package | Main change to the Unity UI | Screenshot |
|---|---|---|---|
| Hierarchy | Hierarchy Plus | Icons, colours, row shading, topbar, batch actions | `hierarchy_plus_community_hierarchy.png` (similar tool) |
| Hierarchy | vHierarchy 2 | Separators, icons, rainbow colours | none saved |
| Inspector | Odin Inspector | Dropdowns, tables, dictionaries, validation | `odin_inspector_github_example.png`, `odin_inspector_game_manager.png` |
| Console | Editor Console Pro | Filter groups, columns, stack source preview | `editor_console_pro_reddit.jpg`, `editor_console_pro_pinterest.jpg` |
| Project | Rainbow Folders | Custom folder icons and backgrounds | `rainbow_folders_2_store.jpg` |
| Project | Neon Folders | Rule-based folder icons | none saved |

## Caveats
- The screenshots are small and some come from third-party pages. Use the store pages above to confirm the current look.
- Store prices and version numbers come from mirror sites and the store page text. They can change.
