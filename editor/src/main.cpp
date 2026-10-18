// Entry point for the Ukiyo World Editor.
#include <cstdio>

#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QTextStream>

#include "icons.h"
#include "main_window.h"

#ifndef UKIYO_ROOT
#define UKIYO_ROOT "."
#endif

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Ukiyo World Editor");

    // Material Symbols icon font, shipped with the editor.
    editor::loadIconFont(QStringLiteral(UKIYO_ROOT) + "/editor/res/fonts/MaterialSymbolsOutlined.ttf");

    // Dark theme, shipped with the editor.
    QFile qss(QStringLiteral(UKIYO_ROOT) + "/editor/res/dark.qss");
    if (qss.open(QFile::ReadOnly | QFile::Text))
        app.setStyleSheet(QTextStream(&qss).readAll());

    editor::MainWindow window;
    window.show();

    QString error;
    const bool selfTest = argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--selftest");
    if (!window.bootstrap(&error)) {
        // A self-test has no one to click a dialog, so it reports the failure and stops.
        if (selfTest) {
            std::fprintf(stderr, "selftest: the engine could not start: %s\n", qPrintable(error));
            return 1;
        }
        QMessageBox::warning(&window, "Ukiyo World Editor",
                             "The engine could not start:\n" + error);
    }

    // `ukiyo_editor --selftest`: run the built-in checks on the live window, then exit.
    if (selfTest) {
        const int failed = window.runSelfTest();
        return failed == 0 ? 0 : 1;
    }
    return app.exec();
}
