// Tests for engine/search.h: Godot's fuzzy ranking and the glob grammar.
//
// The ranking assertions are written the way an editor uses them — "typing this puts that file first" — rather than as
// magic numbers, so a future tweak to the scoring that keeps the ordering keeps the test green.

#include "engine/search.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace ukiyo;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static void checkEq(int got, int want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("FAIL %s: got %d want %d\n", what.c_str(), got, want);
    }
}

static void checkEqStr(const std::string& got, const std::string& want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what.c_str(), got.c_str(), want.c_str());
    }
}

// The first hit is the one the user meant, by name.
static std::string top(const FuzzySearch& fs, const std::string& query, const std::vector<std::string>& items) {
    const auto r = fs.search(query, items);
    return r.empty() ? std::string{} : r.front().target;
}

static std::vector<std::string> names(const std::vector<FuzzyResult>& r) {
    std::vector<std::string> out;
    for (const FuzzyResult& f : r) out.push_back(f.target);
    return out;
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// --- 1. Whole and prefix matches --------------------------------------------------------------

static void testExactAndPrefix() {
    const std::vector<std::string> items{
        "assets/models/hero.vrm",       "assets/models/heroine.vrm", "assets/models/villain.vrm",
        "assets/shaders/lit.ukshader",  "assets/audio/hero_theme.ogg", "docs/hero.png",
    };
    FuzzySearch fs;
    // A name typed out finds its own file, ahead of anything that merely contains the pieces.
    checkEqStr(top(fs, "hero.vrm", items), "assets/models/hero.vrm", "1a: a full file name finds itself");
    check(fs.score("hero", "assets/models/hero.vrm") > fs.score("hero", "assets/models/high_energy_radio.ogg"),
          "1b: a contiguous run beats the same letters scattered");
    check(fs.score("hero", "assets/models/hero.vrm") > fs.score("hero", "assets/hero/models/extra.vrm"),
          "1c: a hit in the file name beats the same hit in a folder");
    check(fs.score("hero", "assets/models/hero.vrm") > 0, "1d: a real match scores positive");

    // Godot adds 100 for a run that covers a whole token, so an exact term cannot be beaten by a long fuzzy one.
    check(fs.score("hero", "assets/models/hero.vrm") >= 100, "1e: the whole-token bonus is in play");

    // No match at all is -1, never 0: 0 is reserved for "empty query matches everything".
    checkEq(fs.score("zzzz", "assets/models/hero.vrm"), -1, "1f: a missing term is -1");
    checkEq(fs.score("", "assets/models/hero.vrm"), 0, "1g: an empty query matches with score 0");
}

// --- 2. Subsequences, misses and budgets -----------------------------------------------------

static void testSubsequence() {
    FuzzySearch fs;
    check(fs.score("jmp", "assets/anim/Jump.glb") > 0, "2a: a plain subsequence matches");
    // One missed character still counts as a match, but it scores at the floor rather than turning negative: -1 is
    // reserved for "this does not match", so a ranking can never sort a match below a non-match.
    check(fs.score("jmpx", "assets/anim/Jump.glb") >= 0, "2b: one miss still matches, at the floor");
    fs.maxMisses = 2;
    check(fs.score("jmxp", "assets/anim/Jump.glb") >= 0, "2c: a raised miss budget admits another miss");

    // "healt" is one character away from "healing": exactly one miss, which the default budget allows.
    check(fs.score("healt", "assets/models/healing_potion.png") > 0, "2d: one typo still finds the file");

    FuzzySearch strict;
    strict.exactTokenMatches = false;
    strict.maxMisses = 0;
    checkEq(strict.score("jmpx", "assets/anim/Jump.glb"), -1, "2e: maxMisses 0 forbids any miss");

    // Misses are a per-query budget shared across tokens, not a per-token allowance.
    FuzzySearch budget;
    budget.maxMisses = 1;
    checkEq(budget.score("jmpz xyzq", "assets/anim/Jump.glb"), -1, "2f: two missing characters exhaust a budget of one");
    check(budget.score("jmpz anim", "assets/anim/Jump.glb") >= 0, "2g: a second token that matches exactly still fits");
}

// --- 3. Ordering the way an editor's search box wants it -------------------------------------

static void testRanking() {
    // The file name beats a folder of the same word, a short name beats a long one, and a boundary beats the middle of
    // a word. That ordering is the whole point: it is what makes typing three letters useful.
    const std::vector<std::string> items{
        "assets/environment/terrain/large_open_world_terrain_heightmap.png",
        "assets/models/Terrain.ukscene",
        "assets/terrain.png",
        "assets/textures/sky/night_sky.png",
    };
    FuzzySearch fs;
    const auto r = fs.search("terrain", items);
    check(!r.empty(), "3a: terrain matches something");
    if (r.size() >= 2) {
        check(r[0].target == "assets/terrain.png" || r[0].target == "assets/models/Terrain.ukscene",
              "3b: the short terrain asset outranks the long heightmap path");
        check(r[0].target != "assets/environment/terrain/large_open_world_terrain_heightmap.png",
              "3c: the long path is not first");
    }
    // Every result carries its index into the caller's list, so a view can select the real item.
    for (const FuzzyResult& f : fs.search("terrain", items)) {
        check(f.index >= 0 && f.index < static_cast<int>(items.size()), "3d: index in range");
        checkEqStr(items[static_cast<size_t>(f.index)], f.target, "3e: index points at the returned target");
    }
    // Deterministic: the same query twice gives the same order.
    check(names(fs.search("terrain", items)) == names(fs.search("terrain", items)), "3f: ordering is stable");
    // Best score first.
    bool sorted = true;
    for (size_t i = 1; i < r.size(); ++i)
        if (r[i - 1].score < r[i].score) sorted = false;
    check(sorted, "3g: results are sorted by score, descending");
}

// --- 4. Multi-token queries ------------------------------------------------------------------

static void testTokens() {
    const std::vector<std::string> items{
        "assets/models/potion_health.glb",
        "assets/models/HealthPotion.glb",
        "assets/models/potion_mana.glb",
        "assets/models/sword.glb",
    };
    FuzzySearch fs;
    const auto r = fs.search("health potion", items);
    check(!r.empty(), "4a: 'health potion' matches the potion assets");
    check(r.size() <= 2, "4b: and does not match the sword or the mana potion");
    check(contains(names(r), "assets/models/HealthPotion.glb"), "4c: HealthPotion is found");
    check(contains(names(r), "assets/models/potion_health.glb"), "4d: potion_health is found too");

    // Word order does not decide whether something matches, only how it ranks.
    check(!fs.search("potion health", items).empty(), "4e: reversed words still match");
    const auto ordered = fs.search("health potion", items);
    const auto reversed = fs.search("potion health", items);
    check(!ordered.empty() && !reversed.empty(), "4f: both directions return results");

    // Extra spaces (and a trailing one) are not separate empty tokens.
    checkEq(fs.search("  health   potion  ", items).size(), ordered.size(), "4g: runs of spaces are ignored");

    // An all-space query is the same as no query: everything, unranked.
    checkEq(fs.search("   ", items).size(), items.size(), "4h: a blank query lists everything");
}

// --- 5. Highlight ranges ---------------------------------------------------------------------

static void testRanges() {
    FuzzySearch fs;
    std::vector<FuzzyRange> ranges;
    const int s = fs.score("jump", "assets/anim/Jump.glb", &ranges);
    check(s > 0, "5a: jump matches");
    checkEq(static_cast<int>(ranges.size()), 1, "5b: one contiguous run");
    if (!ranges.empty()) {
        checkEq(ranges[0].start, 12, "5c: the run starts at 'J'");
        checkEq(ranges[0].length, 4, "5d: and is four characters long");
    }

    // A broken-up subsequence comes back as several runs, which is what an underline needs.
    ranges.clear();
    // "J" and then "mp" — the m in "anim" is closer, so the run breaks once and the highlight is two ranges, not three.
    check(fs.score("jmp", "assets/anim/Jump.glb", &ranges) > 0, "5e: jmp matches");
    checkEq(static_cast<int>(ranges.size()), 2, "5f: two runs, adjacent characters merged");
    if (ranges.size() == 2) {
        check(ranges[0].start < ranges[1].start, "5g: runs are ordered");
        checkEq(ranges[1].length, 2, "5h: the trailing 'mp' is one run");
        check(ranges[0].start >= 0 && ranges[1].start + ranges[1].length <= 22, "5i: runs stay inside the target");
    }

    // Every range is inside the target and non-empty for every query that matches.
    for (const char* q : {"a", "ab", "txt", "ph", "jum"}) {
        for (const char* t : {"a/b/c.txt", "potion_health.glb", "Jump.glb"}) {
            std::vector<FuzzyRange> rs;
            if (fs.score(q, t, &rs) >= 0) {
                for (const FuzzyRange& r : rs) check(r.start >= 0 && r.length > 0, "5j: no empty or negative range");
            }
        }
    }
    // No ranges are reported for a query that does not match.
    ranges.clear();
    checkEq(fs.score("qqqq", "a/b/c.txt", &ranges), -1, "5k: no match");
    check(ranges.empty(), "5l: and no ranges");
}

// --- 6. Case handling and the knobs ----------------------------------------------------------

static void testOptions() {
    FuzzySearch ci;
    FuzzySearch cs;
    cs.caseSensitive = true;
    cs.maxMisses = 0;   // otherwise the wrong case is simply one missed character
    check(ci.score("jump", "assets/Jump.glb") > 0, "6a: case-insensitive finds Jump");
    checkEq(cs.score("jump", "assets/Jump.glb"), -1, "6b: case-sensitive does not");
    check(cs.score("Jump", "assets/Jump.glb") > 0, "6c: but the right case does");
    // Case-insensitive hits rank below case-identical ones.
    check(ci.score("Jump", "assets/Jump.glb") > ci.score("jump", "assets/Jump.glb"),
          "6d: the same case scores above a case-folded hit");

    // maxResults truncates after sorting, so it keeps the best ones.
    FuzzySearch limited;
    limited.maxResults = 2;
    const std::vector<std::string> items{"terrain.png", "assets/terrain.png", "terrain/tall.png", "terrain_old.png"};
    const auto r = limited.search("terrain", items);
    checkEq(static_cast<int>(r.size()), 2, "6e: maxResults 2 returns two");
    if (!r.empty()) checkEqStr(r[0].target, "terrain.png", "6f: and they are the best two");

    // filterLowScores culls the tail. With a factor of 0 the cutoff sits at the average, so only above-average results
    // survive; a query where everything matches equally keeps them all (nothing is below the average).
    FuzzySearch filtering;
    filtering.filterLowScores = true;
    filtering.filterFactor = 0.5f;
    const auto all = FuzzySearch{}.search("a", {"a1.png", "a2.png", "b.png"});
    const auto filtered = filtering.search("a", {"a1.png", "a2.png", "b.png"});
    check(filtered.size() <= all.size(), "6g: filtering never returns more than no filtering");
    check(contains(names(filtered), "a1.png"), "6h: the strong match survives filtering");
    check(!contains(names(filtered), "b.png"), "6i: the weak one is culled");
}

// --- 7. Globs: the basics --------------------------------------------------------------------

static void testGlobBasics() {
    check(globMatch("*.png", "terrain.png"), "7a: *.png matches a file");
    // A star crosses '/', which is what the Project filter wants: typing *.png lists every image in the project, not
    // only the ones in the folder that happens to be open. Pattern a single path segment with [^/] when it matters.
    check(globMatch("*.png", "assets/terrain.png"), "7b: * crosses '/' in this grammar");
    check(globMatch("assets/*.png", "assets/terrain.png"), "7c: a prefixed pattern matches");
    check(!globMatch("assets/*.png", "other/terrain.png"), "7d: and does not match elsewhere");
    check(globMatch("*", "anything"), "7e: a lone star matches everything");
    check(globMatch("", ""), "7f: empty matches empty");
    check(!globMatch("", "x"), "7g: empty does not match a character");
    check(globMatch("?"  , "x"), "7h: ? matches one character");
    check(!globMatch("?", "xy"), "7i: ? does not match two");
    check(!globMatch("x?z", "xz"), "7j: ? is required");
    check(globMatch("terrain.*", "terrain.ukscene"), "7k: literal prefix plus star");
    check(globMatch("**/hero.vrm", "assets/models/hero.vrm"), "7l: ** behaves like *");
    check(globMatch("*hero*", "assets/models/hero.vrm"), "7m: mid-string star on both sides");
    check(!globMatch("*.ogg", "terrain.png"), "7n: a wrong suffix fails");
    // Case-insensitivity is the default, because Windows paths are not case-sensitive and a filter box should not be.
    check(globMatch("*.PNG", "terrain.png"), "7o: default is case-insensitive");
    check(!globMatch("*.PNG", "terrain.png", false), "7p: and can be turned off");
}

// --- 8. Globs: classes, groups, escapes, malformed input -------------------------------------

static void testGlobAdvanced() {
    check(globMatch("[a-c]?.txt", "b1.txt"), "8a: a range class");
    check(!globMatch("[a-c]?.txt", "d1.txt"), "8b: outside the range");
    check(globMatch("[!a-c]?.txt", "d1.txt"), "8c: a negated class");
    check(!globMatch("[!a-c]?.txt", "b1.txt"), "8d: negated class excludes");
    check(globMatch("[^0-9].png", "x.png"), "8e: ^ also negates");
    check(globMatch("[]].txt", "].txt"), "8f: a leading ] is a literal member");
    check(globMatch("[abc].txt", "b.txt"), "8g: a plain set");

    check(globMatch("{Lit,Toon}.ukshader", "Toon.ukshader"), "8h: a group matches either alternative");
    check(globMatch("{Lit,Toon}.ukshader", "Lit.ukshader"), "8i: including the first");
    check(!globMatch("{Lit,Toon}.ukshader", "PBR.ukshader"), "8j: and neither leaves no match");
    check(globMatch("a{b,c{d,e}}f", "acef"), "8k: nested groups");
    check(globMatch("a{b,c{d,e}}f", "abf"), "8l: the flat alternative too");

    // Escapes make syntax literal, which a file named "what?.png" needs.
    check(globMatch("what\\?.png", "what?.png"), "8m: an escaped ? is literal");
    check(!globMatch("what\\?.png", "whatx.png"), "8n: and no longer a wildcard");
    check(globMatch("star\\*.png", "star*.png"), "8o: an escaped * is literal");

    // A half-typed pattern is not an error: it is literal, and the listing keeps working while someone types.
    check(globMatch("[abc", "[abc"), "8p: unclosed [ is literal");
    check(!globMatch("[abc", "a"), "8q: and is not a class");
    check(globMatch("{Lit", "{Lit"), "8r: unclosed { is literal");

    // A pattern with no syntax is a whole-string match, not a substring match: contains() is what a caller wants when it
    // wants the other thing.
    check(globMatch("terrain", "terrain"), "8s: plain text matches itself");
    check(!globMatch("terrain", "terrain.png"), "8t: and not a longer string");
}

// --- 9. Detecting glob syntax, and searching a list ------------------------------------------

static void testGlobSearch() {
    check(looksLikeGlob("*.png"), "9a: * is syntax");
    check(looksLikeGlob("a?b"), "9b: ? is syntax");
    check(looksLikeGlob("[ab]"), "9c: [ is syntax");
    check(looksLikeGlob("{a,b}"), "9d: { is syntax");
    check(!looksLikeGlob("terrain.png"), "9e: a plain name is not a pattern");
    check(!looksLikeGlob("what\\?.png"), "9f: an escaped ? is not syntax");
    check(!looksLikeGlob(""), "9g: empty is not a pattern");

    const std::vector<std::string> items{"a.png", "b.png", "c.txt", "d.PNG", "e.jpg"};
    const auto png = globSearch(items, "*.png");
    checkEq(static_cast<int>(png.size()), 3, "9h: three png files, case-insensitively");
    check(png.size() == 3 && png[0] == 0 && png[1] == 1 && png[2] == 3, "9i: indices in order");
    checkEq(static_cast<int>(globSearch(items, "?.png").size()), 3, "9j: single-character names");
    checkEq(static_cast<int>(globSearch(items, "*.png", false).size()), 2, "9k: case-sensitive finds two");
    check(globSearch(items, "zzz").empty(), "9l: nothing matches");
    checkEq(static_cast<int>(globSearch(items, "*").size()), 5, "9m: a lone star lists the folder");

    // The Project browser's pattern: every asset under a folder with a given extension.
    const std::vector<std::string> paths{"assets/models/hero.vrm", "assets/models/hero.glb",
                                         "assets/audio/theme.ogg", "assets/shaders/lit.ukshader"};
    checkEq(static_cast<int>(globSearch(paths, "assets/models/*").size()), 2, "9n: one folder");
    checkEq(static_cast<int>(globSearch(paths, "{*.vrm,*.ukshader}").size()), 2, "9o: two kinds at once");
}

// --- 10. Scale: the search box runs on every keystroke ---------------------------------------

static void testScale() {
    std::vector<std::string> items;
    items.reserve(4000);
    for (int i = 0; i < 4000; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "assets/characters/character_%04d/body/mesh_%04d.glb", i, i);
        items.push_back(buf);
    }
    items.push_back("assets/models/hero.vrm");

    FuzzySearch fs;
    fs.maxResults = 50;
    const auto r = fs.search("hero", items);
    check(!r.empty(), "10a: a match at the end of a long list is found");
    if (!r.empty()) checkEqStr(r[0].target, "assets/models/hero.vrm", "10b: and ranks first");
    check(r.size() <= 50, "10c: maxResults caps the list");

    // A query that matches nothing still has to walk the list without blowing up.
    check(FuzzySearch{}.search("zzzzzz", items).empty(), "10d: no match over the whole list");

    // A pathological query ('*' in glob terms) stays fast enough on 4000 entries: correctness first, but the pattern
    // matcher must not backtrack into hours on several stars.
    const auto many = globSearch(items, "*/*/*/*.glb");
    checkEq(static_cast<int>(many.size()), 4000, "10e: four path segments");
}

