// The Inspector's search rule (Flax 1.13's Properties panel search): one query, a page of labelled rows, which rows stay
// and which sections still have something in them. Everything here is the rule itself, with the rows an editor would
// hand it — no window, no widget, no assets.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/property_search.h"
#include "engine/search.h"

using ukiyo::propsearch::Field;
using ukiyo::propsearch::Filter;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = "") {
    ++g_checks;
    std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_failures;
}

int countVisible(const Filter& f) {
    int n = 0;
    for (bool v : f.visible) n += v ? 1 : 0;
    return n;
}

// The rows of a page built the way the editor builds one: foldout sections, each with labelled rows, with a separator
// row (no label, no value) between two of them.
std::vector<Field> page() {
    return {
        {"Transform", "Position", "0.00, 1.35, -4.20"},
        {"Transform", "Rotation", "0, 32, 0"},
        {"Transform", "Scale", "1, 1, 1"},
        {"Lighting", "Sun Angle", "32"},
        {"Lighting", "Sun Colour", "#fff3e0"},
        {"Lighting", "Sun Strength", "1.6"},
        {"Lighting", "", ""},                       // a separator, like the one above a sub-group
        {"Lighting", "Shadow Softness", "0.35"},
        {"Lighting", "Shadow Tint", "#8fa8ff"},
        {"Toon Extras", "Ramp Offset", "0.5"},
        {"Toon Extras", "Rim Power", "3.2"},
        {"Toon Extras", "Hatch On", "true"},
        {"Toon Extras", "Outline Noise", "0.15"},
        {"Render", "Wireframe", "false"},
        {"Render", "Exposure", "0.85"},
    };
}

size_t indexOfSection(const Filter& f, const std::vector<Field>& fields, const std::string& section) {
    for (size_t i : f.sections)
        if (fields[i].section == section) return i;
    return SIZE_MAX;
}

bool sectionAlive(const Filter& f, const std::vector<Field>& fields, const std::string& section) {
    return indexOfSection(f, fields, section) != SIZE_MAX;
}

} // namespace

