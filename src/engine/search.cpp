#include "engine/search.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

namespace ukiyo {

namespace {

// Godot's boundary set for a "word start": path separators, a dash, a dot, an underscore and a space.
bool isBoundary(const std::string& s, int i) {
    if (i < 0 || i >= static_cast<int>(s.size())) return true;
    switch (s[static_cast<size_t>(i)]) {
    case '/': case '\\': case '-': case '_': case '.': case ' ': return true;
    default: return false;
    }
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = lower(c);
    return out;
}

struct Token {
    int index = 0;          // position in the query, for the order bonus
    std::string raw;        // as typed, for the case-exactness bonus
    std::string text;       // lower-cased when the search is case-insensitive
};

struct TokenMatch {
    int token = -1;
    int tokenLength = 0;
    int matched = 0;
    int score = 0;
    int lo = -1, hi = -1;   // inclusive range the match covers; (-1, -1) when the token matched with only misses
    int caseDiffers = 0;    // characters matched with different case than the query typed them
    std::vector<FuzzyRange> ranges;

    int misses() const { return tokenLength - matched; }
    bool valid() const { return lo >= 0 && hi >= lo; }
};

} // namespace

// ---------------------------------------------------------------- fuzzy

std::vector<FuzzyResult> FuzzySearch::search(const std::string& query, const std::vector<std::string>& items) const {
    std::vector<FuzzyResult> out;
    for (size_t i = 0; i < items.size(); ++i) {
        std::vector<FuzzyRange> ranges;
        const int s = score(query, items[i], &ranges);
        if (s < 0) continue;
        out.push_back({static_cast<int>(i), s, items[i], ranges});
    }

    std::stable_sort(out.begin(), out.end(), [](const FuzzyResult& a, const FuzzyResult& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.target.size() != b.target.size()) return a.target.size() < b.target.size();
        return a.target < b.target;
    });

    if (filterLowScores && !out.empty()) {
        float average = 0.0f, best = 0.0f;
        for (const FuzzyResult& r : out) {
            average += static_cast<float>(r.score);
            best = std::max(best, static_cast<float>(r.score));
        }
        average /= static_cast<float>(out.size());
        const float cutoff = std::min(static_cast<float>(filterCutoff), average + (best - average) * filterFactor);
        out.erase(std::remove_if(out.begin(), out.end(), [cutoff](const FuzzyResult& r) {
                      return static_cast<float>(r.score) < cutoff;
                  }),
                  out.end());
    }
    if (maxResults > 0 && out.size() > static_cast<size_t>(maxResults)) out.resize(static_cast<size_t>(maxResults));
    return out;
}