// 11. The Project browser's query rule (GH-119103): per-token substring or glob, every token required.
static void testNameTokens() {
    const auto m = [](const char* name, const char* query) { return matchesNameTokens(name, query); };

    check(m("Hero.vrm", "hero"), "11a: a plain word is a substring");
    check(m("Hero.vrm", "HERO"), "11b: and it is case-insensitive");
    check(!m("Hero.vrm", "villain"), "11c: a word the name does not contain does not match");

    check(m("Sword.png", "*.png"), "11d: a token with a star is read as a glob");
    check(!m("Sword.png", "*.vrm"), "11e: and the glob has to match the whole name");
    check(m("Sword.png", "sword *.png"), "11f: a word and a glob together both have to match");
    check(!m("Sword.png", "sword *.vrm"), "11g: one failing token is a failing query");

    check(m("Brass Sword Idle.ukscene", "sword idle"), "11h: two words in either order, anywhere in the name");
    check(!m("Brass Sword Idle.ukscene", "sword run"), "11i: a second word that is absent fails the query");

    check(m("hero.vrm", "  hero  "), "11j: leading, trailing and repeated spaces are ignored");
    check(m("hero.vrm", ""), "11k: an empty query matches everything");
    check(m("hero.vrm", "   "), "11l: a spaces-only query matches everything too");

    check(m("ring_01.png", "ring_[0-9][0-9].png"), "11m: character classes work");
    check(m("Icon.PNG", "[Ii]con.[Pp][Nn][Gg]"), "11n: a class with both cases");
    check(m("a.b", "a.b"), "11o: a dot in a plain token is a literal dot, not a wildcard");
    check(m("axb", "a.b") == false, "11p: so a.b does not match axb");

    // The whole-listing use: filter a directory with one query.
    const std::vector<std::string> names{"Hero.vrm", "Brass Sword.vrm", "Sword Idle.ukscene", "Sword.png",
                                         "readme.txt"};
    int kept = 0;
    for (const auto& n : names)
        if (matchesNameTokens(n, "sword")) ++kept;
    checkEq(kept, 3, "11q: sword keeps the three sword assets and drops the rest");
    kept = 0;
    for (const auto& n : names)
        if (matchesNameTokens(n, "*.vrm")) ++kept;
    checkEq(kept, 2, "11r: *.vrm keeps exactly the two models");
    kept = 0;
    for (const auto& n : names)
        if (matchesNameTokens(n, "sword *.vrm")) ++kept;
    checkEq(kept, 1, "11s: sword *.vrm keeps the one sword model and drops the sword image");
}

