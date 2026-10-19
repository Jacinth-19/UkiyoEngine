#include "hub_window.h"

#include <algorithm>
#include <thread>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include "engine/project_template.h"
#include "icons.h"
#include "new_project_dialog.h"
#include "project_launcher.h"
#include "splash_screen.h"

namespace editor::hub {
namespace {

const char* const kRecentKey = "recent/projects";
constexpr int kRecentLimit = 12;

QString relativeTime(qint64 milliseconds) {
    if (milliseconds <= 0) return QStringLiteral("never");
    const QDateTime then = QDateTime::fromMSecsSinceEpoch(milliseconds);
    const qint64 seconds = then.secsTo(QDateTime::currentDateTime());
    if (seconds < 90) return QStringLiteral("just now");
    if (seconds < 3600) return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 86400) {
        const qint64 hours = seconds / 3600;
        return hours == 1 ? QStringLiteral("1 hour ago") : QStringLiteral("%1 hours ago").arg(hours);
    }
    if (seconds < 7 * 86400) {
        const qint64 days = seconds / 86400;
        return days == 1 ? QStringLiteral("yesterday") : QStringLiteral("%1 days ago").arg(days);
    }
    return then.toString(QStringLiteral("MMM d, yyyy"));
}

QString accentFor(const QString& templateId) {
    if (const engine::ProjectTemplate* projectTemplate = engine::findProjectTemplate(templateId.toStdString())) {
        return QString::fromStdString(projectTemplate->accent);
    }
    return QStringLiteral("#cc4832");
}

QString templateNameFor(const QString& templateId) {
    if (const engine::ProjectTemplate* projectTemplate = engine::findProjectTemplate(templateId.toStdString())) {
        return QString::fromStdString(projectTemplate->name);
    }
    return QStringLiteral("Project");
}

QLabel* makeLabel(const QString& text, const char* objectName) {
    auto* label = new QLabel(text);
    label->setObjectName(QString::fromLatin1(objectName));
    return label;
}

// A frame that reports a left click, so the whole project card is a target while its buttons
// still work on their own.
class ClickableFrame : public QFrame {
public:
    explicit ClickableFrame(QWidget* parent = nullptr) : QFrame(parent) {}
    std::function<void()> onClick;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClick) onClick();
        QFrame::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClick) onClick();
        QFrame::mouseDoubleClickEvent(event);
    }
};

} // namespace

HubWindow::HubWindow(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("hubWindow"));
    setWindowTitle(QStringLiteral("Ukiyo Hub"));
    resize(1200, 780);
    setMinimumSize(980, 640);
    loadRecent();

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildRail());

    pages_ = new QStackedWidget;
    pages_->setObjectName(QStringLiteral("hubPages"));
    pages_->addWidget(buildProjectsPage());
    pages_->addWidget(buildTemplatesPage());
    pages_->addWidget(buildAboutPage());
    root->addWidget(pages_, 1);

    selectPage(0);
    refreshProjects();
}

HubWindow::~HubWindow() = default;

