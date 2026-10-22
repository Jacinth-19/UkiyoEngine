# Inspector search

Flax 1.13's Properties panel search: a filter box above the Inspector that narrows a page of foldouts to the rows a
query matches. The rule is engine code (`src/engine/property_search.{h,cpp}`), so it is tested without a window; the
widget half is `editor/src/inspector_filter.{h,cpp}`, so the editor's screenshot mode draws the same panel the live one
shows.

An Inspector is a page of rows in foldouts — Transform, Lighting, Toon Extras — and once a material has sixty of them,
finding *softness* is scrolling and reading. The box above the page narrows it to the rows a query matches.

![The Inspector with a query typed](inspector-search.png)

The grammar is the Project browser's, deliberately, so the two boxes in the editor behave the same way:

- **words.** A query is split on whitespace and **every** word has to match. `shadow soft` finds Shadow Softness;
  `shadow transform` finds nothing, because no single row carries both.
- **where a word looks.** The row's label, the section and the label together (`lighting sun` works, because a person who
  half remembers types the foldout too), and the row's **value** — `0.35` finds the one field set to 0.35, `true` finds
  every toggle that is on, `#8fa8ff` finds a colour.
- **globs**, with the same grammar as `*.ukshader`: `sun*`, `*power`, `[Ss]un*`, `{Lit,Toon}*`. A pattern has to cover the
  whole label, exactly as it does in the browser, so `[Ss]un` alone finds nothing and `[Ss]un*` finds the three Sun rows.
- **negation.** A leading `-` removes what it matches: `lighting -toon` is "the lighting section, without the toon rows".
  A query of only negations (`-toon`) keeps everything else, which is the question a ninety-row panel actually raises.
- **sections.** A foldout survives only if one of its labelled rows does, and it is **opened** when it does, so a hit
  three foldouts down is visible rather than merely present. Clearing the box puts every foldout back the way it was,
  because what was open before the search is somebody's layout choice.

The rule is engine code rather than widget code, so it is tested without a window (`property_search_test`, 46 checks,
including that it agrees with the browser's `matchesNameTokens` on a plain word and on a pattern). The widget layer is a
`QLineEdit` above the page and a walk over the page's `QFormLayout` rows: label role, field role, spanning role. That is
the whole implementation, and every page gets it without knowing it exists — including pages that did not exist when it
was written.

`Ctrl+Shift+F` focuses the box from anywhere in the window; Escape clears it from inside the box; a query that matches
nothing says so in the status bar rather than leaving a blank panel unexplained. The editor self-test drives the real
widget: two sections, one row hidden, a value match, a query that matches nothing, and a clear that restores everything.

## What the three cost together

Two of them are about a hundred lines each, and the third is one field in a pipeline description. What took the time
was the part that is not code: deciding that the Inspector's search should be the *browser's* grammar rather than a
fourth variant of it, that a wireframe must not touch the fullscreen passes, and that a loading budget which hides its
overruns is worse than no budget at all.