int main() {
    const std::vector<Field> fields = page();

    std::printf("1. An empty query is the page itself\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "");
        check(!f.active, "1a: an empty query is not active");
        check(countVisible(f) == static_cast<int>(fields.size()), "1b: every row stays");
        check(f.shown == 14, "1c: the separators are not counted as matches", std::to_string(f.shown));
        check(f.sections.size() == 4, "1d: all four sections are listed", std::to_string(f.sections.size()));
        check(sectionAlive(f, fields, "Transform") && sectionAlive(f, fields, "Lighting") &&
                  sectionAlive(f, fields, "Toon Extras") && sectionAlive(f, fields, "Render"),
              "1e: by name, in page order");
        const Filter ws = ukiyo::propsearch::filter(fields, "   \t\n ");
        check(!ws.active && countVisible(ws) == static_cast<int>(fields.size()), "1f: whitespace is an empty query");
        check(ukiyo::propsearch::words("  sun   angle ").size() == 2, "1g: words() splits on runs of whitespace");
    }

    std::printf("2. One word finds a row by its label, in any case\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "sun");
        check(f.active, "2a: the query is active");
        check(f.shown == 3, "2b: three rows carry 'sun'", std::to_string(f.shown));
        check(f.visible[3] && f.visible[4] && f.visible[5], "2c: Sun Angle, Sun Colour and Sun Strength");
        check(!f.visible[0], "2d: a row without it is hidden");
        check(f.sections.size() == 1 && sectionAlive(f, fields, "Lighting"), "2e: only the Lighting section survives");
        check(!sectionAlive(f, fields, "Transform"), "2f: and Transform is gone");
        // The separator row of a surviving section stays, so the group it divides keeps its shape.
        check(f.visible[6], "2g: the separator inside the surviving section stays");
        const Filter upper = ukiyo::propsearch::filter(fields, "SUN");
        check(countVisible(upper) == countVisible(f) && upper.shown == f.shown, "2h: matching is case-insensitive");
    }

    std::printf("3. Several words all have to match\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "shadow soft");
        check(f.shown == 1 && f.visible[7], "3a: 'shadow soft' finds Shadow Softness", std::to_string(f.shown));
        const Filter none = ukiyo::propsearch::filter(fields, "shadow transform");
        check(none.shown == 0 && none.sections.empty(), "3b: words from two rows do not match either row");
        // A word may straddle the section and the label: "lighting sun" is what a person types when they half remember.
        const Filter straddle = ukiyo::propsearch::filter(fields, "lighting sun");
        check(straddle.shown == 3, "3c: 'lighting sun' matches through section + label", std::to_string(straddle.shown));
        const Filter wrongSection = ukiyo::propsearch::filter(fields, "toon sun");
        check(wrongSection.shown == 0, "3d: but not across the wrong section");
        const Filter together = ukiyo::propsearch::filter(fields, "toon rim");
        check(together.shown == 1 && together.visible[10], "3e: 'toon rim' narrows to Rim Power");
    }

    std::printf("4. Globs, because a section can be long\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "sun*");
        check(f.shown == 3, "4a: 'sun*' matches every Sun row", std::to_string(f.shown));
        const Filter suffix = ukiyo::propsearch::filter(fields, "*power");
        check(suffix.shown == 1 && suffix.visible[10], "4b: '*power' finds Rim Power");
        // A pattern has to cover the whole label, exactly as it does in the Project browser: "[Ss]un" alone is not
        // "Sun Angle", and a caller who means the prefix writes the prefix.
        const Filter bare = ukiyo::propsearch::filter(fields, "[Ss]un");
        check(bare.shown == 0, "4c: a pattern covers the whole label, so '[Ss]un' alone finds nothing");
        const Filter set = ukiyo::propsearch::filter(fields, "[Ss]un*");
        check(set.shown == 3, "4d: and '[Ss]un*' finds the three");
        const Filter notGlob = ukiyo::propsearch::filter(fields, "sun?");
        check(notGlob.shown == 0, "4e: 'sun?' is a pattern, not a substring: nothing is four letters and starts with sun");
    }

    std::printf("5. A value is searchable\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "0.35");
        check(f.shown == 1 && f.visible[7], "5a: '0.35' finds the row holding it");
        const Filter on = ukiyo::propsearch::filter(fields, "true");
        check(on.shown == 1 && on.visible[11], "5b: 'true' finds the toggle that is on");
        const Filter off = ukiyo::propsearch::filter(fields, "0.35", /*matchValue=*/false);
        check(off.shown == 0, "5c: a caller can turn value matching off");
        const Filter hex = ukiyo::propsearch::filter(fields, "#8fa8ff");
        check(hex.shown == 1 && hex.visible[8], "5d: a colour's hex finds its row");
    }

    std::printf("6. A leading minus removes what it matches\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "lighting -toon");
        check(f.shown == 5, "6a: 'lighting -toon' keeps the five lighting rows", std::to_string(f.shown));
        const Filter onlyNeg = ukiyo::propsearch::filter(fields, "-toon");
        check(onlyNeg.shown == 10, "6b: a query of only negations keeps everything else", std::to_string(onlyNeg.shown));
        check(!onlyNeg.visible[9] && !onlyNeg.visible[12], "6c: and hides what it names");
        check(!sectionAlive(onlyNeg, fields, "Toon Extras"), "6d: the emptied section is gone");
        const Filter twoNeg = ukiyo::propsearch::filter(fields, "-toon -render");
        check(twoNeg.shown == 8, "6e: two negations both apply", std::to_string(twoNeg.shown));
        const Filter mixed = ukiyo::propsearch::filter(fields, "sun -strength");
        check(mixed.shown == 2, "6f: 'sun -strength' keeps two of the three", std::to_string(mixed.shown));
        check(ukiyo::propsearch::matches(fields[9], "-toon", true) == false, "6g: matches() agrees with filter()");
        check(ukiyo::propsearch::matches(fields[0], "-toon", true) == true, "6h: and keeps what is not named");
    }

    std::printf("7. A query that matches nothing empties the page, rather than showing everything\n");
    {
        const Filter f = ukiyo::propsearch::filter(fields, "xylophone");
        check(f.active && f.shown == 0 && f.sections.empty(), "7a: no rows, no sections");
        check(countVisible(f) == 0, "7b: not even the separators");
        check(f.visible.size() == fields.size(), "7c: the answer is one flag per row, always");
    }

    std::printf("8. Sections are listed once, in page order\n");
    {
        // 'u' is in Lighting (Sun), Toon Extras (Outline) and Render (Exposure), but in nothing Transform owns.
        const Filter f = ukiyo::propsearch::filter(fields, "u");
        check(f.sections.size() == 3, "8a: three sections have a row with a 'u'", std::to_string(f.sections.size()));
        check(!sectionAlive(f, fields, "Transform"), "8a2: Transform does not, and is not listed");
        bool ordered = true;
        for (size_t k = 1; k < f.sections.size(); ++k) ordered = ordered && f.sections[k - 1] < f.sections[k];
        check(ordered, "8b: first occurrence order");
        const Filter multi = ukiyo::propsearch::filter(fields, "o");
        check(multi.sections.size() == 4, "8c: and a section with many matches is still listed once");
    }

    std::printf("9. The grammar is the Project browser's, so the two boxes behave the same\n");
    {
        // The same word against a name, through the browser's rule and through this one.
        check(ukiyo::matchesNameTokens("Sun Angle", "sun", true) ==
                  ukiyo::propsearch::matches({"Lighting", "Sun Angle", ""}, "sun", true),
              "9a: a plain word means the same in both");
        check(ukiyo::globMatch("sun*", "Sun Angle", true) &&
                  ukiyo::propsearch::matches({"Lighting", "Sun Angle", ""}, "sun*", true),
              "9b: and so does a pattern");
    }

    std::printf("\nproperty_search_test: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
