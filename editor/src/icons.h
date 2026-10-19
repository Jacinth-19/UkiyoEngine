// Editor icons come from the Material Symbols Outlined font (Apache-2.0), shipped in
// editor/res/fonts. Each icon is rendered from its codepoint into pixmaps, so it stays sharp at
// any size. Names are the Material Symbols ligature names, for example "play_arrow".
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace editor {

// Loads the icon font. Call once after the QApplication exists. Returns false when the file
// cannot be read; icons then come out empty.
bool loadIconFont(const QString& ttfPath);

// Registers the engine's bilingual typeface, IBM Plex Sans JP (SIL OFL, editor/res/fonts), and makes it the
// application font. One family covers Latin and Japanese, so a label can mix "ENGINE" and 浮世 without a fallback
// that silently changes the design on a machine with no Japanese fonts installed. `fontsDir` is editor/res/fonts.
// Returns the family name, or an empty string when no face loaded (the app then keeps its default font).
QString loadBrandFont(const QString& fontsDir);

// Icon for a Material Symbols name. The disabled state is the same glyph at reduced opacity.
// Returns an empty icon when the name is not in the subset shipped with the editor.
QIcon materialIcon(const char* name, const QColor& color, int px = 16);

// Editor palette shared by the panels and the icons.
QColor iconColor();      // standard icon grey
QColor iconDimColor();   // secondary icons
QColor folderColor();    // folder glyphs
QColor infoColor();
QColor warningColor();
QColor errorColor();

} // namespace editor
