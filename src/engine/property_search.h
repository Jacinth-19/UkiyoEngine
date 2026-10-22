// Filtering an Inspector by name (Flax 1.13's Properties panel search).
//
// An Inspector is a list of rows in named sections — Transform > Position, Lighting > Sun Angle, Toon Extras > Ramp
// Offset. Once a material has sixty rows, finding "softness" means scrolling and reading, which is the part of an editor
// that has nothing to do with the engine and everything to do with how long a change takes. This is the rule Flax's
// search box applies, as engine code so it can be tested without a window.
//
// The grammar is the Project browser's, deliberately: a query is split on whitespace, and **every** word has to match.
// A word matches a field when it matches the row's label, the section and label together ("lighting sun"), or the
// row's current value, and it may be written as a glob (`sun*`, `*angle`, `[Ss]un`) with the same grammar
// search.h uses. A word prefixed with `-` hides what it matches, so `lighting -toon` is "the lighting section, without
// the toon rows".
//
// Matching the value matters more than it sounds: "0.35" finds the one field that is set to 0.35, and "true" finds
// every toggle that is on. A field with no label and no value (a separator, a spacer) is never hidden by itself — it
// belongs to whatever section it sits in — and never counts as a match.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ukiyo {
namespace propsearch {

// One row of an Inspector, as the widget layer can describe it.
struct Field {
    std::string section;   // the foldout header: "Lighting"
    std::string label;     // the row's label: "Sun Angle"
    std::string value;     // the row's value as text: "0.35", "On", "Lit"
};

// The answer for one query: which rows stay, and which sections still have something in them.
struct Filter {
    std::vector<bool> visible;      // one per input field, in the order they were given
    std::vector<size_t> sections;   // indices into the input, first occurrence order, of sections with a visible field
    size_t shown = 0;               // fields left visible that had a label or a value
    bool active = false;            // the query had at least one word, so the caller should collapse and expand
};

// Applies `query` to `fields`. `matchValue` lets a caller turn off matching against values (a locked Inspector's fields
// are still readable, so it stays on there; a caller that rebuilds values asynchronously can turn it off).
Filter filter(const std::vector<Field>& fields, std::string_view query, bool matchValue = true);

// Does one row match, on its own? The same rule `filter` uses, for a caller that only has one row in hand.
bool matches(const Field& field, std::string_view query, bool matchValue = true);

// Splits a query into its words, dropping the ones that are only whitespace. Exposed so a UI can label its box ("3
// words") and so the tests can talk about words rather than strings.
std::vector<std::string> words(std::string_view query);

} // namespace propsearch
} // namespace ukiyo
