#include "engine/property_search.h"

#include "engine/search.h"

#include <algorithm>

namespace ukiyo {
namespace propsearch {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool containsInsensitive(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t at = 0; at + needle.size() <= hay.size(); ++at) {
        bool same = true;
        for (size_t k = 0; k < needle.size() && same; ++k) same = lower(hay[at + k]) == lower(needle[k]);
        if (same) return true;
    }
    return false;
}

// A word against one string: a pattern when it reads as one, a substring otherwise. Both case-insensitive, which is
// what a person typing into an inspector expects.
bool wordAgainst(std::string_view word, std::string_view text) {
    if (text.empty()) return false;
    if (looksLikeGlob(word)) return globMatch(word, text, true);
    return containsInsensitive(text, word);
}

bool excluded(const Field& f, std::string_view word, bool matchValue);

// The row as the words see it: its own label, "section label" so a word can straddle the two, and its value.
bool rowMatches(const Field& f, std::string_view word, bool matchValue) {
    if (wordAgainst(word, f.label)) return true;
    if (!f.section.empty()) {
        std::string both = f.section;
        both += ' ';
        both += f.label;
        if (wordAgainst(word, both)) return true;
    }
    if (matchValue && !f.value.empty() && wordAgainst(word, f.value)) return true;
    return false;
}

bool excluded(const Field& f, std::string_view word, bool matchValue) {
    return word.size() > 1 && word[0] == '-' && rowMatches(f, word.substr(1), matchValue);
}

} // namespace

std::vector<std::string> words(std::string_view query) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && isSpace(query[i])) ++i;
        const size_t start = i;
        while (i < query.size() && !isSpace(query[i])) ++i;
        if (i > start) out.emplace_back(query.substr(start, i - start));
    }
    return out;
}

bool matches(const Field& field, std::string_view query, bool matchValue) {
    const std::vector<std::string> ws = words(query);
    if (ws.empty()) return true;
    bool anyPositive = false;
    for (const std::string& w : ws) {
        if (!w.empty() && w[0] == '-') {
            if (excluded(field, w, matchValue)) return false;
            continue;
        }
        anyPositive = true;
        if (!rowMatches(field, w, matchValue)) return false;
    }
    // A query of only negations ("-toon") excludes what it matches and keeps the rest, rather than matching nothing.
    (void)anyPositive;
    return true;
}

Filter filter(const std::vector<Field>& fields, std::string_view query, bool matchValue) {
    Filter out;
    out.visible.assign(fields.size(), false);
    const std::vector<std::string> ws = words(query);
    out.active = !ws.empty();

    auto blank = [&](size_t i) { return fields[i].label.empty() && fields[i].value.empty(); };

    if (!out.active) {
        std::vector<std::string> seen;
        for (size_t i = 0; i < fields.size(); ++i) {
            out.visible[i] = true;
            if (!blank(i)) ++out.shown;
            const std::string& s = fields[i].section;
            if (std::find(seen.begin(), seen.end(), s) == seen.end()) {
                seen.push_back(s);
                out.sections.push_back(i);
            }
        }
        return out;
    }

    // Which rows match on their own. Separators are decided afterwards, by their section.
    std::vector<bool> keep(fields.size(), false);
    for (size_t i = 0; i < fields.size(); ++i)
        if (!blank(i)) {
            keep[i] = matches(fields[i], query, matchValue);
            if (keep[i]) ++out.shown;
        }

    // A section survives when at least one of its labelled rows does. `sections` holds the first surviving index of
    // each, in the order the caller laid the page out, so a widget layer can expand them in a stable order.
    std::vector<std::string> alive;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (!keep[i]) continue;
        const std::string& s = fields[i].section;
        if (std::find(alive.begin(), alive.end(), s) == alive.end()) {
            alive.push_back(s);
            out.sections.push_back(i);
        }
    }

    // Now the visibility of every row: matched rows show, and so do the separators of a surviving section. A separator
    // in a section with no matches hides with it.
    for (size_t i = 0; i < fields.size(); ++i) {
        if (!blank(i)) {
            out.visible[i] = keep[i];
        } else {
            out.visible[i] = std::find(alive.begin(), alive.end(), fields[i].section) != alive.end();
        }
    }
    return out;
}

} // namespace propsearch
} // namespace ukiyo
