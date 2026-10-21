// The player's HUD, laid out once.
//
// The player draws this over every frame; docs/PLAYER.md's figure is produced by the same function through a software
// rasteriser (tools/hud_preview.cpp), so the picture in the docs is the layout the game ships, not a mockup. Nothing here
// touches a device: it only appends to a HudCanvas.
#pragma once

#include <string>

#include "engine/hud.h"

namespace ukiyo {

struct PlayerHudText {
    std::string title;     // product name and version
    std::string stats;     // frame time, draw calls, triangles
    std::string hint;      // the controls line
    std::string warning;   // the first problem the world reported, empty when there is none
};

// Draws the whole HUD: a band and title along the top, the stats right-aligned, the controls along the bottom, and the
// warning in amber under the title. `scale` multiplies every pixel size for high-DPI windows; the manifest's hudScale.
void drawPlayerHud(HudCanvas& hud, const PlayerHudText& text, float scale);

} // namespace ukiyo
