// Paths and process launching for the Ukiyo Hub. The hub lives beside the editor in the build
// folder, so it can start `ukiyo_editor --project <folder>` without any configuration.
#pragma once

#include <QString>

namespace editor::hub {

// The engine checkout this build came from (UKIYO_ROOT).
QString engineRoot();

// editor/res/splash.png when it exists; empty otherwise (the splash paints its fallback scene).
QString splashArtPath();

// The editor binary: $UKIYO_EDITOR when set, otherwise ukiyo_editor beside the hub. Empty when it
// cannot be found, so the hub can explain how to build it.
QString editorExecutable();

// Starts the editor on a project folder. Returns false with a reason when the editor is missing or
// the process cannot be started.
bool launchEditor(const QString& projectRoot, QString* error);

// Opens a folder in the desktop's file manager.
void revealFolder(const QString& path);

} // namespace editor::hub
