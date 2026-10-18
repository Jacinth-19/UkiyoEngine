// File operations for the Project browser: new folder, rename (which rewrites project references),
// move to trash, import, and the "uses" / "used by" lists shown in the details pane.
// Everything here works on a project root and does not touch the window, so the selftest can call it
// on a temporary project.
#pragma once

#include <QString>
#include <QStringList>

namespace assetops {

struct Result {
    bool ok = false;
    QString message;   // shown to the user when ok is false
};

// Project-relative path with forward slashes, e.g. "assets/Seed-san.vrm".
QString relative(const QString& root, const QString& path);

// True for the editor's JSON files that can hold project-relative asset paths
// (.ukscene, .ukprefab, .ukmat, .ukshader).
bool isReferenceFile(const QString& path);

// A name that is not taken in dir: base, then "base 2", "base 3", ...
QString uniqueName(const QString& dir, const QString& base);

// Name rules for files and folders: not empty, no slashes, no control characters, not "." or "..".
Result checkName(const QString& name);

// Creates an empty folder called name inside parentDir. created receives the new path.
Result createFolder(const QString& parentDir, const QString& name, QString* created);

// Renames a file or folder inside root and rewrites every reference file in root that names it
// (or anything inside it). rewritten receives the number of reference files changed.
Result rename(const QString& root, const QString& path, const QString& newName, QString* newPath, int* rewritten);

// Moves a file or folder to the system trash.
Result trash(const QString& path);

// Imports files and folders into destDir. A name that is taken gets " (2)", " (3)"... imported
// receives the new paths.
Result importInto(const QStringList& sources, const QString& destDir, QStringList* imported);

// Project-relative paths that this reference file names and that exist. Empty for other files.
QStringList usesOf(const QString& root, const QString& path);

// Project-relative reference files that name this file, or a file inside this folder.
QStringList usedBy(const QString& root, const QString& path);

} // namespace assetops