QWidget* HubWindow::buildRail() {
    auto* rail = new QWidget;
    rail->setObjectName(QStringLiteral("hubRail"));
    rail->setFixedWidth(236);

    auto* column = new QVBoxLayout(rail);
    column->setContentsMargins(22, 26, 18, 20);
    column->setSpacing(6);

    // The mark and the wordmark read as one lockup. At this size the ring's gap closes up and the ensō reads as a
    // letter C, so the rail carries the hanko instead: a block of red pigment still reads as a stamp at 26 px, and it
    // is the one accent the rest of the interface borrows.
    auto* brand = new QWidget;
    brand->setObjectName(QStringLiteral("hubBrand"));
    brand->setAttribute(Qt::WA_StyledBackground, false);
    auto* brandRow = new QHBoxLayout(brand);
    brandRow->setContentsMargins(0, 0, 0, 0);
    brandRow->setSpacing(11);
    auto* mark = new QLabel;
    mark->setObjectName(QStringLiteral("hubBrandMark"));
    mark->setAttribute(Qt::WA_TranslucentBackground, true);
    mark->setStyleSheet(QStringLiteral("background: transparent;"));
    QPixmap markPixmap(engineRoot() + QStringLiteral("/editor/res/brand/ukiyo-seal-64.png"));
    if (markPixmap.isNull()) markPixmap.load(engineRoot() + QStringLiteral("/editor/res/brand/ukiyo-mark-64.png"));
    if (!markPixmap.isNull()) {
        const int px = 26;
        mark->setPixmap(markPixmap.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        mark->setFixedSize(px, px);
    }
    brandRow->addWidget(mark, 0, Qt::AlignVCenter);

    auto* titles = new QVBoxLayout;
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(0);

    auto* wordmark = new QLabel(QStringLiteral("UKIYO"));
    wordmark->setObjectName(QStringLiteral("hubWordmark"));
    QFont markFont = wordmark->font();
    markFont.setPixelSize(25);
    markFont.setWeight(QFont::Bold);
    markFont.setLetterSpacing(QFont::AbsoluteSpacing, 3.2);
    wordmark->setFont(markFont);
    titles->addWidget(wordmark);

    auto* sub = new QLabel(QStringLiteral("ENGINE"));
    sub->setObjectName(QStringLiteral("hubWordmarkSub"));
    QFont subFont = sub->font();
    subFont.setPixelSize(9);
    subFont.setWeight(QFont::Medium);
    subFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.6);
    sub->setFont(subFont);
    titles->addWidget(sub);
    brandRow->addLayout(titles);
    column->addWidget(brand);

    // The name in Japanese, under the lockup: the engine is English-first, but the face carries both.
    auto* jp = new QLabel(QStringLiteral("浮世エンジン"));
    jp->setObjectName(QStringLiteral("hubWordmarkJp"));
    QFont jpFont = jp->font();
    jpFont.setPixelSize(10);
    jpFont.setWeight(QFont::Medium);
    jpFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.6);
    jp->setFont(jpFont);
    column->addSpacing(6);
    column->addWidget(jp);

    auto* rule = new QFrame;
    rule->setFixedSize(26, 2);
    rule->setStyleSheet(QStringLiteral("background:#cc4832; border-radius:1px;"));
    column->addSpacing(16);
    column->addWidget(rule);
    column->addSpacing(16);

    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    struct NavEntry {
        const char* icon;
        const char* text;
        const char* hint;
    };
    const NavEntry entries[] = {
        {"folder_open", "Projects", "Recent projects and new ones"},
        {"grid_view", "Templates", "Starting points that ship with the engine"},
        {"info", "About", "What this engine is"},
    };
    for (int i = 0; i < 3; ++i) {
        auto* button = new QPushButton(QString::fromLatin1(entries[i].text));
        button->setObjectName(QStringLiteral("hubNav"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setIcon(editor::materialIcon(entries[i].icon, QColor(0xc4, 0xb8, 0xa6), 16));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(QString::fromLatin1(entries[i].hint));
        button->setToolTipDuration(4000);
        group->addButton(button);
        navButtons_.push_back(button);
        column->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i] { selectPage(i); });
    }

    column->addStretch(1);

    auto* engine = makeLabel(QStringLiteral("ENGINE"), "hubRailCaption");
    column->addWidget(engine);
    auto* enginePath = makeLabel(QDir::toNativeSeparators(engineRoot()), "hubRailPath");
    enginePath->setWordWrap(true);
    column->addWidget(enginePath);
    auto* version = makeLabel(QStringLiteral("v0.1  ·  world editor inside"), "hubRailPath");
    column->addWidget(version);
    return rail;
}