int FuzzySearch::score(const std::string& query, const std::string& target, std::vector<FuzzyRange>* rangesOut) const {
    // Split on spaces, as Godot does. An empty token (two spaces, or a query that is all spaces) is dropped: it would
    // otherwise match everywhere and drown the real terms.
    std::vector<Token> tokens;
    {
        size_t begin = 0;
        while (begin <= query.size()) {
            size_t end = query.find(' ', begin);
            if (end == std::string::npos) end = query.size();
            if (end > begin)
                tokens.push_back({static_cast<int>(tokens.size()), query.substr(begin, end - begin),
                                  caseSensitive ? query.substr(begin, end - begin) : lowered(query.substr(begin, end - begin))});
            begin = end + 1;
        }
    }
    // No query matches everything: an editor's empty search box shows the whole list, not nothing.
    if (tokens.empty()) return 0;
    // Longest tokens first: they are the hardest to place, and a short token that takes the only place a long one could
    // have used would make the result worse than the opposite order.
    std::stable_sort(tokens.begin(), tokens.end(), [](const Token& a, const Token& b) {
        if (a.text.size() != b.text.size()) return a.text.size() > b.text.size();
        return a.index < b.index;
    });

    const std::string adjusted = caseSensitive ? target : lowered(target);
    const int dirIndex = static_cast<int>(target.rfind('/'));
    int missBudget = maxMisses;
    int total = 0;
    int matchLo = -1, matchHi = -1;
    std::vector<TokenMatch> chosen;
    std::vector<FuzzyRange> allRanges;

    for (const Token& token : tokens) {
        TokenMatch best;
        int offset = startOffset;
        while (offset <= static_cast<int>(adjusted.size())) {
            TokenMatch m;
            m.token = token.index;
            m.tokenLength = static_cast<int>(token.text.size());
            int runStart = -1, runLen = 0;
            int misses = 0;
            int cursor = offset;
            bool ok = true;
            const auto flushRun = [&] {
                if (runStart < 0) return;
                m.ranges.push_back({runStart, runLen});
                m.matched += runLen;
                if (m.lo < 0 || runStart < m.lo) m.lo = runStart;
                if (runStart + runLen - 1 > m.hi) m.hi = runStart + runLen - 1;
                runStart = -1;
                runLen = 0;
            };
            for (size_t k = 0; k < token.text.size(); ++k) {
                const char c = token.text[k];
                size_t at = adjusted.find(c, static_cast<size_t>(cursor));
                if (at == std::string::npos) {
                    // The rest of the token is not in the target from here. One miss is allowed per character (Godot's
                    // budget), which is what lets "healt" find "Healing".
                    if (++misses > missBudget) {
                        ok = false;
                        break;
                    }
                    flushRun();
                    continue;
                }
                const int pos = static_cast<int>(at);
                if (!caseSensitive && target[static_cast<size_t>(pos)] != token.raw[k]) ++m.caseDiffers;
                if (runStart >= 0 && pos == cursor) {
                    ++runLen;
                } else {
                    flushRun();
                    runStart = pos;
                    runLen = 1;
                }
                cursor = pos + 1;
            }
            flushRun();
            if (!ok) break;

            // Does it collide with what the other tokens already took? Overlapping ranges would highlight one character
            // twice and make the score say the query matched more than it did.
            bool clashes = false;
            if (m.valid()) {
                for (const TokenMatch& t : chosen) {
                    if (!t.valid()) continue;
                    if (m.lo <= t.hi && t.lo <= m.hi) {
                        clashes = true;
                        break;
                    }
                }
            }
            if (!clashes && m.misses() <= missBudget) {
                m.score = -20 * m.misses();
                // A hit that had to be case-folded costs 3, which is enough to put "Jump" above "jump" when both fit.
                if (m.caseDiffers > 0) m.score -= 3;
                for (const FuzzyRange& r : m.ranges) {
                    int substringScore = r.length * r.length;
                    if (r.start > dirIndex) substringScore *= 2;   // a hit in the file name beats one in the folder
                    if (isBoundary(target, r.start - 1) || isBoundary(target, r.start + r.length)) substringScore += 4;
                    if (r.start - 1 == dirIndex) substringScore += 4;
                    if (r.length == m.tokenLength) substringScore += 100;   // the whole token, found whole
                    m.score += substringScore;
                }
                if (best.token < 0 || m.score > best.score) best = m;
            }
            if (!m.valid()) break;
            offset = m.lo + 1;
        }
        if (best.token < 0) return -1;
        total += best.score;
        missBudget -= best.misses();
        for (const FuzzyRange& r : best.ranges) allRanges.push_back(r);
        if (best.valid()) {
            if (matchLo < 0 || best.lo < matchLo) matchLo = best.lo;
            if (best.hi > matchHi) matchHi = best.hi;
        }
        chosen.push_back(best);
    }

    // Tokens in query order score one more than tokens found out of order. It is a tie-breaker, not a rule: "potion
    // health" still finds "Greater Health Potion".
    if (chosen.size() > 1) {
        std::vector<int> starts(tokens.size(), -1);
        for (const TokenMatch& m : chosen) starts[static_cast<size_t>(m.token)] = m.valid() ? m.lo : -1;
        bool ordered = true;
        for (size_t i = 1; i < starts.size(); ++i)
            if (starts[i] >= 0 && starts[i - 1] >= 0 && starts[i - 1] > starts[i]) ordered = false;
        if (ordered) total += 1;
    }

    // A match is never reported as -1, however many misses it took: -1 is the "no match" answer and a caller that
    // sorts by score must not see a match sort below a non-match.
    total = std::max(total, 0);

    if (rangesOut) {
        std::sort(allRanges.begin(), allRanges.end(),
                  [](const FuzzyRange& a, const FuzzyRange& b) { return a.start < b.start; });
        *rangesOut = allRanges;
    }
    return total;
}

// ---------------------------------------------------------------- glob

bool looksLikeGlob(std::string_view pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\') {
            ++i;   // an escape is literal
            continue;
        }
        if (c == '*' || c == '?' || c == '[' || c == '{') return true;
    }
    return false;
}

namespace {

class Glob {
public:
    Glob(std::string_view pattern, bool caseInsensitive) : p_(pattern), ci_(caseInsensitive) {}

    bool match(std::string_view text) {
        std::vector<std::pair<int, int>> stack{{0, 0}};
        while (!stack.empty()) {
            auto [pi, ti] = stack.back();
            stack.pop_back();
            if (step(pi, ti, text, &stack)) return true;
        }
        return false;
    }

private:
    static char fold(char c) { return std::tolower(static_cast<unsigned char>(c)); }

    bool eq(char a, char b) const { return ci_ ? fold(a) == fold(b) : a == b; }

