#include "player/hud_layout.h"

namespace ukiyo {

void drawPlayerHud(HudCanvas& hud, const PlayerHudText& text, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    hud.clear();
    const float w = hud.screenWidth();
    const float h = hud.screenHeight();
    const float pad = 12.0f * scale;
    const HudColor white(0.96f, 0.96f, 0.96f, 1.0f);
    const HudColor dim(0.72f, 0.74f, 0.78f, 0.9f);

    // A soft band behind the top line, so the title reads over a bright scene. The band is translucent: the game is
    // still visible through it, and a flat black box would look like a debug overlay rather than a shipped HUD.
    hud.rect({0.0f, 0.0f, w, 56.0f * scale}, HudColor(0.04f, 0.045f, 0.06f, 0.55f));
    hud.textShadowed(pad, pad, text.title, white, 20.0f * scale);
    if (!text.stats.empty()) {
        const float size = 15.0f * scale;
        hud.textShadowed(w - pad - hud.textWidth(text.stats, size), pad + 4.0f * scale, text.stats, dim, size);
    }

    // The controls along the bottom edge, and the first warning above the title, where the eye already is.
    hud.rect({0.0f, h - 34.0f * scale, w, 34.0f * scale}, HudColor(0.04f, 0.045f, 0.06f, 0.6f));
    hud.text(pad, h - 27.0f * scale, text.hint, HudColor(0.86f, 0.88f, 0.92f, 0.92f), 13.0f * scale);
    if (!text.warning.empty()) {
        hud.textShadowed(pad, 64.0f * scale, text.warning, HudColor(1.0f, 0.78f, 0.35f, 1.0f), 14.0f * scale);
    }
}

} // namespace ukiyo
