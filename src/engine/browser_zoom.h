// The Project browser's thumbnail zoom (Godot 4.8's GH-119103): the size ladder, the grid that follows it, and the
// rule that decides whether a mouse wheel over the browser is a zoom or an ordinary scroll.
//
// It lives in the engine, not in the widget, for the same reason the glob grammar does: the numbers are the contract.
// The editor's slider, the Ctrl + wheel handler and the browser's layout all read them from here, so the three cannot
// drift apart, and a test can hold the ladder still without a window. Nothing in this file includes Qt or a graphics
// header, so the Android build compiles it unchanged.
#pragma once

namespace ukiyo {

// Godot's ladder: 32 to 224 in steps of 16, opening at 96. Sixteen is small enough that one wheel notch feels like a
// nudge and large enough that a thumbnail never lands on a size the grid cannot lay out.
inline constexpr int kThumbMin = 32;
inline constexpr int kThumbMax = 224;
inline constexpr int kThumbStep = 16;
inline constexpr int kThumbDefault = 96;

// The sizes the slider can hold: 32, 48, 64 ... 224.
inline constexpr int kThumbStops = (kThumbMax - kThumbMin) / kThumbStep + 1;

// Any size brought onto the ladder: below 32 it is 32, above 224 it is 224, and anything between is rounded up to the
// next stop. A saved preference from an older build (the editor used to allow 48 to 192 in steps of one) therefore
// still opens at a sane size instead of being thrown away.
int thumbClamp(int px);

// The size after `notches` wheel notches: positive zooms in, negative zooms out, and the result is always on the
// ladder. Notches that would leave the range stop at the edge, so a long scroll neither overshoots nor wraps.
int thumbStep(int px, int notches);

// The value `notches` away from px, as the slider would give it (the same function; named for the slider's use).
int thumbSliderValue(int px);

// Grid cell for an icon of `px`: the icon, a little air on each side, and room for one wrapped line of text under it.
int thumbGridWidth(int px);
int thumbGridHeight(int px);

// Row icon in list view. List rows are text first, so the icon is a fifth of the thumbnail size — 20 px at the default
// 96, exactly what the list view used before zoom existed — held between 14 and 48 so rows stay readable at both ends.
int thumbListIcon(int px);

// The icon size a view draws with: icon view uses the thumbnail itself, list view the smaller row icon, and the details
// view its own fixed size (it shows columns, not pictures).
int thumbIconForView(int view, int px);

// Whether Ctrl + the wheel zooms in this view. The details view is a table and keeps the wheel for scrolling; the two
// picture views zoom. `view` is the editor's browser view index (0 icons, 1 list, 2 details).
bool thumbWheelZooms(int view);

}  // namespace ukiyo
