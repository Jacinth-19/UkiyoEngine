#include "engine/browser_zoom.h"

namespace ukiyo {

namespace {

// Round up to the nearest stop, then hold it inside the range. Rounding up (rather than to nearest) keeps a size the
// user asked for from shrinking under their hand: 97 becomes 112, not 96.
int snap(int px) {
    if (px <= kThumbMin) return kThumbMin;
    if (px >= kThumbMax) return kThumbMax;
    const int stops = (px - kThumbMin + kThumbStep - 1) / kThumbStep;
    return kThumbMin + stops * kThumbStep;
}

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

int thumbClamp(int px) { return snap(px); }

int thumbSliderValue(int px) { return snap(px); }

int thumbStep(int px, int notches) {
    // The current size is snapped first: a value that is already on the ladder is unchanged by this, and one that is
    // not cannot shift the whole ladder as it is stepped (96 + 16 is 112 whether 96 or 97 was the start).
    return snap(snap(px) + notches * kThumbStep);
}

int thumbGridWidth(int px) {
    const int v = snap(px);
    return v + 24;
}

int thumbGridHeight(int px) {
    const int v = snap(px);
    return v + 44;   // the icon, its air, and one wrapped line of the name
}

int thumbListIcon(int px) {
    const int v = snap(px);
    const int fifth = (v * 20 + 48) / 96;   // 20 px at the default 96, scaled with the slider
    return clampInt(fifth, 14, 48);
}

int thumbIconForView(int view, int px) {
    if (view == 1) return thumbListIcon(px);   // list view: text rows with a small icon
    return snap(px);                           // icon view, and the details view's own fixed size is the caller's
}

bool thumbWheelZooms(int view) { return view == 0 || view == 1; }

}  // namespace ukiyo
