#include "prefs_dialog.h"

#include "engine/browser_zoom.h"

#include "icons.h"

#include <QCheckBox>
#include <functional>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QFrame>
#include "foldout.h"

namespace editor {

namespace {
// Same grey as the rest of the editor icons.
QIcon uiIcon(const char* name) { return materialIcon(name, iconColor()); }
}  // namespace

namespace prefs {

QSettings& store() {
    static QSettings settings(QStringLiteral("Ukiyo"), QStringLiteral("UkiyoEditor"));
    return settings;
}

bool boolean(const QString& key, bool fallback) { return store().value(key, fallback).toBool(); }
int integer(const QString& key, int fallback) { return store().value(key, fallback).toInt(); }
QString text(const QString& key, const QString& fallback) { return store().value(key, fallback).toString(); }
void set(const QString& key, const QVariant& value) { store().setValue(key, value); }

}  // namespace prefs

namespace {

// Records the preference key and its default on a widget, so Reset Preferences can restore it.
void tagPref(QWidget* w, const QString& key, const QVariant& fallback) {
    w->setProperty("prefKey", key);
    w->setProperty("prefDefault", fallback);
}

// A Unity-style section: a foldout header over a form, the same as Project Settings.
QFormLayout* section(QVBoxLayout* v, const QString& title) {
    QWidget* body = new QWidget;
    QFormLayout* form = new QFormLayout(body);
    form->setContentsMargins(14, 6, 10, 8);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(5);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    v->addWidget(new Foldout(title, body));
    return form;
}

// A checkbox row that writes its value to the store.
QCheckBox* prefCheck(QFormLayout* form, const QString& label, const QString& key, bool fallback,
                     const std::function<void()>& changed) {
    QCheckBox* box = new QCheckBox(label);
    box->setChecked(prefs::boolean(key, fallback));
    tagPref(box, key, fallback);
    QObject::connect(box, &QCheckBox::toggled, box, [key, changed](bool on) {
        prefs::set(key, on);
        changed();
    });
    form->addRow(box);
    return box;
}

// A page is a scroll area around the sections, matching the Project Settings pages.
QWidget* wrapPage(QWidget* content) {
    QScrollArea* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("prefsPage"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

}  // namespace

PreferencesDialog::PreferencesDialog(std::vector<ShortcutEntry> shortcuts, QWidget* parent)
    : QDialog(parent), shortcuts_(std::move(shortcuts)) {
    setWindowTitle(QStringLiteral("Preferences"));
    setWindowIcon(uiIcon("settings"));
    resize(760, 500);

    categories_ = new QListWidget;
    categories_->setObjectName("prefsCategories");
    categories_->setFixedWidth(176);
    categories_->setIconSize(QSize(18, 18));
    pages_ = new QStackedWidget;
    pages_->setObjectName("prefsPages");

    const struct {
        const char* icon;
        const char* name;
        QWidget* page;
    } entries[] = {
        {"tune", "General", buildGeneral()},
        {"folder", "Content Browser", buildBrowser()},
        {"terminal", "Scripting", buildScripting()},
        {"keyboard", "Shortcuts", buildShortcuts()},
    };
    for (const auto& e : entries) {
        categories_->addItem(new QListWidgetItem(uiIcon(e.icon), QString::fromLatin1(e.name)));
        pages_->addWidget(e.page);
    }
    connect(categories_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);

    // Search narrows the category list, the same as the Project Settings search.
    QLineEdit* search = new QLineEdit;
    search->setObjectName(QStringLiteral("prefsSearch"));
    search->setPlaceholderText(QStringLiteral("Search"));
    search->setClearButtonEnabled(true);
    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < categories_->count(); ++i)
            categories_->item(i)->setHidden(!categories_->item(i)->text().contains(text, Qt::CaseInsensitive));
        // Keep a visible page selected: move to the first match if the current one is hidden.
        if (categories_->currentItem() && categories_->currentItem()->isHidden()) {
            for (int i = 0; i < categories_->count(); ++i)
                if (!categories_->item(i)->isHidden()) { categories_->setCurrentRow(i); break; }
        }
    });
    categories_->setCurrentRow(0);

