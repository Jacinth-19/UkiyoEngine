// Tests for engine/browser_zoom.h — the Project browser's thumbnail zoom (GH-119103).
//
// The checks are written as the gestures the user makes ("two notches up, one notch down") and as the invariants the
// layout depends on (the grid is always bigger than the icon, the ladder never leaves its ends, a round trip returns to
// where it started), so a future change to the numbers that keeps the behaviour keeps the test green.

#include "engine/browser_zoom.h"

#include <cstdio>
#include <vector>

using namespace ukiyo;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

static void checkEq(int got, int want, const char* what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("FAIL %s: got %d want %d\n", what, got, want);
    }
}

// The ladder itself: every stop, in order, from the bottom to the top.
static void testLadder() {
    checkEq(kThumbMin, 32, "1a: the ladder starts at 32");
    checkEq(kThumbMax, 224, "1b: and ends at 224");
    checkEq(kThumbStep, 16, "1c: in steps of 16");
    checkEq(kThumbDefault, 96, "1d: opening at the size the browser always had");
    checkEq(kThumbStops, 13, "1e: thirteen stops on the ladder");
    checkEq(kThumbStops, (kThumbMax - kThumbMin) / kThumbStep + 1, "1f: the stop count matches the step");

    // Each stop, and nothing between them.
    int stop = kThumbMin;
    int seen = 0;
    for (int px = kThumbMin; px <= kThumbMax; ++px) {
        const int snapped = thumbClamp(px);
        check(snapped >= px && snapped < px + kThumbStep, "1g: a size snaps up to the next stop");
        check((snapped - kThumbMin) % kThumbStep == 0, "1h: every snapped size is on the ladder");
        if (px == stop) {
            checkEq(snapped, px, "1i: a stop is already snapped");
            stop += kThumbStep;
            ++seen;
        }
    }
    checkEq(seen, kThumbStops, "1j: the walk visited every stop");
}

static void testClamp() {
    checkEq(thumbClamp(-1000), kThumbMin, "2a: far below the ladder is the bottom stop");
    checkEq(thumbClamp(0), kThumbMin, "2b: zero is the bottom stop");
    checkEq(thumbClamp(kThumbMin - 1), kThumbMin, "2c: one below the bottom is the bottom");
    checkEq(thumbClamp(kThumbMax + 1), kThumbMax, "2d: one above the top is the top");
    checkEq(thumbClamp(100000), kThumbMax, "2e: far above the ladder is the top stop");
    checkEq(thumbClamp(96), 96, "2f: a stop is left alone");
    checkEq(thumbClamp(97), 112, "2g: a value between stops rounds up, so an icon never shrinks under the hand");
    checkEq(thumbClamp(111), 112, "2h: and it rounds to the same stop from just under it");

    // Every value lands on the ladder.
    for (int px = -64; px <= 320; ++px) {
        const int v = thumbClamp(px);
        check(v >= kThumbMin && v <= kThumbMax, "2i: clamped sizes stay in range");
        check((v - kThumbMin) % kThumbStep == 0, "2j: clamped sizes stay on the ladder");
    }
}