QWidget* HubWindow::buildProjectsPage() {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("hubPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 20);
    layout->setSpacing(16);

    auto* header = new QHBoxLayout;
    header->setSpacing(10);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(makeLabel(QStringLiteral("Projects"), "hubPageTitle"));
    auto* subtitle = makeLabel(
        QStringLiteral("Pick up where you left off, or start something new. Everything here opens "
                       "in the Ukiyo World Editor."),
        "hubPageSubtitle");
    subtitle->setWordWrap(true);
    titles->addWidget(subtitle);
    projectCount_ = makeLabel(QStringLiteral("No projects yet"), "hubCount");
    titles->addWidget(projectCount_);
    header->addLayout(titles, 1);

    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("hubSearch"));
    search_->setPlaceholderText(QStringLiteral("Search projects"));
    search_->setClearButtonEnabled(true);
    search_->setFixedWidth(210);
    connect(search_, &QLineEdit::textChanged, this, [this] { refreshProjects(); });
    header->addWidget(search_, 0, Qt::AlignBottom);

    auto* openFolder = new QPushButton(QStringLiteral("Open folder"));
    openFolder->setObjectName(QStringLiteral("hubSecondary"));
    openFolder->setIcon(editor::materialIcon("folder_open", editor::iconColor()));
    connect(openFolder, &QPushButton::clicked, this, &HubWindow::openProjectDialog);
    header->addWidget(openFolder, 0, Qt::AlignBottom);

    auto* create = new QPushButton(QStringLiteral("New project"));
    create->setObjectName(QStringLiteral("hubPrimary"));
    create->setIcon(editor::materialIcon("add", QColor(255, 255, 255)));
    connect(create, &QPushButton::clicked, this, [this] { newProject(); });
    header->addWidget(create, 0, Qt::AlignBottom);

    layout->addLayout(header);

    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("hubScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    host->setObjectName(QStringLiteral("hubListHost"));
    projectList_ = new QVBoxLayout(host);
    projectList_->setContentsMargins(0, 0, 6, 0);
    projectList_->setSpacing(10);
    projectList_->addStretch(1);
    scroll->setWidget(host);
    layout->addWidget(scroll, 1);

    return page;
}

QWidget* HubWindow::buildTemplatesPage() {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("hubPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 24);
    layout->setSpacing(16);

    layout->addWidget(makeLabel(QStringLiteral("Templates"), "hubPageTitle"));
    auto* subtitle = makeLabel(
        QStringLiteral("Every project starts as one of these. The files are plain text and live in "
                       "the project, so a template is only a beginning."),
        "hubPageSubtitle");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto* row = new QHBoxLayout;
    row->setSpacing(14);
    for (const engine::ProjectTemplate& projectTemplate : engine::projectTemplates()) {
        const QString id = QString::fromStdString(projectTemplate.id);
        auto* card = new QFrame;
        card->setObjectName(QStringLiteral("hubTemplateCard"));
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* column = new QVBoxLayout(card);
        column->setContentsMargins(18, 16, 18, 16);
        column->setSpacing(8);

        auto* accent = new QFrame;
        accent->setFixedHeight(3);
        accent->setStyleSheet(QStringLiteral("background:%1; border-radius:1px;")
                                  .arg(QString::fromStdString(projectTemplate.accent)));
        column->addWidget(accent);

        auto* name = makeLabel(QString::fromStdString(projectTemplate.name), "hubTemplateName");
        column->addWidget(name);
        auto* tagline = makeLabel(QString::fromStdString(projectTemplate.tagline), "hubTemplateTagline");
        tagline->setWordWrap(true);
        column->addWidget(tagline);
        auto* description = makeLabel(QString::fromStdString(projectTemplate.description), "hubTemplateText");
        description->setWordWrap(true);
        column->addWidget(description);

        for (const std::string& bullet : projectTemplate.bullets) {
            auto* line = new QHBoxLayout;
            line->setSpacing(7);
            auto* tick = new QLabel;
            tick->setPixmap(editor::materialIcon("check_circle", QColor(0x74, 0xc0, 0x9a), 14).pixmap(14, 14));
            line->addWidget(tick, 0, Qt::AlignTop);
            auto* text = makeLabel(QString::fromStdString(bullet), "hubTemplateBullet");
            text->setWordWrap(true);
            line->addWidget(text, 1);
            column->addLayout(line);
        }

        column->addStretch(1);
        auto* use = new QPushButton(QStringLiteral("Use this template"));
        use->setObjectName(QStringLiteral("hubSecondary"));
        connect(use, &QPushButton::clicked, this, [this, id] { newProject(id); });
        column->addWidget(use);

        row->addWidget(card, 1);
    }
    layout->addLayout(row);

    auto* hint = makeLabel(
        QStringLiteral("Templates are written by engine::createProject (src/engine/project_template.cpp). "
                       "Add one there and it appears here and in the new-project dialog."),
        "hubHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);
    return page;
}

QWidget* HubWindow::buildAboutPage() {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("hubPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 24);
    layout->setSpacing(14);

    layout->addWidget(makeLabel(QStringLiteral("Ukiyo Engine"), "hubPageTitle"));
    auto* subtitle = makeLabel(
        QStringLiteral("A Vulkan engine and world editor for stylised anime characters and worlds."),
        "hubPageSubtitle");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto* body = makeLabel(
        QStringLiteral(
            "Ukiyo is a small engine built around a toon and anime pipeline: the Custom Anime "
            "tonemap, outlines, a face SDF, and a shader graph that writes .ukshader files. "
            "Terrain, foliage and water are part of the editor, with physics through Jolt, "
            "navigation through Recast and Detour, Lua scripting, particles, audio through "
            "miniaudio, and UDP networking with reliable sessions.\n\n"
            "Projects are folders. This hub creates one from a template, records it in "
            "ukiyo.project.json, and hands it to the editor with --project. Nothing is installed "
            "and nothing is downloaded."),
        "hubAboutBody");
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* grid = new QHBoxLayout;
    grid->setSpacing(12);
    const char* features[][2] = {
        {"Anime pipeline", "Toon shading, outlines, face SDF and a node-based shader graph."},
        {"World building", "Terrain with layers and foliage, water with Gerstner waves, prefabs."},
        {"Game systems", "Jolt physics, Recast navigation, Lua scripts, particles and audio."},
        {"Networking", "UDP client and server sessions with reliable, ordered messages."},
    };
    for (const auto& feature : features) {
        auto* card = new QFrame;
        card->setObjectName(QStringLiteral("hubFeatureCard"));
        auto* column = new QVBoxLayout(card);
        column->setContentsMargins(14, 12, 14, 12);
        column->setSpacing(4);
        column->addWidget(makeLabel(QString::fromLatin1(feature[0]), "hubFeatureName"));
        auto* text = makeLabel(QString::fromLatin1(feature[1]), "hubFeatureText");
        text->setWordWrap(true);
        column->addWidget(text);
        column->addStretch(1);
        grid->addWidget(card, 1);
    }
    layout->addLayout(grid);

    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    auto* openEngine = new QPushButton(QStringLiteral("Open engine folder"));
    openEngine->setObjectName(QStringLiteral("hubSecondary"));
    openEngine->setIcon(editor::materialIcon("folder_open", editor::iconColor()));
    connect(openEngine, &QPushButton::clicked, this, [this] { revealFolder(engineRoot()); });
    buttons->addWidget(openEngine);

    auto* openProjects = new QPushButton(QStringLiteral("Open projects folder"));
    openProjects->setObjectName(QStringLiteral("hubSecondary"));
    openProjects->setIcon(editor::materialIcon("folder", editor::iconColor()));
    connect(openProjects, &QPushButton::clicked, this, [this] {
        if (recent_.isEmpty()) {
            revealFolder(QDir::homePath());
            return;
        }
        revealFolder(recent_.front().root);
    });
    buttons->addWidget(openProjects);
    buttons->addStretch(1);

    auto* version = makeLabel(QStringLiteral("v0.1  ·  built on the engine in this checkout"), "hubHint");
    buttons->addWidget(version);
    layout->addLayout(buttons);

    return page;
}

QWidget* HubWindow::buildEmptyState() {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("hubEmpty"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(28, 26, 28, 26);
    layout->setSpacing(10);

    auto* art = new QLabel;
    const QString artPath = splashArtPath();
    if (!artPath.isEmpty()) {
        QPixmap shot(artPath);
        art->setPixmap(shot.scaled(420, 236, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        art->setFixedHeight(200);
        art->setScaledContents(true);
        art->setObjectName(QStringLiteral("hubEmptyArt"));
    }
    layout->addWidget(art);

    layout->addWidget(makeLabel(QStringLiteral("No projects yet"), "hubEmptyTitle"));
    auto* text = makeLabel(
        QStringLiteral("Start with a template: a clean scene, an anime character, or a terrain tile "
                       "with a lake. The hub writes the project and opens it in the editor."),
        "hubEmptyText");
    text->setWordWrap(true);
    layout->addWidget(text);

    auto* create = new QPushButton(QStringLiteral("Create your first project"));
    create->setObjectName(QStringLiteral("hubPrimary"));
    create->setIcon(editor::materialIcon("add", QColor(255, 255, 255)));
    connect(create, &QPushButton::clicked, this, [this] { newProject(); });
    layout->addWidget(create, 0, Qt::AlignLeft);
    return frame;
}

void HubWindow::capturePages(const QString& dir) {
    QDir().mkpath(dir);
    resize(1280, 800);
    show();
    static const char* names[] = {"projects", "templates", "about"};
    for (int i = 0; i < pages_->count(); ++i) {
        selectPage(i);
        // Two turns of the event loop: the page's lazy rows and the first paint both land before the grab.
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const QString path = dir + QStringLiteral("/hub_") + QString::fromLatin1(i < 3 ? names[i] : "page") +
                             QStringLiteral(".png");
        if (!grab().save(path)) std::fprintf(stderr, "could not write %s\n", qUtf8Printable(path));
        else std::printf("wrote %s\n", qUtf8Printable(path));
    }
}

void HubWindow::selectPage(int index) {
    if (!pages_ || index < 0 || index >= pages_->count()) return;
    pages_->setCurrentIndex(index);
    for (int i = 0; i < navButtons_.size(); ++i) navButtons_[i]->setChecked(i == index);
}

// ---- Recently opened projects ----------------------------------------------------------------

void HubWindow::loadRecent() {
    recent_.clear();
    QSettings settings(QStringLiteral("Ukiyo"), QStringLiteral("UkiyoHub"));
    const QByteArray raw = settings.value(QString::fromLatin1(kRecentKey)).toByteArray();
    const QJsonArray array = QJsonDocument::fromJson(raw).array();
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        RecentProject project;
        project.root = object.value(QStringLiteral("root")).toString();
        project.name = object.value(QStringLiteral("name")).toString();
        project.templateId = object.value(QStringLiteral("template")).toString();
        project.opened = qint64(object.value(QStringLiteral("opened")).toDouble());
        if (project.root.isEmpty()) continue;
        if (project.name.isEmpty()) project.name = QFileInfo(project.root).fileName();
        const bool duplicate = std::any_of(recent_.begin(), recent_.end(), [&](const RecentProject& other) {
            return QDir::cleanPath(other.root) == QDir::cleanPath(project.root);
        });
        if (!duplicate) recent_.push_back(project);
    }
}

void HubWindow::saveRecent() {
    QJsonArray array;
    for (const RecentProject& project : recent_) {
        QJsonObject object;
        object[QStringLiteral("root")] = project.root;
        object[QStringLiteral("name")] = project.name;
        object[QStringLiteral("template")] = project.templateId;
        object[QStringLiteral("opened")] = double(project.opened);
        array.append(object);
    }
    QSettings settings(QStringLiteral("Ukiyo"), QStringLiteral("UkiyoHub"));
    settings.setValue(QString::fromLatin1(kRecentKey), QJsonDocument(array).toJson(QJsonDocument::Compact));
}

void HubWindow::remember(const QString& root, const QString& name, const QString& templateId) {
    const QString clean = QDir::cleanPath(root);
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const RecentProject& project) {
                                     return QDir::cleanPath(project.root) == clean;
                                 }),
                  recent_.end());
    RecentProject project;
    project.root = clean;
    project.name = name;
    project.templateId = templateId;
    project.opened = QDateTime::currentMSecsSinceEpoch();
    recent_.prepend(project);
    while (recent_.size() > kRecentLimit) recent_.removeLast();
    saveRecent();
    refreshProjects();
}

void HubWindow::removeProject(const QString& root) {
    const QString clean = QDir::cleanPath(root);
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const RecentProject& project) {
                                     return QDir::cleanPath(project.root) == clean;
                                 }),
                  recent_.end());
    saveRecent();
    refreshProjects();
}

