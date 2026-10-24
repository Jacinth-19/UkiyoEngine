// Editor preferences (Edit > Preferences) and the QSettings store behind them.
#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <vector>

class QAction;
class QListWidget;
class QStackedWidget;
class QTableWidget;

namespace editor {

// A menu command that can be rebound in Preferences > Shortcuts.
struct ShortcutEntry {
    QString id;               // stable key used in the settings store
    QString label;            // shown in the table
    QAction* action = nullptr;
    QKeySequence defaultKey;
};

namespace prefs {
QSettings& store();
bool boolean(const QString& key, bool fallback);
int integer(const QString& key, int fallback);
QString text(const QString& key, const QString& fallback);
void set(const QString& key, const QVariant& value);
}  // namespace prefs

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(std::vector<ShortcutEntry> shortcuts, QWidget* parent = nullptr);

signals:
    // Emitted after any preference changes, so the editor can re-read them.
    void changed();

private:
    QWidget* buildGeneral();
    QWidget* buildBrowser();
    // The external editor command, and whether scripts open here or there.
    QWidget* buildScripting();
    QWidget* buildShortcuts();
    // Restores every General and Content Browser preference to its default. Shortcuts have their own reset.
    void resetPreferences();

    std::vector<ShortcutEntry> shortcuts_;
    QTableWidget* shortcutTable_ = nullptr;
    QListWidget* categories_ = nullptr;
    QStackedWidget* pages_ = nullptr;
};

}  // namespace editor
