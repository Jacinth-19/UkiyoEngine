# The Project browser: wheel zoom and glob search (item 31)

Godot 4.8's `FileSystem` dock got two quality-of-life features in
[GH-119103](https://github.com/godotengine/godot/pull/119103), and both are here, in Ukiyo's own Project browser:

* **Ctrl + the mouse wheel zooms the thumbnails.** Hover the browser, hold Ctrl, scroll: the grid grows or shrinks one
  stop per notch, and the size is remembered.
* **The search box speaks globs.** `*.vrm` lists every model, `Sword*` every asset whose name starts with Sword,
  `sword *.vrm` both at once, and `**/*.ukscene` every scene anywhere under the open folder.

![The zoom ladder and eight glob queries, computed by the engine](browser-qol.png)

The figure is `tools/browser_qol_preview.cpp`. The tiles are drawn at their real pixel sizes from
`engine/browser_zoom.h`, and every count in the lower half comes from running `engine/search.h` over the asset list
printed above it — 23 matches across the eight queries — so the picture cannot drift from the code the way a screenshot
does.

```
src/engine/browser_zoom.h|cpp   the ladder, the grid, the row icon, and which views the wheel zooms
src/engine/search.h|cpp         the query grammar: looksLikeGlob, globMatch, matchesNameTokens
editor/src/main_window.cpp      BrowserFilterModel, WheelZoomFilter, applyThumbSize / zoomBrowserThumb / saveBrowserThumb
editor/src/prefs_dialog.cpp     the same ladder in Preferences → Content Browser
tests/browser_zoom_test.cpp     1 281 checks, run in CI on Linux and Windows
tests/search_test.cpp           151 checks, including the query rule and the `**/` cases
tools/browser_qol_preview.cpp   the figure above
```

## 1. The zoom

### The ladder

| | |
| --- | --- |
| smallest thumbnail | **32 px** |
| largest | **224 px** |
| one wheel notch | **16 px** |
| opening size | **96 px** — the size the browser has always opened at |
| stops | 13 (32, 48, 64 … 224) |

The numbers are Godot's, kept deliberately: 16 px is small enough that one notch reads as a nudge and large enough that
a thumbnail never lands on a size the grid cannot lay out, and 32 is the smallest size at which the editor's generated
asset thumbnails are still recognisable. `engine/browser_zoom.h` owns them, so the slider, the wheel, the grid and the
Preferences dialog cannot disagree:

```cpp
const int v = ukiyo::thumbClamp(px);                      // onto the ladder: 97 becomes 112, 1000 becomes 224
const int icon = ukiyo::thumbIconForView(view, v);         // the thumbnail in icon view, the row icon in list view
fsList_->setGridSize(QSize(ukiyo::thumbGridWidth(v),       // icon + 24 px across
                          ukiyo::thumbGridHeight(v)));     // icon + 44 px down, for one wrapped line of the name
```

Rounding is **up** to the next stop rather than to nearest, so a size the user asked for never shrinks under their hand.
That also migrates the old preference: the editor used to allow 48–192 in steps of one, and a saved 150 now opens at 160
instead of being thrown away.

### What the wheel does

`WheelZoomFilter` sits on the icon/list view's viewport and answers the wheel **before** the view does:

| Gesture | Result |
| --- | --- |
| Ctrl + wheel up / down | the thumbnail size steps **+16 / −16**, and the event is consumed |
| Ctrl + wheel past either end | stays at 32 or 224; it does not wrap or overshoot |
| plain wheel | scrolls the grid, exactly as before |
| Shift + wheel | scrolls sideways, exactly as before |
| Ctrl + wheel in the details view | scrolls: the details view is a table, and a table's rows are text, not pictures |

The modified event is the only one taken. Anything the filter does not claim goes to `QListView::wheelEvent` untouched,
which is why the ordinary scroll behaviour did not have to be re-implemented — and why a trackpad's smooth pixel scroll
(an event with a `pixelDelta` but no `angleDelta`) is left alone rather than being read as a burst of notches.

In list view the row icon scales with the same ladder but at a fifth of the size — 20 px at the default 96, which is
exactly what list view drew before zoom existed — held between 14 and 48 px so rows stay text-first at both ends.

### Saving

Dragging the slider now saves too, which it did not before: the value was read from `browser/thumb` on startup and then
never written back. Both paths call `applyThumbSize`, which starts a **1.5 s single-shot timer**; when the timer fires,
`saveBrowserThumb` writes the size to `browser/thumb`. A scroll of nine notches is one write, not nine — the same
debounce Godot's dock uses. `applyThumbSize` also moves the slider itself onto the snapped value under a
`QSignalBlocker`, so the two can never hold different numbers.

## 2. The search

### The rule

The box holds a **query**, which is split on spaces into **tokens**. An asset is listed when **every** token matches it:

| A token that… | matches like | example |
| --- | --- | --- |
| has no wildcard | a **case-insensitive substring** — the old behaviour, kept | `hero` finds `Hero.vrm` |
| contains `* ? [ ] { } \` | a **glob against the whole path** | `*.vrm`, `s?ord.png`, `[A-C]*`, `*{vrm,ukscene}` |

The subject of a token is the path **relative to the folder the browser is showing**, not just the file name. That is
what makes `props/*.png` work, and it means typing a folder's name lists what is inside it — the `swords` row of the
figure finds `Swords/Blade.vrm` as well as the files named Sword.

An empty (or spaces-only) query matches everything, so clearing the box restores the folder. The **type button** (All /
Models / Textures / Audio / Folders) narrows on top of the query and keeps its own extension lists, now in one place:
`BrowserFilterModel::extensionAllowed`.

### The examples, measured

Eight assets: `Hero.vrm`, `Brass Sword.vrm`, `Sword Idle.ukscene`, `Sword Run.ukscene`, `Sword.png`,
`Swords/Blade.vrm`, `Swords/Blade.ukscene`, `readme.txt`.

| Query | Keeps | Why |
| --- | --- | --- |
| `sword` | **6** | a word is a substring, and it is found in the folder name too |
| `*.vrm` | **3** | one type of file, anywhere in the folder |
| `sword *.vrm` | **2** | both tokens have to match |
| `s?ord.png` | **1** | one character of any kind |
| `*{vrm,ukscene}` | **6** | a brace set picks between endings |
| `**/*.ukscene` | **3** | here, or in any folder below |
| `hero.vrm` | **1** | a dot is a literal, not "any character" |
| `*.png sword` | **1** | order does not matter |

Those counts are not a claim in prose: `browser_qol_preview` runs the queries through `matchesNameTokens` and prints the
names it keeps, in CI, every build.

### Why a proxy, and not the model's name filters

The browser used to push a wildcard list into `QFileSystemModel::setNameFilters`. Qt's wildcards cannot express what a
glob box needs — no `{}` alternatives, no `**/`, no "this token or that token" per word, and no way to say "match the
path" — and a second, almost-the-same pattern dialect in the editor is exactly the kind of drift this codebase avoids.
So the grid and the details view now look through `BrowserFilterModel`, a `QSortFilterProxyModel` that calls
`ukiyo::matchesNameTokens` — **the same function the tests exercise**, one grammar, one implementation. The folder tree
keeps the plain folder model: it is navigation, not a place to search.

Two details that make the proxy behave:

* **the folder being viewed is never filtered away.** A view whose root row is hidden would show nothing at all, so a
  query like `*.vrm` would make the browser look empty rather than filtered.
* **everything that maps a path goes through one pair of helpers** — `browserPathAt` / `browserIndex` — so the context
  menu, F2/Delete, drag-and-drop, "show in the Console" and the preview hub all keep working on a filtered grid. A path
  the current query hides simply maps to an invalid index, which is the right answer.

### One addition to the grammar: `**/`

`globMatch` already treated `*` as crossing `/` (so `*.png` finds an image in any folder). `**/` now also stands for
**no directory at all**, which is what every shell does and what a path pattern needs to be useful:

```
**/idle.png   matches  idle.png, Anims/idle.png, Anims/Idle/idle.png
anim/**/*.png matches  anim/idle.png and anim/walk/idle.png, but not anims/idle.png
```

## 3. Tests

| Target | What it pins | Checks |
| --- | --- | --- |
| `browser_zoom_test` | the ladder, clamping, ±16 stepping, saturation at both ends, round trips, the grid arithmetic, the row icon's bounds, which views zoom | **1 281** |
| `search_test` | the whole glob grammar, `**/`, the token rule (substring vs glob, every token required, empty query), fuzzy ranking, scale | **151** |
| `ukiyo_editor --selftest` | the wiring: real `QWheelEvent`s sent to the viewport, the slider, the grid size, list-mode icons, the details view refusing to zoom, and eight glob queries against a folder of files the test creates | **21** browser checks |

`browser_zoom_test` is pure C++ — no Qt, no window, no GPU — so it runs anywhere the compiler does. The editor's own
self-test is the one that needs a window, and it drives the real widgets: the zoom checks send `QCoreApplication::sendEvent`
with a `QWheelEvent` built the way a mouse builds one, so the event filter is under test rather than bypassed.

## 4. Android, Windows, Linux

The two rules live in the engine and include no Qt and no graphics header, so the CI matrix builds and runs them
everywhere:

| Job | What it does with this feature |
| --- | --- |
| **Linux (GCC)** | builds `browser_zoom_test` and `search_test` and runs both, then renders `docs/browser-qol.png` |
| **Windows (MSVC)** | builds the same two tests into the same array as every other test, plus the editor, so `main_window.cpp` is compiled by a second compiler |
| **Android (NDK)** | configures the repository with the NDK toolchain for **arm64-v8a** and **x86_64** and builds `search_test` and `browser_zoom_test` for both, then checks the binaries really are AArch64 / x86-64 ELF files linked against Android's loader |

The Android job exists because "also build it for Android" has to mean something. There is no Android application in this
repository yet and no NDK build of Qt, so what the phone build compiles is the **portable core** — the query grammar and
the zoom ladder — with the NDK's own compiler, C library and ABI. It deliberately does not pretend to build the Qt
editor: `CMakeLists.txt` returns before the desktop targets when `ANDROID` is set, and the job's comment says so.
Locally:

```sh
cmake -B build-android-arm64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
cmake --build build-android-arm64
```

## 5. Limits

* **The browser box ranks nothing.** It filters, in file-system order. The fuzzy ranking in `engine/search.h` (docs/SEARCH.md)
  is still there and still tested; giving the box a ranked results list is a different UI — a flat list of hits rather
  than a folder view — and is not part of this change.
* **Case folding is ASCII.** `Sword` matches `sword` and `SWORD`, and UTF-8 names match as bytes; `さくら` will not match
  `サクラ`.
* **A folder is filtered as one level.** The query is matched against each row's path relative to the open folder; it
  does not descend to list matches from subfolders (`**/` matches paths that are already visible, one level deep).
* **The details view does not zoom.** Its rows are columns of text, and the slider is disabled there, as before.
