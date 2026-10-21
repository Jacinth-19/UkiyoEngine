// A Lua editor for the scripts a project owns, inside the editor window: syntax highlighting, line numbers, the
// current line, the line of a reported error, indentation that matches the project's .editorconfig, Save, Check
// (through the engine's own Lua host, so it compiles exactly what Play will run) and Open in External Editor.
#pragma once

#include <QDialog>
#include <QPair>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

#include <functional>

class QLabel;
class QPushButton;
class QWidget;

namespace editor {

// Opens a file in the editor named in Preferences > Scripting (`scripting/externalEditor`, default
// "code -g {file}:{line}"). `location` replaces {line} when it is not empty, so a reported error line can be passed.
// A command with no {file} gets the path appended, so "zed" and "code" work as written.
bool openInExternalEditor(const QString& path, const QString& location, QString* error);

// Lua 5.4 inside the engine's sandbox: keywords, the standard library's names, `ukiyo` and `self`, strings (including
// long brackets), numbers, comments (including long comments) and labels.
class LuaHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit LuaHighlighter(QTextDocument* document);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> rules_;
    QTextCharFormat keyword_;
    QTextCharFormat comment_;
    QTextCharFormat string_;
};

// The text control: line numbers, the current line, tab that inserts four spaces, and the indent rules a Lua
// programmer expects (a new line keeps the indentation, and one more after function/then/do/else).
class LuaCodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LuaCodeEditor(QWidget* parent = nullptr);
    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent* event);
    // A line to mark in red, or 0 to clear.
    void markErrorLine(int line);
    int errorLine() const { return errorLine_; }
    void goToLine(int line);

signals:
    void cursorMoved(int line, int column);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void updateLineNumberWidth();
    void updateHighlight();

    QWidget* numbers_ = nullptr;
    int errorLine_ = 0;
};

// The window around the text: the file's name, Save, Check, and a line to say what happened.
class LuaEditorDialog : public QDialog {
    Q_OBJECT
public:
    // Compiles the text on screen without running it, with the engine's own Lua host. The worker does the compiling;
    // the dialog only asks, and the text (not the saved file) is what is checked.
    using Checker = std::function<bool(const QString& path, const QString& text, QString* error)>;

    LuaEditorDialog(QWidget* parent, const QString& path, Checker checker);
    bool openFile(QString* error);
    bool saveFile(QString* error);
    const QString& path() const { return path_; }

    LuaCodeEditor* editor() const { return editor_; }

signals:
    void saved(const QString& path);

private:
    void check();
    void openExternal();

    QString path_;
    Checker checker_;
    LuaCodeEditor* editor_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* where_ = nullptr;
};

} // namespace editor
