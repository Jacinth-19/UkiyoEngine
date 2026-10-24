// Tools > Scripting Tools: the extensions and programs this project is set up for, whether each one is already on this
// machine, and the command that installs it. The list comes from ukiyo::script_project::toolRecommendations(), the same
// table `.vscode/extensions.json` is generated from, so what the project asks for and what this window offers are one
// list.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace editor {

class ScriptingToolsDialog : public QDialog {
    Q_OBJECT
public:
    // Writes the project's support files. The window owns the render worker, so it passes a callback instead of the
    // session; the dialog has no engine of its own.
    using Writer = std::function<bool(QString* error)>;

    ScriptingToolsDialog(QWidget* parent, QString projectRoot, Writer writer);

    // For the editor's self-test: the rows shown, and which tools were found on this machine.
    int toolCount() const;
    QStringList foundTools() const;

    // The command that opens a file in the editor named in `path`, with {file} and {line} filled in.
    static QString externalEditorCommand(const QString& tool);

signals:
    // The user picked an editor for Open in External Editor; the window logs it.
    void externalEditorChosen(const QString& command);

private:
    void refreshDetection();
    void runOnSelection(int action);
    void useExternalEditor();

    QString projectRoot_;
    Writer writer_;
    QTreeWidget* list_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* editorLine_ = nullptr;
    QComboBox* editors_ = nullptr;
    QPushButton* install_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* open_ = nullptr;
    QStringList found_;
};

} // namespace editor