    QPushButton* reset = new QPushButton(uiIcon("restart_alt"), QStringLiteral("Reset Preferences"));
    reset->setObjectName(QStringLiteral("prefsReset"));
    reset->setToolTip(QStringLiteral("Restores General and Content Browser preferences. Shortcuts have their own reset."));
    connect(reset, &QPushButton::clicked, this, &PreferencesDialog::resetPreferences);
    QPushButton* close = new QPushButton(QStringLiteral("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    QHBoxLayout* footer = new QHBoxLayout;
    footer->addWidget(reset);
    footer->addStretch(1);
    footer->addWidget(close);

    QVBoxLayout* side = new QVBoxLayout;
    side->addWidget(search);
    side->addWidget(categories_, 1);
    QHBoxLayout* body = new QHBoxLayout;
    body->addLayout(side);
    body->addWidget(pages_, 1);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->addLayout(body, 1);
    root->addLayout(footer);
}

void PreferencesDialog::resetPreferences() {
    // Every tagged control goes back to its default, and its stored key is removed.
    const QList<QWidget*> all = findChildren<QWidget*>();
    for (QWidget* w : all) {
        const QVariant key = w->property("prefKey");
        if (!key.isValid()) continue;
        const QVariant fallback = w->property("prefDefault");
        prefs::store().remove(key.toString());
        QSignalBlocker block(w);
        if (auto* cb = qobject_cast<QCheckBox*>(w)) cb->setChecked(fallback.toBool());
        else if (auto* combo = qobject_cast<QComboBox*>(w)) combo->setCurrentIndex(fallback.toInt());
        else if (auto* slider = qobject_cast<QSlider*>(w)) slider->setValue(fallback.toInt());
        else if (auto* line = qobject_cast<QLineEdit*>(w)) line->setText(fallback.toString());
    }
    emit changed();
}

QWidget* PreferencesDialog::buildGeneral() {
    QWidget* content = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(content);
    v->setContentsMargins(12, 12, 12, 12);
    QFormLayout* form = section(v, QStringLiteral("General"));
    prefCheck(form, QStringLiteral("Ask to save unsaved scene changes when closing the editor"),
              QStringLiteral("general/confirmExit"), true, [this] { emit changed(); });
    prefCheck(form, QStringLiteral("Reopen the last scene at startup"),
              QStringLiteral("general/reopenLast"), true, [this] { emit changed(); });
    form->addRow(new QLabel(QStringLiteral("Scene changes are saved to .ukscene files in the Project's assets folder.")));
    v->addStretch(1);
    return wrapPage(content);
}

QWidget* PreferencesDialog::buildBrowser() {
    QWidget* content = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(content);
    v->setContentsMargins(12, 12, 12, 12);
    QFormLayout* form = section(v, QStringLiteral("Content Browser"));

    QComboBox* view = new QComboBox;
    view->addItems({QStringLiteral("Icons"), QStringLiteral("List"), QStringLiteral("Details")});
    view->setCurrentIndex(prefs::integer(QStringLiteral("browser/view"), 0));
    tagPref(view, QStringLiteral("browser/view"), 0);
    connect(view, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        prefs::set(QStringLiteral("browser/view"), i);
        emit changed();
    });
    form->addRow(QStringLiteral("Default view mode"), view);

    // The same ladder the browser's own slider and Ctrl + wheel use (engine/browser_zoom.h): 32 to 224 in steps of 16.
    QSlider* thumb = new QSlider(Qt::Horizontal);
    thumb->setRange(ukiyo::kThumbMin, ukiyo::kThumbMax);
    thumb->setSingleStep(ukiyo::kThumbStep);
    thumb->setPageStep(ukiyo::kThumbStep * 2);
    thumb->setValue(ukiyo::thumbSliderValue(prefs::integer(QStringLiteral("browser/thumb"), ukiyo::kThumbDefault)));
    tagPref(thumb, QStringLiteral("browser/thumb"), ukiyo::kThumbDefault);
    connect(thumb, &QSlider::valueChanged, this, [this](int val) {
        prefs::set(QStringLiteral("browser/thumb"), val);
        emit changed();
    });
    form->addRow(QStringLiteral("Default thumbnail size"), thumb);

    prefCheck(form, QStringLiteral("Show hidden files"), QStringLiteral("browser/hidden"), false,
              [this] { emit changed(); });
    v->addStretch(1);
    return wrapPage(content);
}

QWidget* PreferencesDialog::buildScripting() {
    QWidget* content = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(content);
    v->setContentsMargins(12, 12, 12, 12);

    QFormLayout* editor = section(v, QStringLiteral("Lua Editor"));
    prefCheck(editor, QStringLiteral("Open .lua files in the built-in editor"), QStringLiteral("scripting/builtinEditor"),
              true, [this] { emit changed(); });

    QLineEdit* command = new QLineEdit(prefs::text(QStringLiteral("scripting/externalEditor"),
                                                   QStringLiteral("code -g {file}:{line}")));
    command->setPlaceholderText(QStringLiteral("code -g {file}:{line}"));
    tagPref(command, QStringLiteral("scripting/externalEditor"), QStringLiteral("code -g {file}:{line}"));
    connect(command, &QLineEdit::editingFinished, this, [this, command] {
        prefs::set(QStringLiteral("scripting/externalEditor"), command->text().trimmed());
        emit changed();
    });
    editor->addRow(QStringLiteral("External editor command"), command);
    editor->addRow(new QLabel(QStringLiteral(
        "Used by Open in External Editor. {file} is the path and {line} the line to jump to; a command without "
        "{file} gets the path appended, so \"zed\" works.")));

    QFormLayout* project = section(v, QStringLiteral("Project Files"));
    project->addRow(new QLabel(QStringLiteral(
        "Tools > Write Script Support Files writes the API stub (Scripts/.ukiyo/lua/ukiyo.lua), .luarc.json for "
        "LuaLS, .editorconfig, stylua.toml, selene.toml, and .vscode/extensions.json with the recommended "
        "extensions. They are also written when a project is created.")));
    project->addRow(new QLabel(QStringLiteral(
        "Recommended: LuaLS (sumneko.lua) for completion from the stub, StyLua for formatting, Selene for linting, "
        "EditorConfig, and actboy168.lua-debug for breakpoints.")));
    v->addStretch(1);
    return wrapPage(content);
}

QWidget* PreferencesDialog::buildShortcuts() {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(12, 12, 12, 12);
    v->addWidget(new QLabel(QStringLiteral("<b>Shortcuts</b>  Click a key field and press the new combination.")));
    shortcutTable_ = new QTableWidget(static_cast<int>(shortcuts_.size()), 2);
    shortcutTable_->setObjectName("shortcutTable");
    shortcutTable_->setHorizontalHeaderLabels({QStringLiteral("Command"), QStringLiteral("Shortcut")});
    shortcutTable_->verticalHeader()->setVisible(false);
    shortcutTable_->horizontalHeader()->setStretchLastSection(false);
    shortcutTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcutTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    shortcutTable_->setColumnWidth(1, 220);
    shortcutTable_->setSelectionMode(QAbstractItemView::NoSelection);

    for (int row = 0; row < static_cast<int>(shortcuts_.size()); ++row) {
        const ShortcutEntry& entry = shortcuts_[row];
        shortcutTable_->setItem(row, 0, new QTableWidgetItem(entry.label));
        QKeySequenceEdit* edit = new QKeySequenceEdit(entry.action->shortcut());
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, row](const QKeySequence& seq) {
            const ShortcutEntry& e = shortcuts_[row];
            e.action->setShortcut(seq);
            prefs::set(QStringLiteral("shortcuts/") + e.id,
                       seq.isEmpty() ? QStringLiteral("none") : seq.toString(QKeySequence::PortableText));
            emit changed();
        });
        shortcutTable_->setCellWidget(row, 1, edit);
    }
    v->addWidget(shortcutTable_, 1);

    QPushButton* reset = new QPushButton(uiIcon("restart_alt"), QStringLiteral("Reset all shortcuts"));
    connect(reset, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < static_cast<int>(shortcuts_.size()); ++row) {
            const ShortcutEntry& e = shortcuts_[row];
            e.action->setShortcut(e.defaultKey);
            prefs::store().remove(QStringLiteral("shortcuts/") + e.id);
            QKeySequenceEdit* edit = qobject_cast<QKeySequenceEdit*>(shortcutTable_->cellWidget(row, 1));
            if (edit) {
                QSignalBlocker block(edit);
                edit->setKeySequence(e.defaultKey);
            }
        }
        emit changed();
    });
    QHBoxLayout* h = new QHBoxLayout;
    h->addStretch(1);
    h->addWidget(reset);
    v->addLayout(h);
    return page;
}

}  // namespace editor
