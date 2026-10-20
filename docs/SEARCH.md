# Asset search (item 27b)

Two ways to find an asset, and they answer different questions.

* **Fuzzy** — *"I half remember the name."* Type `potion health` and get `HealthPotion.glb` before `potion_mana.glb`,
  with the characters that matched marked so a UI can underline them.
* **Glob** — *"I know the shape."* `*.ukshader`, `assets/**/*.png`, `{Lit,Toon}.ukshader`, `[a-c]?.txt`.

Both are `src/engine/search.h` — engine code, not an editor one-off, so the hub, the Project browser and any future
asset picker share one implementation and one test. The Project browser's filter box is the first caller; it needs a
flat results model of its own (the browser lists a filesystem tree, and a ranked search is not a tree), which is a
UI change rather than an engine one and is the next step for that panel. The fuzzy ranking is Godot 4.8's `FuzzySearch` (the algorithm behind
the new FileSystem dock search, part of GH-117107's neighbourhood); the glob grammar is the `String::matchn` family
extended with the braces that path patterns want.

```
src/engine/search.h      FuzzySearch, FuzzyResult, FuzzyRange, globMatch, globSearch, looksLikeGlob
src/engine/search.cpp    the ranking, the scoring constants, the glob matcher
tests/search_test.cpp    123 checks, run in CI on Linux and Windows
```

## 1. Why the ranking is not "contains"

A `contains` filter turns useless the moment a project has two hundred files whose names share a word. The ranking is
what makes three keystrokes enough. It is Godot's scoring, kept deliberately rather than invented, because the
combination is what produces the ordering authors expect:

| Term | Points | What it buys |
| --- | --- | --- |
| a missed character | **−20** each | a typo costs, but one is survivable |
| a case-folded hit | **−3** | `Jump` ranks above `jump` when both fit |
| a run of length *n* | **+n²** | one long run beats several short ones |
| the run is after the last `/` | **×2** | a hit in the file name beats the same hit in a folder |
| the run starts or ends on a boundary (`/ \ - _ . ` or space) | **+4** | `hero` prefers `hero.glb` over `heroic.glb` |
| the run starts right after the last `/` | **+4** | the file name's first word is the strongest signal there is |
| the run covers the whole token | **+100** | an exact term cannot be beaten by a fuzzy one |
| tokens matched in query order | **+1** | a tie-breaker, not a rule: `potion health` still finds `Health potion.glb` |

Misses are a **shared budget** across the query, not a per-token allowance: `maxMisses = 1` means the query as a whole
may fail to find one character. Tokens are matched longest-first and placed without overlapping, which is what keeps
`ab` from stealing the `a` that `assets` needed.

## 2. What comes back

```cpp
ukiyo::FuzzySearch search;
search.maxResults = 50;

// items are the paths, in the order the browser lists them:
auto hits = search.search("potion health", paths);
for (const ukiyo::FuzzyResult& hit : hits) {
    paths[hit.index];        // the asset to select
    hit.score;               // for the debug overlay, if you want one
    hit.ranges;              // {(start, length), …} — the runs to underline
}
```

`FuzzyResult` carries the item's **index** into the list that was searched, so a view selects the real row rather than
matching strings. `ranges` are byte offsets in UTF-8 and adjacent characters are merged into one run: `jmp` in
`assets/anim/Jump.glb` comes back as two ranges, because `m` and `p` are adjacent and `J` is not.

A match scores at least **0**; no match is **−1**. That distinction matters: a query that used its whole miss budget is
still a match, and a sort must never place a match below a non-match.

## 3. Globs

| Pattern | Matches |
| --- | --- |
| `*` | everything, **including `/`** — typing `*.png` lists every image in the project, not only the open folder's |
| `?` | one character |
| `[a-c]`, `[!a-c]`, `[^0-9]`, `[]]` | a set, a negated set, a range, a literal `]` first |
| `{a,b}`, `{a,{b,c}}` | either alternative, nested |
| `\*` | a literal `*` (also escapes `? [ ] { } \\`) |

An unclosed `[` or `{` is a **literal**, not an error. Someone typing `[a-z]` passes through `[` on the way, and a
listing that flickers empty on that keystroke is worse than one that shows nothing useful for one frame.

`looksLikeGlob` tells a UI which of the two boxes it is looking at, so a search field can treat `*.png` as a pattern and
`terrain` as a name.

## 4. Why it is engine code

The Project browser is Qt; the hub is Qt; a game's own asset picker is neither. Putting the ranking in `src/engine`
means:

* it is **tested without a window** — 123 checks in `tests/search_test.cpp` cover ranking order, the miss budget, the
  highlight ranges, every glob construct, malformed patterns, case sensitivity, and four thousand paths in a list;
* a UI's filter box becomes a thin adapter — text in, indices out, the model rebuilt from the hits — rather than the
  only copy of the algorithm;
* a future tool — a CLI that lists a pak, a content browser in the hub — gets the same behaviour.

## 5. Scale

The search runs on every keystroke of a filter box, so it has to be cheap on a real project. `search_test.cpp` builds
4 000 paths and checks that a name at the end of the list is still found and still ranks first, that a query matching
nothing walks the list without blowing up, and that a four-segment glob over all 4 000 stays linear rather than
backtracking. `maxResults` truncates after sorting, so the cap keeps the best hits instead of the first ones.

## 6. Limits

* **No path-aware ranking beyond the last `/`.** A hit in `assets/environment/` is not scored above one in `tmp/`; the
  scoring sees only the final separator. Godot's does the same, and a project's folder layout is not something a search
  box should have to learn.
* **No pinyin, kana or accent folding.** Matching is byte-wise after ASCII case folding. Japanese asset names work as
  UTF-8 byte subsequences, which finds them but does not rank `さくら` against `サクラ`.
* **No index.** Every keystroke is a linear pass. At 100 000 assets that is a visible cost and the answer is an
  incremental index, not a faster loop.
