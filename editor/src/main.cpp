// Entry point for the Ukiyo World Editor. `--project <folder>` opens a project made by the hub;
// without it the editor uses the engine checkout, as it always has.
#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QTextStream>

#include "icons.h"
#include "main_window.h"

#include "lua_editor.h"

#include "scripting_tools.h"
#include "splash_screen.h"

#ifndef UKIYO_ROOT
#define UKIYO_ROOT "."
#endif

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Ukiyo World Editor");

    // Material Symbols icon font and the bilingual UI typeface, both shipped with the editor.
    editor::loadIconFont(QStringLiteral(UKIYO_ROOT) + "/editor/res/fonts/MaterialSymbolsOutlined.ttf");
    editor::loadBrandFont(QStringLiteral(UKIYO_ROOT) + "/editor/res/fonts");
    editor::loadDisplayFont(QStringLiteral(UKIYO_ROOT) + "/editor/res/fonts");

    // Dark theme, shipped with the editor.
    QFile qss(QStringLiteral(UKIYO_ROOT) + "/editor/res/dark.qss");
    if (qss.open(QFile::ReadOnly | QFile::Text)) app.setStyleSheet(QTextStream(&qss).readAll());

    bool selfTest = false;
    bool noSplash = false;
    QString projectRoot;
    QString shotScript;   // --screenshot-script <file>: capture the Lua editor, for the figure in docs/LUA.md
    bool shotTools = false;  // --screenshot-tools: capture the Scripting Tools window
    QString shotOut;      // --out <png>
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        const QString& arg = arguments[i];
        if (arg == QLatin1String("--selftest")) {
            selfTest = true;
        } else if (arg == QLatin1String("--no-splash")) {
            noSplash = true;
        } else if (arg == QLatin1String("--screenshot-script") && i + 1 < arguments.size()) {
            shotScript = arguments[++i];
        } else if (arg == QLatin1String("--screenshot-tools")) {
            shotTools = true;
        } else if (arg == QLatin1String("--out") && i + 1 < arguments.size()) {
            shotOut = arguments[++i];
        } else if (arg == QLatin1String("--project") && i + 1 < arguments.size()) {
            projectRoot = arguments[++i];
        } else if (arg.startsWith(QLatin1String("--project="))) {
            projectRoot = arg.section(QLatin1Char('='), 1);
        } else if (!arg.startsWith(QLatin1Char('-')) && projectRoot.isEmpty()) {
            // `ukiyo_editor <folder>` works too, which is handy from a terminal.
            projectRoot = arg;
        }
    }
    if (!projectRoot.isEmpty()) {
        projectRoot = QDir::cleanPath(QDir::current().absoluteFilePath(projectRoot));
        if (!QFileInfo(projectRoot).isDir()) {
            QMessageBox::warning(nullptr, QStringLiteral("Ukiyo World Editor"),
                                 QStringLiteral("The project folder does not exist:\n\n") + projectRoot);
            return 2;
        }
    }

    // `ukiyo_editor --screenshot-script <file> --out <png>`: the built-in Lua editor, drawn by the program itself.
    // It runs before the engine starts, so it needs neither a device nor a project: the dialog is a widget.
    if (!shotScript.isEmpty()) {
        editor::LuaEditorDialog dialog(nullptr, shotScript, nullptr);
        QString error;
        if (!dialog.openFile(&error)) {
            std::fprintf(stderr, "screenshot-script: %s\n", qPrintable(error));
            return 1;
        }
        dialog.resize(1000, 760);
        dialog.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const QPixmap shot = dialog.grab();
        if (shotOut.isEmpty() || !shot.save(shotOut)) {
            std::fprintf(stderr, "screenshot-script: could not write %s\n", qPrintable(shotOut));
            return 1;
        }
        return 0;
    }

    // `ukiyo_editor --screenshot-tools --out <png>`: the Scripting Tools window, drawn by the program itself. The tool
    // list is engine data, so this needs no project and no device either.
    if (shotTools) {
        editor::ScriptingToolsDialog tools(nullptr, QString(), nullptr);
        tools.resize(960, 600);
        tools.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const QPixmap shot = tools.grab();
        if (shotOut.isEmpty() || !shot.save(shotOut)) {
            std::fprintf(stderr, "screenshot-tools: could not write %s\n", qPrintable(shotOut));
            return 1;
        }
        return 0;
    }

    // The splash covers the window while the engine starts. A self-test has no one to look at it.
    QPointer<editor::SplashScreen> splash;
    if (!selfTest && !noSplash) {
        splash = new editor::SplashScreen(QStringLiteral(UKIYO_ROOT) + "/editor/res/splash.png",
                                          QStringLiteral("World Editor"));
        splash->setEyebrow(QStringLiteral("STARTING EDITOR"));
        splash->setStage(0.02f, QStringLiteral("Preparing the editor"));
        if (!projectRoot.isEmpty()) splash->setCaption(QDir::toNativeSeparators(projectRoot));
        splash->show();
        QCoreApplication::processEvents();
    }

    editor::MainWindow window(projectRoot);
    window.show();

    QString error;
    const bool started = window.bootstrap(&error, [&splash](const QString& stage, float fraction) {
        if (splash) splash->setStage(fraction, stage);
        QCoreApplication::processEvents();
    });

    if (!started) {
        if (splash) splash->finish();
        // A self-test has no one to click a dialog, so it reports the failure and stops.
        if (selfTest) {
            std::fprintf(stderr, "selftest: the engine could not start: %s\n", qPrintable(error));
            return 1;
        }
        QMessageBox::warning(&window, "Ukiyo World Editor",
                             "The engine could not start:\n" + error);
    } else if (splash) {
        splash->setStage(1.0f, QStringLiteral("Ready"));
        splash->finish();
    }

    // `ukiyo_editor --selftest`: run the built-in checks on the live window, then exit.
    if (selfTest) {
        const int failed = window.runSelfTest();
        return failed == 0 ? 0 : 1;
    }
    return app.exec();
}
