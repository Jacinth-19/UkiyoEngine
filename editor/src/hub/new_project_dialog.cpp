#include "new_project_dialog.h"

#include <QButtonGroup>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "engine/project_template.h"
#include "icons.h"

namespace editor::hub {
namespace {

QString defaultLocation() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty()) base = QDir::homePath();
    return QDir::cleanPath(base + QStringLiteral("/Ukiyo Projects"));
}

} // namespace

NewProjectDialog::NewProjectDialog(const QString& preselectedTemplate, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("New Ukiyo project"));
    setModal(true);
    resize(820, 590);
    setObjectName(QStringLiteral("newProjectDialog"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(26, 22, 26, 20);
    layout->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("New project"));
    title->setObjectName(QStringLiteral("hubPageTitle"));
    layout->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral(
        "Give the project a name and a folder, then pick a starting point. The project is built "
        "in place; nothing is downloaded."));
    subtitle->setObjectName(QStringLiteral("hubPageSubtitle"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto* body = new QHBoxLayout;
    body->setSpacing(22);
    layout->addLayout(body, 1);

    // Left: the name, the folder, and the exact path that will be created.
    auto* form = new QVBoxLayout;
    form->setSpacing(8);
    body->addLayout(form, 3);

    auto* nameLabel = new QLabel(QStringLiteral("PROJECT NAME"));
    nameLabel->setObjectName(QStringLiteral("hubFieldLabel"));
    form->addWidget(nameLabel);
    name_ = new QLineEdit(QStringLiteral("My Ukiyo Project"));
    name_->setObjectName(QStringLiteral("hubNameField"));
    name_->setMaxLength(64);
    form->addWidget(name_);

    auto* locationLabel = new QLabel(QStringLiteral("LOCATION"));
    locationLabel->setObjectName(QStringLiteral("hubFieldLabel"));
    form->addWidget(locationLabel);
    auto* locationRow = new QHBoxLayout;
    locationRow->setSpacing(8);
    location_ = new QLineEdit(defaultLocation());
    locationRow->addWidget(location_, 1);
    auto* browseButton = new QPushButton(QStringLiteral("Browse..."));
    browseButton->setObjectName(QStringLiteral("hubSecondary"));
    locationRow->addWidget(browseButton);
    form->addLayout(locationRow);

    auto* pathLabel = new QLabel(QStringLiteral("THE PROJECT WILL BE CREATED AT"));
    pathLabel->setObjectName(QStringLiteral("hubFieldLabel"));
    form->addWidget(pathLabel);
    preview_ = new QLabel;
    preview_->setObjectName(QStringLiteral("hubPathPreview"));
    preview_->setWordWrap(true);
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addWidget(preview_);

    auto* note = new QLabel(QStringLiteral(
        "Scenes, ProjectSettings and the starter script are written into the folder. Template art "
        "stays with the engine, so the project stays small."));
    note->setObjectName(QStringLiteral("hubHint"));
    note->setWordWrap(true);
    form->addWidget(note);
    form->addStretch(1);

    // Right: the templates.
    auto* templateColumn = new QVBoxLayout;
    templateColumn->setSpacing(8);
    body->addLayout(templateColumn, 4);

    auto* templateLabel = new QLabel(QStringLiteral("START FROM"));
    templateLabel->setObjectName(QStringLiteral("hubFieldLabel"));
    templateColumn->addWidget(templateLabel);

    auto* templateGroup = new QButtonGroup(this);
    templateGroup->setExclusive(true);

    for (const engine::ProjectTemplate& projectTemplate : engine::projectTemplates()) {
        const QString id = QString::fromStdString(projectTemplate.id);
        auto* button = new QPushButton;
        button->setObjectName(QStringLiteral("templateChoice"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("templateId", id);
        templateGroup->addButton(button);

        auto* card = new QHBoxLayout(button);
        card->setContentsMargins(0, 0, 12, 0);
        card->setSpacing(12);

        auto* accent = new QFrame;
        accent->setFixedWidth(4);
        accent->setStyleSheet(QStringLiteral("background:%1; border-radius:2px;")
                                  .arg(QString::fromStdString(projectTemplate.accent)));
        card->addWidget(accent);

        auto* text = new QVBoxLayout;
        text->setContentsMargins(0, 9, 0, 9);
        text->setSpacing(2);
        auto* name = new QLabel(QString::fromStdString(projectTemplate.name));
        name->setObjectName(QStringLiteral("templateChoiceName"));
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->addWidget(name);
        auto* tagline = new QLabel(QString::fromStdString(projectTemplate.tagline));
        tagline->setObjectName(QStringLiteral("templateChoiceTagline"));
        tagline->setWordWrap(true);
        tagline->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->addWidget(tagline);
        card->addLayout(text, 1);

        templateColumn->addWidget(button);
        templateButtons_.push_back(button);
        connect(button, &QPushButton::clicked, this, [this, id] { chooseTemplate(id); });
    }
    templateColumn->addStretch(1);

    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("hubError"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    auto* create = new QPushButton(QStringLiteral("Create project"));
    create->setObjectName(QStringLiteral("hubPrimary"));
    create->setDefault(true);
    create->setIcon(editor::materialIcon("add", QColor(255, 255, 255)));
    connect(create, &QPushButton::clicked, this, &NewProjectDialog::accept);
    buttons->addWidget(create);
    layout->addLayout(buttons);

    connect(browseButton, &QPushButton::clicked, this, &NewProjectDialog::browse);
    connect(name_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    connect(location_, &QLineEdit::textChanged, this, [this] { updatePreview(); });

    if (!preselectedTemplate.isEmpty() && engine::findProjectTemplate(preselectedTemplate.toStdString())) {
        chooseTemplate(preselectedTemplate);
    } else if (!templateButtons_.isEmpty()) {
        chooseTemplate(templateButtons_.front()->property("templateId").toString());
    }
    name_->setFocus();
    name_->selectAll();
    updatePreview();
}

QString NewProjectDialog::projectName() const { return name_->text().trimmed(); }

QString NewProjectDialog::projectRoot() const {
    return QDir::cleanPath(location_->text().trimmed() + QLatin1Char('/') + projectName());
}

QString NewProjectDialog::templateId() const { return templateId_; }

void NewProjectDialog::chooseTemplate(const QString& id) {
    templateId_ = id;
    for (QPushButton* button : templateButtons_) {
        button->setChecked(button->property("templateId").toString() == id);
    }
    updatePreview();
}

void NewProjectDialog::updatePreview() {
    preview_->setText(QDir::toNativeSeparators(projectRoot()));
    const QString text = name_->text().trimmed();
    error_->hide();

    std::string why;
    const bool good = engine::validProjectName(text.toStdString(), &why);
    preview_->setStyleSheet(good ? QStringLiteral("color:#8fa3b8;")
                                 : QStringLiteral("color:#e0767a;"));
}

void NewProjectDialog::browse() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose where to create the project"), location_->text());
    if (!folder.isEmpty()) location_->setText(QDir::cleanPath(folder));
}

void NewProjectDialog::showError(const QString& text) {
    error_->setText(text);
    error_->show();
}

void NewProjectDialog::accept() {
    const QString name = projectName();
    std::string why;
    if (!engine::validProjectName(name.toStdString(), &why)) {
        showError(QString::fromStdString(why));
        return;
    }
    if (location_->text().trimmed().isEmpty()) {
        showError(QStringLiteral("Choose a folder for the project."));
        return;
    }
    if (templateId_.isEmpty()) {
        showError(QStringLiteral("Pick a template to start from."));
        return;
    }

    const QFileInfo target(projectRoot());
    if (target.exists()) {
        if (!target.isDir()) {
            showError(QStringLiteral("A file with that name is already there."));
            return;
        }
        if (!QDir(target.absoluteFilePath()).isEmpty()) {
            showError(QStringLiteral("That folder already has files in it. Pick another name or folder."));
            return;
        }
    }
    QDialog::accept();
}

} // namespace editor::hub