    // One step from (pattern index, text index): either the whole pattern is consumed (done), a wildcard consumes text
    // and is retried, or a literal consumes one character of both.
    bool step(int pi, int ti, std::string_view text, std::vector<std::pair<int, int>>* stack) {
        // Collapse runs of '*': "**" over "***" is one wildcard, and a path pattern can say either.
        while (pi < static_cast<int>(p_.size()) && p_[pi] == '*' && pi + 1 < static_cast<int>(p_.size()) &&
               p_[pi + 1] == '*')
            ++pi;
        if (pi >= static_cast<int>(p_.size())) return ti >= static_cast<int>(text.size());
        const char c = p_[pi];

        if (c == '*') {
            // Greedy would backtrack quadratically on a long path; pushing "stay here, consume one more" keeps it
            // linear in the number of (pattern, text) pairs.
            for (int k = static_cast<int>(text.size()); k >= ti; --k) stack->push_back({pi + 1, k});
            return false;
        }
        if (ti >= static_cast<int>(text.size())) return false;

        if (c == '?') {
            stack->push_back({pi + 1, ti + 1});
            return false;
        }
        if (c == '[') {
            int end = pi + 1;
            bool negate = false;
            if (end < static_cast<int>(p_.size()) && (p_[end] == '!' || p_[end] == '^')) {
                negate = true;
                ++end;
            }
            // A ']' first in the set is a literal, as in []].
            int scan = end;
            if (scan < static_cast<int>(p_.size()) && p_[scan] == ']') ++scan;
            while (scan < static_cast<int>(p_.size()) && p_[scan] != ']') ++scan;
            if (scan >= static_cast<int>(p_.size())) {
                // No closing bracket: literal, so a half-typed pattern still lists files.
                if (eq(c, text[static_cast<size_t>(ti)])) stack->push_back({pi + 1, ti + 1});
                return false;
            }
            bool inSet = false;
            int i = end;
            while (i < scan) {
                if (i + 2 < scan && p_[i + 1] == '-') {
                    char lo = p_[i], hi = p_[i + 2];
                    const char t = text[static_cast<size_t>(ti)];
                    const bool big = ci_ ? fold(t) >= fold(lo) && fold(t) <= fold(hi) : t >= lo && t <= hi;
                    if (big) inSet = true;
                    i += 3;
                } else {
                    if (eq(p_[i], text[static_cast<size_t>(ti)])) inSet = true;
                    ++i;
                }
            }
            if (inSet != negate) stack->push_back({scan + 1, ti + 1});
            return false;
        }
        if (c == '{') {
            // The group stands for one of its alternatives followed by whatever came after the closing brace. Each
            // alternative is tried by matching that rewritten suffix at the current position, so a wildcard or a
            // literal before the brace keeps the alignment it already earned.
            int depth = 0, end = pi;
            while (end < static_cast<int>(p_.size())) {
                if (p_[end] == '{') ++depth;
                else if (p_[end] == '}') {
                    if (--depth == 0) break;
                }
                ++end;
            }
            if (end >= static_cast<int>(p_.size())) {
                if (eq(c, text[static_cast<size_t>(ti)])) stack->push_back({pi + 1, ti + 1});
                return false;
            }
            // Split the inside at top-level commas, then rewrite the pattern for each alternative. The rewrite is
            // simpler than carrying a list of active spans, and a pattern is short.
            std::vector<std::string> alternatives;
            std::string current;
            int d = 0;
            for (int i = pi + 1; i < end; ++i) {
                const char ch = p_[i];
                if (ch == '{') ++d;
                if (ch == '}') --d;
                if (ch == ',' && d == 0) {
                    alternatives.push_back(current);
                    current.clear();
                    continue;
                }
                current += ch;
            }
            alternatives.push_back(current);
            for (const std::string& alt : alternatives) {
                // Only the suffix is rewritten: the prefix has already been matched against text up to `ti`.
                const std::string suffix = alt + std::string(p_.substr(static_cast<size_t>(end + 1)));
                if (Glob(suffix, ci_).match(text.substr(static_cast<size_t>(ti)))) return true;
            }
            return false;
        }
        if (c == '\\' && pi + 1 < static_cast<int>(p_.size())) {
            if (eq(p_[pi + 1], text[static_cast<size_t>(ti)])) stack->push_back({pi + 2, ti + 1});
            return false;
        }
        if (eq(c, text[static_cast<size_t>(ti)])) stack->push_back({pi + 1, ti + 1});
        return false;
    }

    std::string_view p_;
    bool ci_ = true;
};

} // namespace

bool globMatch(std::string_view pattern, std::string_view text, bool caseInsensitive) {
    return Glob(pattern, caseInsensitive).match(text);
}

std::vector<int> globSearch(const std::vector<std::string>& items, std::string_view pattern, bool caseInsensitive) {
    std::vector<int> out;
    for (size_t i = 0; i < items.size(); ++i)
        if (globMatch(pattern, items[i], caseInsensitive)) out.push_back(static_cast<int>(i));
    return out;
}

} // namespace ukiyo