void HubWindow::refreshProjects() {
    if (!projectList_) return;
    while (QLayoutItem* item = projectList_->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }

    const QString filter = search_ ? search_->text().trimmed() : QString();
    int shown = 0;
    bool matchedSomething = false;

    for (const RecentProject& project : recent_) {
        const bool matches = filter.isEmpty() ||
                             project.name.contains(filter, Qt::CaseInsensitive) ||
                             project.root.contains(filter, Qt::CaseInsensitive);
        if (!matches) continue;
        matchedSomething = true;

        const bool exists = QFileInfo(project.root).isDir();
        const bool valid = exists && engine::isProject(project.root.toStdString());

        auto* card = new ClickableFrame;
        card->setObjectName(QStringLiteral("hubCard"));
        card->setCursor(Qt::PointingHandCursor);
        card->setAttribute(Qt::WA_Hover, true);
        card->onClick = [this, root = project.root] { openProject(root); };

        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(0, 0, 12, 0);
        row->setSpacing(14);

        auto* accent = new QFrame;
        accent->setFixedWidth(4);
        accent->setStyleSheet(QStringLiteral("background:%1; border-radius:2px;")
                                  .arg(accentFor(project.templateId)));
        row->addWidget(accent);

        auto* content = new QVBoxLayout;
        content->setContentsMargins(0, 12, 0, 12);
        content->setSpacing(3);

        auto* titleRow = new QHBoxLayout;
        titleRow->setSpacing(8);
        auto* name = makeLabel(project.name, "hubCardName");
        titleRow->addWidget(name);
        auto* badge = makeLabel(templateNameFor(project.templateId), "hubBadge");
        titleRow->addWidget(badge);
        if (!exists) {
            auto* missing = makeLabel(QStringLiteral("Folder missing"), "hubBadgeMissing");
            titleRow->addWidget(missing);
        } else if (!valid) {
            auto* missing = makeLabel(QStringLiteral("Not a project"), "hubBadgeMissing");
            titleRow->addWidget(missing);
        }
        titleRow->addStretch(1);
        content->addLayout(titleRow);

        auto* path = makeLabel(QDir::toNativeSeparators(project.root), "hubCardPath");
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        content->addWidget(path);

        const QString opened = project.opened > 0
                                   ? QStringLiteral("Opened %1").arg(relativeTime(project.opened))
                                   : QStringLiteral("Never opened");
        auto* meta = makeLabel(opened + QStringLiteral("  ·  ") + templateNameFor(project.templateId),
                               "hubCardMeta");
        content->addWidget(meta);
        row->addLayout(content, 1);

        auto* open = new QPushButton(QStringLiteral("Open"));
        open->setObjectName(QStringLiteral("hubPrimarySmall"));
        connect(open, &QPushButton::clicked, this, [this, root = project.root] { openProject(root); });
        row->addWidget(open, 0, Qt::AlignVCenter);

        auto* more = new QToolButton;
        more->setObjectName(QStringLiteral("hubIconButton"));
        more->setIcon(editor::materialIcon("more_vert", editor::iconColor()));
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        QAction* openAction = menu->addAction(editor::materialIcon("play_arrow", editor::iconColor()),
                                              QStringLiteral("Open in the editor"));
        connect(openAction, &QAction::triggered, this, [this, root = project.root] { openProject(root); });
        QAction* revealAction = menu->addAction(editor::materialIcon("folder_open", editor::iconColor()),
                                                QStringLiteral("Show in the file manager"));
        connect(revealAction, &QAction::triggered, this,
                [root = project.root] { revealFolder(root); });
        menu->addSeparator();
        QAction* removeAction = menu->addAction(editor::materialIcon("delete", editor::iconColor()),
                                                QStringLiteral("Remove from this list"));
        connect(removeAction, &QAction::triggered, this,
                [this, root = project.root] { removeProject(root); });
        more->setMenu(menu);
        row->addWidget(more, 0, Qt::AlignVCenter);

        projectList_->addWidget(card);
        ++shown;
    }

    if (shown == 0) {
        if (recent_.isEmpty()) {
            projectList_->addWidget(buildEmptyState());
        } else if (!matchedSomething) {
            auto* none = makeLabel(QStringLiteral("No project matches \"%1\".").arg(filter), "hubCount");
            projectList_->addWidget(none);
        }
    }
    projectList_->addStretch(1);

    if (projectCount_) {
        if (recent_.isEmpty()) projectCount_->setText(QStringLiteral("No projects yet"));
        else if (shown == recent_.size())
            projectCount_->setText(shown == 1 ? QStringLiteral("1 project")
                                              : QStringLiteral("%1 projects").arg(shown));
        else
            projectCount_->setText(QStringLiteral("%1 of %2 projects").arg(shown).arg(recent_.size()));
    }
}