static void testWheel() {
    checkEq(thumbStep(96, 1), 112, "3a: one notch up grows by 16");
    checkEq(thumbStep(96, -1), 80, "3b: one notch down shrinks by 16");
    checkEq(thumbStep(96, 2), 128, "3c: two notches up grow by 32");
    checkEq(thumbStep(96, 0), 96, "3d: no notch changes nothing");
    checkEq(thumbStep(thumbStep(96, 1), -1), 96, "3e: up then down returns to where it started");
    checkEq(thumbStep(thumbStep(96, -1), 1), 96, "3f: down then up returns as well");

    // The ends hold: a long scroll saturates instead of wrapping or overshooting.
    checkEq(thumbStep(kThumbMax, 1), kThumbMax, "3g: a notch at the top stays at the top");
    checkEq(thumbStep(kThumbMax, 50), kThumbMax, "3h: fifty notches at the top stay at the top");
    checkEq(thumbStep(kThumbMin, -1), kThumbMin, "3i: a notch at the bottom stays at the bottom");
    checkEq(thumbStep(kThumbMin, -50), kThumbMin, "3j: fifty notches at the bottom stay at the bottom");

    // A whole sweep up and back down visits every stop, in order, once.
    std::vector<int> up;
    int px = kThumbMin;
    for (int i = 0; i < kThumbStops - 1; ++i) {
        px = thumbStep(px, 1);
        up.push_back(px);
    }
    checkEq(px, kThumbMax, "3k: stepping up from the bottom reaches the top");
    checkEq(static_cast<int>(up.size()), kThumbStops - 1, "3l: in exactly stops-1 notches");
    bool ascending = true;
    for (size_t i = 1; i < up.size(); ++i) ascending = ascending && up[i] > up[i - 1];
    check(ascending, "3m: and every stop on the way is larger than the one before");
    int back = px;
    for (int i = 0; i < kThumbStops - 1; ++i) back = thumbStep(back, -1);
    checkEq(back, kThumbMin, "3n: stepping down again returns to the bottom");

    // A stray value off the ladder (a pref file edited by hand) cannot shift the ladder as it is stepped.
    checkEq(thumbStep(97, 1), 128, "3o: stepping from just off the ladder snaps it onto the ladder first");
    checkEq(thumbStep(97, -1), 96, "3p: in both directions");
}

static void testGrid() {
    // The grid always leaves room for the icon and a line of text, at every stop.
    for (int px = kThumbMin; px <= kThumbMax; px += kThumbStep) {
        check(thumbGridWidth(px) > px, "4a: the grid cell is wider than the icon");
        check(thumbGridHeight(px) > px + 16, "4b: and taller than the icon plus a readable line of text");
        checkEq(thumbGridWidth(px), px + 24, "4c: 24 px of air across");
        checkEq(thumbGridHeight(px), px + 44, "4d: 44 px down, for one wrapped line");
    }
    checkEq(thumbGridWidth(96), 120, "4e: the default grid matches the size the browser used before zoom");
    checkEq(thumbGridHeight(96), 140, "4f: in both directions");
    // Off-ladder input is snapped first, so the grid is never a size the slider cannot show.
    checkEq(thumbGridWidth(100), 136, "4g: the grid snaps with the icon");
    checkEq(thumbGridHeight(100), 156, "4h: in both directions");
}

static void testListIcon() {
    checkEq(thumbListIcon(96), 20, "5a: at the default size a list row keeps the 20 px icon it always had");
    checkEq(thumbListIcon(kThumbMin), 14, "5b: the smallest thumbnail gives the row icon its floor");
    checkEq(thumbListIcon(kThumbMax), 47, "5c: the largest thumbnail gives the row icon a bigger picture");
    int previous = 0;
    bool monotonic = true;
    for (int px = kThumbMin; px <= kThumbMax; px += kThumbStep) {
        const int icon = thumbListIcon(px);
        check(icon >= 14 && icon <= 48, "5d: the row icon stays between 14 and 48");
        monotonic = monotonic && icon >= previous;
        previous = icon;
    }
    check(monotonic, "5e: the row icon never shrinks as the thumbnail grows");
    check(thumbListIcon(kThumbMax) < 96, "5f: a list row is still text first, not a picture");
}

static void testViews() {
    checkEq(thumbIconForView(0, 96), 96, "6a: icon view draws the thumbnail");
    checkEq(thumbIconForView(1, 96), 20, "6b: list view draws the row icon");
    check(thumbWheelZooms(0), "6c: the wheel zooms the icon view");
    check(thumbWheelZooms(1), "6d: and the list view");
    check(!thumbWheelZooms(2), "6e: the details view is a table and keeps the wheel for scrolling");
    check(!thumbWheelZooms(-1), "6f: an unknown view does not swallow the wheel");
    check(!thumbWheelZooms(3), "6g: nor does one past the last");
}

int main() {
    testLadder();
    testClamp();
    testWheel();
    testGrid();
    testListIcon();
    testViews();

    if (failures == 0) std::printf("browser zoom: %d checks, 0 failure(s)\n", checks);
    else std::printf("browser zoom: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
