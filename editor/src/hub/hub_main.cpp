// Entry point for the Ukiyo Hub.
#include <cstdio>

#include <QApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>

#include "hub_selftest.h"
#include "hub_window.h"
#include "icons.h"
#include "project_launcher.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Ukiyo Hub"));
    app.setOrganizationName(QStringLiteral("Ukiyo"));
    app.setApplicationDisplayName(QStringLiteral("Ukiyo Hub"));

    const QString engine = editor::hub::engineRoot();
    editor::loadIconFont(engine + QStringLiteral("/editor/res/fonts/MaterialSymbolsOutlined.ttf"));
    editor::loadBrandFont(engine + QStringLiteral("/editor/res/fonts"));

    QString theme;
    for (const char* file : {"/editor/res/dark.qss", "/editor/res/hub.qss"}) {
        QFile qss(engine + QString::fromLatin1(file));
        if (qss.open(QFile::ReadOnly | QFile::Text)) theme += QTextStream(&qss).readAll();
    }
    app.setStyleSheet(theme);

    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--selftest")) {
            return editor::hub::runHubSelfTest();
        }
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--screenshots") && i + 1 < argc) {
            editor::hub::HubWindow window;
            window.capturePages(QString::fromLocal8Bit(argv[i + 1]));
            return 0;
        }
    }

    editor::hub::HubWindow window;
    window.show();
    return app.exec();
}
