#include "project_launcher.h"

#include <QCoreApplication>
#include <QtGlobal>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QUrl>

#ifndef UKIYO_ROOT
#define UKIYO_ROOT "."
#endif

namespace editor::hub {

QString engineRoot() {
    return QDir::cleanPath(QStringLiteral(UKIYO_ROOT));
}

QString splashArtPath() {
    const QString path = engineRoot() + QStringLiteral("/editor/res/splash.png");
    return QFileInfo::exists(path) ? path : QString();
}

QString editorExecutable() {
    const QString override = qEnvironmentVariable("UKIYO_EDITOR");
    if (!override.isEmpty() && QFileInfo::exists(override)) return override;

#ifdef Q_OS_WIN
    const QString name = QStringLiteral("ukiyo_editor.exe");
#else
    const QString name = QStringLiteral("ukiyo_editor");
#endif
    const QString beside = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(name);
    if (QFileInfo::exists(beside)) return beside;

    // Editors built by hand sometimes land in a sibling folder.
    const QString build = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../build/" + name);
    if (QFileInfo::exists(build)) return QDir::cleanPath(build);
    return QString();
}

bool launchEditor(const QString& projectRoot, QString* error) {
    const QString editor = editorExecutable();
    if (editor.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                         "The editor executable was not found beside the hub.\n\n"
                         "Build it with:\n    cmake --build build --target ukiyo_editor\n\n"
                         "or set UKIYO_EDITOR to its path.");
        }
        return false;
    }
    if (!QFileInfo(projectRoot).isDir()) {
        if (error) *error = QStringLiteral("The project folder does not exist:\n") + projectRoot;
        return false;
    }

    const QStringList arguments{QStringLiteral("--project"), QDir::cleanPath(projectRoot)};
    qint64 pid = 0;
    if (!QProcess::startDetached(editor, arguments, engineRoot(), &pid)) {
        if (error) *error = QStringLiteral("The editor could not be started from ") + editor;
        return false;
    }
    return true;
}

void revealFolder(const QString& path) {
    if (path.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

} // namespace editor::hub
