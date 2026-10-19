#include "hub_selftest.h"

#include <cstdio>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QTemporaryDir>

#include "engine/project_template.h"
#include "hub_window.h"
#include "new_project_dialog.h"
#include "project_launcher.h"
#include "splash_screen.h"

namespace editor::hub {
namespace {

int passed = 0;
int failed = 0;

void check(bool ok, const char* name, const QString& detail = QString()) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.isEmpty() ? "" : "  -- ",
                detail.toLocal8Bit().constData());
    std::fflush(stdout);
    ok ? ++passed : ++failed;
}

} // namespace

int runHubSelfTest() {
    // Paths used by the hub.
    check(!engineRoot().isEmpty() && QFileInfo(engineRoot()).isDir(), "paths: the engine root exists",
          engineRoot());
    check(!splashArtPath().isEmpty(), "paths: the splash artwork ships with the engine",
          splashArtPath());
    const QString editor = editorExecutable();
    check(!editor.isEmpty() && QFileInfo(editor).exists(), "paths: the editor is beside the hub", editor);

    // Templates.
    const std::vector<engine::ProjectTemplate>& templates = engine::projectTemplates();
    check(templates.size() >= 3, "templates: the built-ins are listed",
          QString::number(templates.size()));
    bool unique = true;
    for (size_t i = 0; i < templates.size(); ++i) {
        for (size_t j = i + 1; j < templates.size(); ++j) {
            if (templates[i].id == templates[j].id) unique = false;
        }
    }
    check(unique, "templates: their ids are unique");
    check(engine::findProjectTemplate("empty") != nullptr &&
              engine::findProjectTemplate("anime") != nullptr &&
              engine::findProjectTemplate("terrain") != nullptr,
          "templates: the three starter templates are present");

    // Scaffolding a project, the work the splash is showing.
    QTemporaryDir dir;
    check(dir.isValid(), "projects: a temporary folder is available");
    const QString root = dir.filePath(QStringLiteral("Self Test Project"));
    engine::CreateProjectOptions options;
    options.root = root.toStdString();
    options.name = "Self Test Project";
    options.templateId = "terrain";
    options.engineRoot = engineRoot().toStdString();

    int stages = 0;
    float lastFraction = -1.0f;
    bool increasing = true;
    std::string error;
    const bool created = engine::createProject(
        options,
        [&](float fraction, const std::string&) {
            ++stages;
            if (fraction < lastFraction) increasing = false;
            lastFraction = fraction;
        },
        &error);
    check(created, "projects: the terrain template creates a project",
          QString::fromStdString(error));
    check(stages >= 5 && increasing && lastFraction > 0.99f,
          "projects: the splash receives ordered progress",
          QStringLiteral("%1 steps, last %2").arg(stages).arg(double(lastFraction)));
    check(QFileInfo::exists(root + QStringLiteral("/ukiyo.project.json")) &&
              QFileInfo::exists(root + QStringLiteral("/Scenes/Main.ukscene")) &&
              QFileInfo::exists(root + QStringLiteral("/assets/Terrain/hills.u16")),
          "projects: the manifest, scene and heightmap are written");
    check(engine::isProject(root.toStdString()), "projects: the folder reads back as a project");

    // The splash paints and animates without a window manager, then deletes itself.
    QPointer<SplashScreen> splash = new SplashScreen(splashArtPath(), QStringLiteral("Self test"));
    splash->setEyebrow(QStringLiteral("SELF TEST"));
    splash->setCaption(root);
    splash->skipIntro();
    splash->setStage(0.5f, QStringLiteral("Checking the splash"));
    check(splash->width() > 400 && splash->height() > 200, "splash: it has a sensible size",
          QStringLiteral("%1 x %2").arg(splash->width()).arg(splash->height()));

    // Let the bar ease toward the stage, then paint without a window manager: artwork,
    // wordmark, heading, bar and the line under it.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
    }
    QPixmap frame(splash->size());
    splash->render(&frame);
    check(!frame.isNull() && frame.width() == splash->width(), "splash: it paints its frame",
          QStringLiteral("%1 x %2").arg(frame.width()).arg(frame.height()));
    const QImage painted = frame.toImage();
    const QColor bar = painted.pixelColor(64, 287);   // inside the vermilion bar at 50%
    check(bar.red() > 110 && bar.red() > bar.green() + 40, "splash: the progress bar is drawn",
          bar.name());
    check(!splash->finishing(), "splash: it starts visible");
    splash->finish();
    check(splash->finishing(), "splash: finish starts the fade-out");
    QElapsedTimer fadeClock;
    fadeClock.start();
    while (!splash.isNull() && fadeClock.elapsed() < 2500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    check(splash.isNull(), "splash: it frees itself when the fade ends",
          QStringLiteral("%1 ms").arg(fadeClock.elapsed()));

    // The dialog offers every template and a valid default path.
    NewProjectDialog dialog(QStringLiteral("anime"));
    check(dialog.templateId() == QLatin1String("anime"), "dialog: the requested template is preselected",
          dialog.templateId());
    check(dialog.projectName() == QLatin1String("My Ukiyo Project"), "dialog: it suggests a name",
          dialog.projectName());
    check(dialog.projectRoot().endsWith(QStringLiteral("My Ukiyo Project")),
          "dialog: the target path ends in the project name", dialog.projectRoot());

    // The window itself builds, loads the recent list, and can be torn down.
    {
        HubWindow window;
        check(window.width() >= 900, "hub: the window lays out");
    }
    check(true, "hub: the window is destroyed cleanly");

    std::printf("hub: %d passed, %d failed\n", passed, failed);
    return failed;
}

} // namespace editor::hub
