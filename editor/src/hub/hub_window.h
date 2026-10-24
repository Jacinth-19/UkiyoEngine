// The Ukiyo Hub: a launcher for engine projects, in the spirit of Unity Hub. It lists recent
// projects, creates new ones from the built-in templates, and opens everything in the World Editor.
// Creating or opening a project shows the splash screen while the work happens.
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace editor::hub {

struct RecentProject {
    QString root;
    QString name;
    QString templateId;
    qint64 opened = 0;   // milliseconds since the epoch
};

class HubWindow : public QWidget {
public:
    explicit HubWindow(QWidget* parent = nullptr);
    ~HubWindow() override;

    // Opens the new-project dialog, optionally with a template already chosen.
    void newProject(const QString& templateId = QString());
    // Opens a project in the editor, showing the splash while it checks the folder.
    void openProject(const QString& root);

    // Renders every page to `<dir>/hub_<page>.png`. Used by `ukiyo_hub --screenshots` on CI, where the offscreen
    // platform plugin can grab the widgets without a display, and by anyone who wants the launcher in the docs.
    void capturePages(const QString& dir);

private:
    QWidget* buildRail();
    QWidget* buildProjectsPage();
    QWidget* buildTemplatesPage();
    QWidget* buildAboutPage();
    QWidget* buildEmptyState();

    void selectPage(int index);
    void loadRecent();
    void saveRecent();
    void refreshProjects();
    void remember(const QString& root, const QString& name, const QString& templateId);
    void removeProject(const QString& root);
    void openProjectDialog();
    void createProject(const QString& root, const QString& name, const QString& templateId);
    void showLaunchError(const QString& message);

    QStackedWidget* pages_ = nullptr;
    QVBoxLayout* projectList_ = nullptr;
    QLabel* projectCount_ = nullptr;
    QLineEdit* search_ = nullptr;
    QVector<QPushButton*> navButtons_;
    QVector<RecentProject> recent_;
};

// Renders the startup splash to `<dir>/splash_mark.png` (beat 1, the mark) and `<dir>/splash.png` (beat 2, the
// artwork with the lockup and a half-finished status block). Used by `ukiyo_hub --splash-shots`, so the splash in
// the docs is the program's own output at the commit being read, not a mockup.
int captureSplash(const QString& dir);

} // namespace editor::hub
