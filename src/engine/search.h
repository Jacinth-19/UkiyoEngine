// Finding things by name: fuzzy search and glob matching (Godot 4.8's FuzzySearch and the FileSystem dock's glob-based
// search), as engine code rather than an editor one-off.
//
// Two different questions, and they want different answers:
//
//   * "I half remember the name" — fuzzySearch. The query is split on spaces into tokens, and every token has to land
//     somewhere in the target: exactly, as a subsequence, or with a small miss budget ("health potion" finds "Potion of
//     Healing" because "health" is one miss away from the "Healing" it does contain). Results are ranked, and the
//     matching character ranges come back so a UI can highlight them.
//   * "I know the shape" — globMatch. `*.ukshader`, `assets/**/*.png`, `[a-c]?.txt`, `{Lit,Toon}.ukshader`. This is what
//     the editor's Project filter speaks, and it is the same grammar `String::matchn` grew into.
//
// The scoring is Godot's, carried over deliberately rather than invented: exact token matches are worth far more than a
// broken-up subsequence (+100), a longer run is worth more than several short ones (length squared), a match on a word
// boundary or at the start of a file name is worth more, and every missed character costs 20. That combination is why
// typing a file name finds it ahead of a file that merely contains the same letters scattered through a path.
//
// Everything here is pure C++ over UTF-8 bytes and runs on any machine, which is what lets tests/search_test.cpp pin
// the ranking without a window.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ukiyo {

// One character range a match covers: (start, length) in bytes of the target. Adjacent characters that matched in one
// run are one range, so a UI draws one underline per run rather than one per character.
struct FuzzyRange {
    int start = 0;
    int length = 0;
};

struct FuzzyResult {
    int index = 0;              // the item's position in the list that was searched
    int score = 0;              // higher is better
    std::string target;         // a copy of the item, so a caller can sort or display without the original list
    std::vector<FuzzyRange> ranges;
};

// Godot's FuzzySearch, with the same knobs and the same scoring. A default-constructed search is case-insensitive,
// allows one missed character in a token, and returns every match ranked.
class FuzzySearch {
public:
    bool caseSensitive = false;
    // Every token has to appear as an exact substring (Godot's `exact_token_matches`). Faster, and stricter: "jmp"
    // would not find "Jump".
    bool exactTokenMatches = false;
    // Characters a token may fail to find and still match, in total across the query.
    int maxMisses = 1;
    // Characters at the start of a target that never match, for callers that prepend a marker (a path root).
    int startOffset = 0;
    // Drop results far below the best one. Off by default: a caller that asks for the top few sorts and truncates
    // instead, which never hides a match that is merely unusual.
    bool filterLowScores = false;
    float filterFactor = 0.4f;
    int filterCutoff = 200;
    int maxResults = 100;

    // Ranked matches, best first. Ties are broken by the shorter target, then alphabetically, so a listing is stable
    // from one keystroke to the next.
    std::vector<FuzzyResult> search(const std::string& query, const std::vector<std::string>& items) const;

    // One target. -1 when the query does not match at all. `ranges` receives the matched character runs.
    int score(const std::string& query, const std::string& target, std::vector<FuzzyRange>* ranges = nullptr) const;
};

// Does a pattern contain glob syntax? A UI uses this to decide between "substring" and "pattern" behaviour.
bool looksLikeGlob(std::string_view pattern);

// True when `text` matches `pattern`. The grammar:
//   *      any run of characters, including '/'
//   **     the same, written for path patterns people expect to work
//   ?      exactly one character
//   [...]  one character from the set; [a-z] is a range, [!a] and [^a] are negations
//   {a,b}  either alternative; nested alternatives are allowed
//   \x     x itself, keeping any of * ? [ ] { } or a backslash literal
// An unclosed '[' or '{' is a literal character, not an error: a person typing as they go passes through `[` on the way
// to `[a-z]`, and the listing should not vanish on the way.
bool globMatch(std::string_view pattern, std::string_view text, bool caseInsensitive = true);

// The indices of every item the pattern matches, in order.
std::vector<int> globSearch(const std::vector<std::string>& items, std::string_view pattern,
                            bool caseInsensitive = true);

// The Project browser's query rule (Godot 4.8's GH-119103): a query is split on spaces into tokens and a name matches
// when *every* token does — each token either as a plain substring or, when it reads as a pattern, as a glob. So
// "hero" finds "Hero.vrm", "*.vrm hero" finds the same file with a pattern and a word, and "sword idle" needs both
// words anywhere in the name. This is what the editor's search box speaks; it is here rather than in the widget so the
// rule can be tested on its own, and so anything else that lists names can share it.
bool matchesNameTokens(std::string_view name, std::string_view query, bool caseInsensitive = true);

} // namespace ukiyo
