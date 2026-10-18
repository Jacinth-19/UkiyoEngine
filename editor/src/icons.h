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
