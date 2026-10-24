// Entry point for the Ukiyo World Editor. `--project <folder>` opens a project made by the hub;
// without it the editor uses the engine checkout, as it always has.
#include <cstdio>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QTextStream>

#include "foldout.h"
#include "icons.h"
#include "inspector_filter.h"
#include "inspector_ui.h"
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
    QString shotQuery;    // --screenshot-inspector's query
    bool shotTools = false;  // --screenshot-tools: capture the Scripting Tools window
    bool shotInspector = false;  // --screenshot-inspector: capture the Inspector with a search typed into it
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
        } else if (arg == QLatin1String("--screenshot-inspector")) {
            shotInspector = true;
            // The next argument, when it does not start with '-', is the query to type into the box.
            if (i + 1 < arguments.size() && !arguments[i + 1].startsWith(QLatin1Char('-')))
                shotQuery = arguments[++i];
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

    // `ukiyo_editor --screenshot-inspector "shadow soft" --out <png>`: the Inspector with a query typed into its filter
    // box, drawn by the real widgets and narrowed by the real rule. The page is built here rather than opened from a
    // project, so the figure needs no assets and no device — but every widget in it is the one the editor builds, and
    // the narrowing is done by editor::InspectorFilter, the same class the live Inspector uses.
    if (shotInspector) {
        QWidget holder;
        holder.setObjectName(QStringLiteral("inspectorShot"));
        QVBoxLayout* outer = new QVBoxLayout(&holder);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        QLineEdit* filter = new QLineEdit;
        filter->setObjectName(QStringLiteral("inspectorFilter"));
        filter->setPlaceholderText(QStringLiteral("Filter properties"));
        filter->setClearButtonEnabled(true);
        QHBoxLayout* filterRow = new QHBoxLayout;
        filterRow->setContentsMargins(6, 4, 6, 4);
        filterRow->addWidget(filter);
        outer->addLayout(filterRow);
        QScrollArea* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        QWidget* page = new QWidget;
        QVBoxLayout* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        QFormLayout* transform = editor::addSection(v, QStringLiteral("Transform"));
        auto number = [](double value) {
            auto* box = new QDoubleSpinBox;
            box->setRange(-10000.0, 10000.0);
            box->setDecimals(3);
            box->setValue(value);
            return box;
        };
        auto vec3 = [&](double x, double y, double z) {
            auto* w = new QWidget;
            auto* row = new QHBoxLayout(w);
            row->setContentsMargins(0, 0, 0, 0);
            row->addWidget(number(x));
            row->addWidget(number(y));
            row->addWidget(number(z));
            return w;
        };
        transform->addRow(QStringLiteral("Position"), vec3(0.0, 1.35, -4.2));
        transform->addRow(QStringLiteral("Rotation"), vec3(0.0, 32.0, 0.0));
        transform->addRow(QStringLiteral("Scale"), vec3(1.0, 1.0, 1.0));

        QFormLayout* lighting = editor::addSection(v, QStringLiteral("Lighting"));
        auto checkBox = [](bool on, const QString& text) {
            auto* c = new QCheckBox(text);
            c->setChecked(on);
            return c;
        };
        lighting->addRow(QStringLiteral("Sun Angle"), number(32.0));
        lighting->addRow(QStringLiteral("Sun Colour"), new QLineEdit(QStringLiteral("#fff3e0")));
        lighting->addRow(QStringLiteral("Sun Strength"), number(1.6));
        lighting->addRow(QStringLiteral("Cast Shadows"), checkBox(true, QStringLiteral("On")));
        lighting->addRow(QStringLiteral("Shadow Softness"), number(0.35));
        lighting->addRow(QStringLiteral("Shadow Tint"), new QLineEdit(QStringLiteral("#8fa8ff")));
        lighting->addRow(QStringLiteral("Shadow Distance"), number(24.0));

        QFormLayout* toon = editor::addSection(v, QStringLiteral("Toon Extras"));
        toon->addRow(QStringLiteral("Ramp Offset"), number(0.5));
        toon->addRow(QStringLiteral("Rim Power"), number(3.2));
        toon->addRow(QStringLiteral("Hatch On"), checkBox(true, QStringLiteral("On")));
        toon->addRow(QStringLiteral("Outline Noise"), number(0.15));

        QFormLayout* render = editor::addSection(v, QStringLiteral("Render"));
        render->addRow(QStringLiteral("Exposure"), number(0.85));
        render->addRow(QStringLiteral("Quantum Bloom"), checkBox(false, QStringLiteral("Off")));

        scroll->setWidget(page);
        outer->addWidget(scroll, 1);

        editor::InspectorFilter rule(filter);
        filter->setText(shotQuery.isEmpty() ? QStringLiteral("shadow") : shotQuery);
        const size_t left = rule.apply(page);
        std::printf("screenshot-inspector: \"%s\" leaves %zu row(s)\n", qPrintable(filter->text()), left);

        holder.resize(360, 520);
        holder.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        // The theme's own background, so the figure looks like the dock rather than like a widget on white.
        holder.setAutoFillBackground(true);
        holder.setStyleSheet(QStringLiteral("QWidget#inspectorShot { background: #2b2b2b; }"));
        QCoreApplication::processEvents();
        const QPixmap shot = holder.grab();
        holder.hide();
        if (shotOut.isEmpty() || !shot.save(shotOut)) {
            std::fprintf(stderr, "screenshot-inspector: could not write %s\n", qPrintable(shotOut));
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
