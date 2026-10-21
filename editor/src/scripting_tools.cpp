#include "scripting_tools.h"

#include "engine/script_project.h"
#include "icons.h"
#include "prefs_dialog.h"

#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QAbstractItemView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace editor {

namespace {

QIcon toolIcon(const QString& kind) {
    if (kind == QStringLiteral("Language server")) return materialIcon("language", iconColor());
    if (kind == QStringLiteral("Formatter")) return materialIcon("edit_note", iconColor());
    if (kind == QStringLiteral("Linter")) return materialIcon("warning", iconColor());
    if (kind == QStringLiteral("Debugger")) return materialIcon("bug_report", iconColor());
    if (kind == QStringLiteral("File format")) return materialIcon("description", iconColor());
    return materialIcon("deployed_code", iconColor());
}

// The editors this machine can be told to hand a script to, in the order they are offered.
struct KnownEditor {
    const char* program;     // what to look for on PATH
    const char* label;
    const char* command;     // the external-editor command it gets
};

const KnownEditor kEditors[] = {
    {"code", "VS Code", "code -g {file}:{line}"},
    {"code-insiders", "VS Code Insiders", "code-insiders -g {file}:{line}"},
    {"codium", "VSCodium", "codium -g {file}:{line}"},
    {"zed", "Zed", "zed {file}:{line}"},
    {"nvim", "Neovim", "nvim +{line} {file}"},
    {"subl", "Sublime Text", "subl {file}:{line}"},
};

} // namespace

QString ScriptingToolsDialog::externalEditorCommand(const QString& tool) {
    for (const KnownEditor& e : kEditors)
        if (tool == QLatin1String(e.label)) return QString::fromLatin1(e.command);
    return {};
}

ScriptingToolsDialog::ScriptingToolsDialog(QWidget* parent, QString projectRoot, Writer writer)
    : QDialog(parent), projectRoot_(std::move(projectRoot)), writer_(std::move(writer)) {
    setWindowTitle(QStringLiteral("Scripting Tools"));
    setWindowIcon(materialIcon("deployed_code", iconColor()));
    resize(920, 560);

    auto* title = new QLabel(QStringLiteral("<b>Lua tools this project is set up for</b>"), this);
    auto* intro = new QLabel(
        QStringLiteral("The project carries the configuration each of these reads: <code>Scripts/.ukiyo/lua/ukiyo.lua</code> "
                       "(the API stub), <code>.luarc.json</code>, <code>.editorconfig</code>, <code>stylua.toml</code>, "
                       "<code>selene.toml</code> and <code>.vscode/</code>. Install what is missing; nothing else is "
                       "needed for completion, formatting, linting and diagnostics."),
        this);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color: #a9b1bd;"));
    title->setObjectName(QStringLiteral("toolsTitle"));

    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("toolsList"));
    list_->setColumnCount(4);
    list_->setHeaderLabels({QStringLiteral("Tool"), QStringLiteral("Kind"), QStringLiteral("Found here"),
                            QStringLiteral("What it gives this project")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(3, QHeaderView::Stretch);

    for (const ukiyo::script_project::ToolRecommendation& tool : ukiyo::script_project::toolRecommendations()) {
        auto* row = new QTreeWidgetItem(list_);
        row->setText(0, QString::fromUtf8(tool.name));
        row->setIcon(0, toolIcon(QString::fromUtf8(tool.kind)));
        row->setText(1, QString::fromUtf8(tool.kind));
        row->setText(3, QString::fromUtf8(tool.why));
        row->setToolTip(3, QString::fromUtf8(tool.why));
        row->setData(0, Qt::UserRole, QString::fromUtf8(tool.vscodeId));
        row->setData(0, Qt::UserRole + 1, QString::fromUtf8(tool.executable));
        row->setData(0, Qt::UserRole + 2, QString::fromUtf8(tool.url));
    }

    install_ = new QPushButton(QStringLiteral("Install in VS Code"), this);
    install_->setObjectName(QStringLiteral("toolsInstall"));
    install_->setToolTip(QStringLiteral("Runs code --install-extension for the selected tool"));
    copy_ = new QPushButton(QStringLiteral("Copy Command"), this);
    copy_->setObjectName(QStringLiteral("toolsCopy"));
    open_ = new QPushButton(QStringLiteral("Open Page"), this);
    open_->setObjectName(QStringLiteral("toolsOpen"));
    auto* write = new QPushButton(QStringLiteral("Write Support Files"), this);
    write->setObjectName(QStringLiteral("toolsWrite"));
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("toolsClose"));

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("toolsStatus"));
    status_->setStyleSheet(QStringLiteral("color: #8b93a1;"));
    status_->setWordWrap(true);

    // The editors found on this machine, so "use an external IDE" is one click and not a field to fill in.
    editorLine_ = new QLabel(this);
    editorLine_->setObjectName(QStringLiteral("toolsEditorLine"));
    editorLine_->setStyleSheet(QStringLiteral("color: #a9b1bd;"));
    editors_ = new QComboBox(this);
    editors_->setObjectName(QStringLiteral("toolsEditors"));
    auto* use = new QPushButton(QStringLiteral("Use as External Editor"), this);
    use->setObjectName(QStringLiteral("toolsUseEditor"));

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(install_);
    buttons->addWidget(copy_);
    buttons->addWidget(open_);
    buttons->addSpacing(12);
    buttons->addWidget(write);
    buttons->addStretch(1);
    buttons->addWidget(close);

    auto* editorRow = new QHBoxLayout;
    editorRow->addWidget(editorLine_);
    editorRow->addWidget(editors_, 1);
    editorRow->addWidget(use);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(title);
    layout->addWidget(intro);
    layout->addWidget(list_, 1);
    layout->addWidget(status_);
    layout->addLayout(editorRow);
    layout->addLayout(buttons);

    connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* row, QTreeWidgetItem*) {
        if (!row) return;
        const QString id = row->data(0, Qt::UserRole).toString();
        install_->setEnabled(!id.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("code")).isEmpty());
        copy_->setEnabled(!id.isEmpty());
    });
    connect(install_, &QPushButton::clicked, this, [this] { runOnSelection(0); });
    connect(copy_, &QPushButton::clicked, this, [this] { runOnSelection(1); });
    connect(open_, &QPushButton::clicked, this, [this] { runOnSelection(2); });
    connect(write, &QPushButton::clicked, this, [this] {
        if (!writer_) {
            status_->setText(QStringLiteral("This window has no project to write to."));
            return;
        }
        QString error;
        if (writer_(&error)) status_->setText(QStringLiteral("The support files are up to date."));
        else status_->setText(QStringLiteral("Could not write the support files: ") + error);
    });
    connect(close, &QPushButton::clicked, this, [this] { accept(); });
    connect(use, &QPushButton::clicked, this, [this] { useExternalEditor(); });

    refreshDetection();
    list_->setCurrentItem(list_->topLevelItem(0));
}