// The "zero or more directories" half of "**/": a pattern written against a path also finds the file at the top.
static void testDoubleStarDirectories() {
    check(globMatch("**/idle.png", "idle.png"), "10a: **/ matches no directory at all");
    check(globMatch("**/idle.png", "Anims/idle.png"), "10b: and one directory");
    check(globMatch("**/idle.png", "Anims/Idle/idle.png"), "10c: and several");
    check(!globMatch("**/idle.png", "Anims/run.png"), "10d: while still requiring the file name");
    check(globMatch("anim/**/*.png", "anim/idle.png"), "10e: a double star in the middle may stand for nothing");
    check(globMatch("anim/**/*.png", "anim/walk/idle.png"), "10f: and for a folder");
    check(!globMatch("anim/**/*.png", "anims/idle.png"), "10g: the literal part either side still has to match");
    check(globMatch("**/*.vrm", "Swords/Blade.vrm"), "10h: a browser query of **/*.vrm finds a nested model");
    check(globMatch("**/*.vrm", "Blade.vrm"), "10i: including one at the top of the folder");
}

int main() {
    testExactAndPrefix();
    testSubsequence();
    testRanking();
    testTokens();
    testRanges();
    testOptions();
    testGlobBasics();
    testGlobAdvanced();
    testGlobSearch();
    testScale();
    testNameTokens();
    testDoubleStarDirectories();
    std::printf("search: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