// ---- Creating and opening --------------------------------------------------------------------

void HubWindow::newProject(const QString& templateId) {
    NewProjectDialog dialog(templateId, this);
    if (dialog.exec() != QDialog::Accepted) return;
    createProject(dialog.projectRoot(), dialog.projectName(), dialog.templateId());
}

void HubWindow::createProject(const QString& root, const QString& name, const QString& templateId) {
    auto* splash = new SplashScreen(splashArtPath(), name);
    splash->setEyebrow(QStringLiteral("CREATING PROJECT"));
    splash->setStage(0.02f, QStringLiteral("Preparing the project"));
    splash->setCaption(QDir::toNativeSeparators(root));
    splash->show();

    QPointer<SplashScreen> splashGuard(splash);
    QPointer<HubWindow> self(this);

    engine::CreateProjectOptions options;
    options.root = root.toStdString();
    options.name = name.toStdString();
    options.templateId = templateId.toStdString();
    options.engineRoot = engineRoot().toStdString();

    std::thread([splashGuard, self, options, root, name, templateId] {
        std::string error;
        const bool created = engine::createProject(
            options,
            [splashGuard](float fraction, const std::string& message) {
                const QString line = QString::fromStdString(message);
                QMetaObject::invokeMethod(qApp, [splashGuard, fraction, line] {
                    if (splashGuard) splashGuard->setStage(fraction, line);
                }, Qt::QueuedConnection);
            },
            &error);
        const QString reason = QString::fromStdString(error);
        QMetaObject::invokeMethod(qApp, [self, splashGuard, created, reason, root, name, templateId] {
            if (!self) return;
            if (!created) {
                if (splashGuard) splashGuard->finish();
                QMessageBox::critical(self, QStringLiteral("Ukiyo Hub"),
                                      QStringLiteral("The project could not be created:\n\n") + reason);
                return;
            }
            self->remember(root, name, templateId);
            if (splashGuard) splashGuard->setStage(1.0f, QStringLiteral("Opening the editor"));
            QString launchError;
            if (!launchEditor(root, &launchError)) {
                if (splashGuard) splashGuard->finish();
                self->showLaunchError(launchError);
                return;
            }
            QTimer::singleShot(650, self.data(), [splashGuard] {
                if (splashGuard) splashGuard->finish();
            });
        }, Qt::QueuedConnection);
    }).detach();
}