void ScriptingToolsDialog::refreshDetection() {
    found_.clear();
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = list_->topLevelItem(i);
        const QString exe = row->data(0, Qt::UserRole + 1).toString();
        QString where;
        if (!exe.isEmpty()) where = QStandardPaths::findExecutable(exe);
        if (where.isEmpty() && !row->data(0, Qt::UserRole).toString().isEmpty()) {
            // A tool that only lives inside an extension has no program of its own: the extension is what is installed.
            const QString code = QStandardPaths::findExecutable(QStringLiteral("code"));
            if (!code.isEmpty()) where = QStringLiteral("with VS Code");
        }
        row->setText(2, where.isEmpty() ? QStringLiteral("-") : where);
        if (!where.isEmpty()) found_ << row->text(0);
    }

    editors_->clear();
    for (const KnownEditor& e : kEditors) {
        if (QStandardPaths::findExecutable(QString::fromLatin1(e.program)).isEmpty()) continue;
        editors_->addItem(QString::fromLatin1(e.label));
    }
    const QString current = prefs::text(QStringLiteral("scripting/externalEditor"),
                                        QStringLiteral("code -g {file}:{line}"));
    if (editors_->count() == 0) {
        editors_->addItem(QStringLiteral("No editor found on PATH"));
        editors_->setEnabled(false);
        editorLine_->setText(QStringLiteral("No external editor was found. Set the command in Preferences > Scripting."));
    } else {
        editors_->setEnabled(true);
        editorLine_->setText(QStringLiteral("Open in:"));
        const QString remembered = prefs::text(QStringLiteral("scripting/externalEditorLabel"), QString());
        if (!remembered.isEmpty() && editors_->findText(remembered) >= 0) editors_->setCurrentText(remembered);
    }
    status_->setText(QStringLiteral("External editor command: %1").arg(current));
}

void ScriptingToolsDialog::runOnSelection(int action) {
    QTreeWidgetItem* row = list_->currentItem();
    if (!row) return;
    const QString id = row->data(0, Qt::UserRole).toString();
    const QString url = row->data(0, Qt::UserRole + 2).toString();
    if (action == 1) {
        if (id.isEmpty()) {
            status_->setText(QStringLiteral("%1 is installed by its own installer, not by VS Code.").arg(row->text(0)));
            return;
        }
        const QString command = QStringLiteral("code --install-extension ") + id;
        QGuiApplication::clipboard()->setText(command);
        status_->setText(QStringLiteral("Copied: ") + command);
        return;
    }
    if (action == 2) {
        if (url.isEmpty()) return;
        QDesktopServices::openUrl(QUrl(url));
        status_->setText(QStringLiteral("Opened ") + url);
        return;
    }
    // Install: only VS Code extensions with a code on this machine.
    const QString code = QStandardPaths::findExecutable(QStringLiteral("code"));
    if (id.isEmpty() || code.isEmpty()) {
        status_->setText(QStringLiteral("VS Code was not found. Copy the command and run it where it is installed."));
        return;
    }
    if (!QProcess::startDetached(code, {QStringLiteral("--install-extension"), id, QStringLiteral("--force")})) {
        status_->setText(QStringLiteral("Could not start VS Code."));
        return;
    }
    status_->setText(QStringLiteral("Installing %1 in VS Code...").arg(id));
}

void ScriptingToolsDialog::useExternalEditor() {
    if (editors_->count() == 0) return;
    const QString label = editors_->currentText();
    const QString command = externalEditorCommand(label);
    if (command.isEmpty()) {
        status_->setText(QStringLiteral("Pick an editor from the list, or set the command in Preferences > Scripting."));
        return;
    }
    prefs::set(QStringLiteral("scripting/externalEditor"), command);
    prefs::set(QStringLiteral("scripting/externalEditorLabel"), label);
    status_->setText(QStringLiteral("Scripts now open in %1: %2").arg(label, command));
    emit externalEditorChosen(command);
}

int ScriptingToolsDialog::toolCount() const { return list_ ? list_->topLevelItemCount() : 0; }

QStringList ScriptingToolsDialog::foundTools() const { return found_; }

} // namespace editor