void HubWindow::openProjectDialog() {
    const QString start = recent_.isEmpty() ? QDir::homePath() : recent_.front().root;
    const QString root = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open a Ukiyo project"), start);
    if (root.isEmpty()) return;

    if (!engine::isProject(root.toStdString())) {
        QMessageBox::warning(
            this, QStringLiteral("Ukiyo Hub"),
            QStringLiteral("That folder is not a Ukiyo project yet.\n\nA project has "
                           "ukiyo.project.json and the scene it names. Pick the folder that holds "
                           "them, or create a new project from a template."));
        return;
    }
    openProject(root);
}

void HubWindow::openProject(const QString& root) {
    if (!QFileInfo(root).isDir()) {
        QMessageBox::warning(this, QStringLiteral("Ukiyo Hub"),
                             QStringLiteral("That project folder is no longer there:\n\n") + root);
        return;
    }

    engine::ProjectManifest manifest;
    std::string error;
    const bool readable = engine::loadProjectManifest(root.toStdString(), &manifest, &error);
    if (!readable) {
        QMessageBox::warning(
            this, QStringLiteral("Ukiyo Hub"),
            QStringLiteral("This folder is not a Ukiyo project:\n\n") + QString::fromStdString(error));
        return;
    }

    const QString name = manifest.name.empty() ? QFileInfo(root).fileName()
                                               : QString::fromStdString(manifest.name);
    auto* splash = new SplashScreen(splashArtPath(), name);
    splash->setEyebrow(QStringLiteral("OPENING PROJECT"));
    splash->setStage(0.15f, QStringLiteral("Reading the project manifest"));
    splash->setCaption(QDir::toNativeSeparators(root));
    splash->show();
    QPointer<SplashScreen> splashGuard(splash);

    QCoreApplication::processEvents();

    if (!engine::isProject(root.toStdString())) {
        splashGuard->finish();
        QMessageBox::warning(
            this, QStringLiteral("Ukiyo Hub"),
            QStringLiteral("The main scene is missing:\n\n") +
                QDir::toNativeSeparators(QDir(root).absoluteFilePath(QString::fromStdString(manifest.mainScene))) +
                QStringLiteral("\n\nOpen the project folder and check Scenes/."));
        return;
    }

    splashGuard->setStage(0.62f, QStringLiteral("Checking the main scene"));
    splashGuard->setStage(0.86f, QStringLiteral("Starting the Ukiyo editor"));
    remember(root, name, QString::fromStdString(manifest.templateId));

    QPointer<QWidget> self(this);
    QTimer::singleShot(140, this, [self, splashGuard, root] {
        if (!self) return;
        QString launchError;
        if (!launchEditor(root, &launchError)) {
            if (splashGuard) splashGuard->finish();
            QMessageBox::warning(self, QStringLiteral("Ukiyo Hub"), launchError);
        }
    });
    QTimer::singleShot(820, this, [splashGuard] {
        if (splashGuard) splashGuard->finish();
    });
}

void HubWindow::showLaunchError(const QString& message) {
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Ukiyo Hub"), message,
                    QMessageBox::Ok, this);
    box.setInformativeText(QStringLiteral(
        "The project itself is ready. You can open its folder from the project card's menu."));
    box.exec();
}

} // namespace editor::hub
