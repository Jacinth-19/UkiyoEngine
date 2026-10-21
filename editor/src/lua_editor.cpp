#include "lua_editor.h"

#include "prefs_dialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>

namespace editor {

namespace {

const char* kKeywords =
    "and|break|do|else|elseif|end|false|for|function|goto|if|in|local|nil|not|or|repeat|return|then|true|until|while";
// The names a script sees without requiring anything: the standard library, and the engine's own table.
const char* kBuiltins =
    "require|pcall|xpcall|error|assert|select|type|tostring|tonumber|ipairs|pairs|next|rawget|rawset|rawequal|"
    "setmetatable|getmetatable|unpack|print|string|table|math|utf8|coroutine|ukiyo|self";

QColor gutterColor() { return QColor(26, 27, 30); }
QColor currentLineColor() { return QColor(255, 255, 255, 10); }
QColor errorLineColor() { return QColor(120, 32, 36, 140); }

} // namespace

// --- highlighting ----------------------------------------------------------------------------------------------

LuaHighlighter::LuaHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
    keyword_.setForeground(QColor(255, 121, 198));
    keyword_.setFontWeight(QFont::DemiBold);
    comment_.setForeground(QColor(110, 118, 129));
    comment_.setFontItalic(true);
    string_.setForeground(QColor(200, 214, 130));

    QTextCharFormat builtin;
    builtin.setForeground(QColor(122, 210, 255));
    QVector<QString> patterns{kKeywords, kBuiltins};
    for (int i = 0; i < patterns.size(); ++i) {
        QTextCharFormat f = i == 0 ? keyword_ : builtin;
        rules_.push_back({QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(patterns[i])), f});
    }

    QTextCharFormat number;
    number.setForeground(QColor(247, 178, 103));
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(0[xX][0-9a-fA-F]+|\\d+(\\.\\d*)?([eE][+-]?\\d+)?)\\b")),
                      number});

    // A function name after `function`, so a script's own API reads at a glance.
    QTextCharFormat call;
    call.setForeground(QColor(170, 220, 255));
    rules_.push_back({QRegularExpression(QStringLiteral("(?<=\\bfunction\\s)[A-Za-z_][A-Za-z0-9_.:]*")), call});

    // Short strings. Escapes are part of the match so a quote inside one does not end it early.
    rules_.push_back({QRegularExpression(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"")), string_});
    rules_.push_back({QRegularExpression(QStringLiteral("'(?:\\\\.|[^'\\\\])*'")), string_});

    // A short comment, but not the `--[[` that starts a long one.
    rules_.push_back({QRegularExpression(QStringLiteral("--(?!\\[\\[)[^\\n]*")), comment_});
}

void LuaHighlighter::highlightBlock(const QString& text) {
    // Long brackets carry across lines, so the block state says whether a comment or a string is still open. They are
    // recognised first and their ranges reserved, because a short-string rule must not claim the quotes inside [[ ]].
    const int previous = previousBlockState();
    QVector<QPair<int, int>> reserved; // [start, end) already painted
    int start = 0;
    if (previous == 1 || previous == 2) {
        const int close = text.indexOf(QStringLiteral("]]"));
        const QTextCharFormat& open = previous == 1 ? comment_ : string_;
        if (close < 0) {
            setFormat(0, static_cast<int>(text.size()), open);
            setCurrentBlockState(previous);
            return;
        }
        setFormat(0, close + 2, open);
        reserved.push_back({0, close + 2});
        start = close + 2;
    }

    const QRegularExpression longBracket(QStringLiteral("(--)?\\[\\["));
    auto it = longBracket.globalMatch(text, start);
    while (it.hasNext()) {
        const auto match = it.next();
        const bool isComment = !match.captured(1).isEmpty();
        const int from = static_cast<int>(match.capturedStart());
        const int close = text.indexOf(QStringLiteral("]]"), from + match.capturedLength());
        if (close < 0) {
            setFormat(from, static_cast<int>(text.size()) - from, isComment ? comment_ : string_);
            setCurrentBlockState(isComment ? 1 : 2);
            return;
        }
        setFormat(from, close + 2 - from, isComment ? comment_ : string_);
        reserved.push_back({from, close + 2});
        it = longBracket.globalMatch(text, close + 2);
    }

    for (const Rule& rule : rules_) {
        auto ruleIt = rule.pattern.globalMatch(text);
        while (ruleIt.hasNext()) {
            const auto match = ruleIt.next();
            const int at = static_cast<int>(match.capturedStart());
            const int threshold = at + static_cast<int>(match.capturedLength());
            bool conflicts = false;
            for (const auto& span : reserved) conflicts = conflicts || (at < span.second && span.first < threshold);
            if (!conflicts) setFormat(at, threshold - at, rule.format);
        }
    }
    if (currentBlockState() == 1 || currentBlockState() == 2) setCurrentBlockState(0);
}

// --- the text control ------------------------------------------------------------------------------------------

namespace {
class LuaNumberArea : public QWidget {
public:
    LuaNumberArea(LuaCodeEditor* editor) : QWidget(editor), editor_(editor) {}
    QSize sizeHint() const override { return QSize(editor_->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* event) override { editor_->paintLineNumbers(event); }

private:
    LuaCodeEditor* editor_;
};
} // namespace

LuaCodeEditor::LuaCodeEditor(QWidget* parent) : QPlainTextEdit(parent) {
    numbers_ = new LuaNumberArea(this);
    numbers_->setObjectName(QStringLiteral("luaLineNumbers"));
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setTabStopDistance(4 * QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    new LuaHighlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateLineNumberWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy) numbers_->scroll(0, dy);
        else numbers_->update(0, rect.y(), numbers_->width(), rect.height());
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        updateHighlight();
        emit cursorMoved(textCursor().blockNumber() + 1, textCursor().positionInBlock() + 1);
    });
    updateLineNumberWidth();
    updateHighlight();
}

int LuaCodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10) ++digits;
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void LuaCodeEditor::updateLineNumberWidth() { setViewportMargins(lineNumberAreaWidth(), 0, 0, 0); }

void LuaCodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    numbers_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void LuaCodeEditor::paintLineNumbers(QPaintEvent* event) {
    QPainter painter(numbers_);
    painter.fillRect(event->rect(), gutterColor());
    painter.setFont(font());
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const bool isError = number + 1 == errorLine_;
            painter.setPen(isError ? QColor(255, 120, 120) : QColor(120, 126, 138));
            painter.drawText(0, top, numbers_->width() - 6, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void LuaCodeEditor::markErrorLine(int line) {
    errorLine_ = line;
    updateHighlight();
    numbers_->update();
}

void LuaCodeEditor::goToLine(int line) {
    QTextCursor cursor(document()->findBlockByNumber(qMax(0, line - 1)));
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

void LuaCodeEditor::updateHighlight() {
    QVector<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection current;
    current.format.setBackground(currentLineColor());
    current.format.setProperty(QTextFormat::FullWidthSelection, true);
    current.cursor = textCursor();
    current.cursor.clearSelection();
    selections.push_back(current);
    if (errorLine_ > 0 && errorLine_ <= blockCount()) {
        QTextEdit::ExtraSelection bad;
        bad.format.setBackground(errorLineColor());
        bad.format.setProperty(QTextFormat::FullWidthSelection, true);
        bad.cursor = QTextCursor(document()->findBlockByNumber(errorLine_ - 1));
        bad.cursor.clearSelection();
        selections.push_back(bad);
    }
    setExtraSelections(selections);
}

void LuaCodeEditor::keyPressEvent(QKeyEvent* event) {
    // Enter keeps the indentation and adds a level after a block opener; a closing `end` drops back one.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const QString line = textCursor().block().text();
        QString indent;
        for (const QChar c : line) {
            if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) indent += c;
            else break;
        }
        const QString trimmed = line.trimmed();
        const bool opens = trimmed.endsWith(QLatin1String("then")) || trimmed.endsWith(QLatin1String("do")) ||
                           trimmed.startsWith(QLatin1String("function")) || trimmed.endsWith(QLatin1String("else")) ||
                           trimmed.endsWith(QLatin1String("repeat"));
        if (opens) indent += QStringLiteral("    ");
        QPlainTextEdit::keyPressEvent(event);
        insertPlainText(indent);
        return;
    }
    if (event->key() == Qt::Key_Tab && !textCursor().hasSelection()) {
        insertPlainText(QStringLiteral("    "));
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

// --- the window ------------------------------------------------------------------------------------------------

LuaEditorDialog::LuaEditorDialog(QWidget* parent, const QString& path, Checker checker)
    : QDialog(parent), path_(path), checker_(std::move(checker)) {
    setWindowTitle(QStringLiteral("Script - ") + QFileInfo(path).fileName());
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(980, 720);

    editor_ = new LuaCodeEditor(this);
    editor_->setObjectName(QStringLiteral("luaEditor"));

    auto* bar = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("luaStatus"));
    where_ = new QLabel(this);
    where_->setObjectName(QStringLiteral("luaWhere"));
    where_->setStyleSheet(QStringLiteral("color: #8b93a1;"));
    auto* save = new QPushButton(QStringLiteral("Save"), this);
    save->setObjectName(QStringLiteral("luaSave"));
    save->setShortcut(QKeySequence::Save);
    auto* checkButton = new QPushButton(QStringLiteral("Check"), this);
    checkButton->setObjectName(QStringLiteral("luaCheck"));
    checkButton->setToolTip(QStringLiteral("Compiles the script with the engine's own Lua host, without running it"));
    auto* external = new QPushButton(QStringLiteral("Open in External Editor"), this);
    external->setObjectName(QStringLiteral("luaExternal"));
    external->setToolTip(QStringLiteral("Opens the file in the editor set in Preferences > Scripting"));
    bar->addWidget(save);
    bar->addWidget(checkButton);
    bar->addWidget(external);
    bar->addWidget(where_, 1);
    bar->addWidget(status_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(editor_, 1);
    layout->addLayout(bar);

    connect(editor_, &LuaCodeEditor::cursorMoved, this, [this](int line, int column) {
        where_->setText(QStringLiteral("Line %1, column %2").arg(line).arg(column));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        QString error;
        if (saveFile(&error)) status_->setText(QStringLiteral("Saved"));
        else status_->setText(error);
    });
    connect(checkButton, &QPushButton::clicked, this, [this] { this->check(); });
    connect(external, &QPushButton::clicked, this, [this] { openExternal(); });
}

bool LuaEditorDialog::openFile(QString* error) {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("could not open ") + path_;
        return false;
    }
    editor_->setPlainText(QString::fromUtf8(file.readAll()));
    editor_->markErrorLine(0);
    editor_->document()->setModified(false);
    if (status_) status_->setText(QStringLiteral("Loaded"));
    return true;
}

bool LuaEditorDialog::saveFile(QString* error) {
    QFile file(path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("could not write ") + path_;
        return false;
    }
    file.write(editor_->toPlainText().toUtf8());
    editor_->document()->setModified(false);
    emit saved(path_);
    return true;
}

void LuaEditorDialog::check() {
    if (!checker_) {
        status_->setText(QStringLiteral("No checker"));
        return;
    }
    // Check what is on screen, not what is on disk: saving first would hide a file the user has not committed to.
    QString error;
    const bool ok = checker_(path_, editor_->toPlainText(), &error);
    if (ok) {
        editor_->markErrorLine(0);
        status_->setStyleSheet(QString());
        status_->setText(QStringLiteral("No errors"));
        return;
    }
    // "path:12: message" -- mark the line and keep the message.
    const QRegularExpression re(QStringLiteral(":(\\d+):"));
    const auto match = re.match(error);
    if (match.hasMatch()) editor_->goToLine(match.captured(1).toInt());
    editor_->markErrorLine(match.hasMatch() ? match.captured(1).toInt() : 0);
    status_->setText(error);
    status_->setStyleSheet(QStringLiteral("color: #ff8b8b;"));
}

void LuaEditorDialog::openExternal() {
    QString error;
    if (openInExternalEditor(path_, QString(), &error)) {
        status_->setText(QStringLiteral("Opened in ") + error);
        return;
    }
    status_->setText(error);
}

bool openInExternalEditor(const QString& path, const QString& location, QString* error) {
    QString command = prefs::text(QStringLiteral("scripting/externalEditor"), QString());
    if (command.trimmed().isEmpty()) {
        command = QStringLiteral("code -g {file}:{line}");
        prefs::set(QStringLiteral("scripting/externalEditor"), command);
    }
    QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        if (error) *error = QStringLiteral("The external editor command is empty");
        return false;
    }
    const QString program = parts.takeFirst();
    // {file} is the path, {line} is the line to jump to (or 1). A command that names neither still gets the path.
    const bool namesFile = command.contains(QStringLiteral("{file}"));
    const QString line = location.isEmpty() ? QStringLiteral("1") : location;
    for (QString& part : parts) part.replace(QStringLiteral("{file}"), path).replace(QStringLiteral("{line}"), line);
    if (!namesFile) parts << path;
    if (!QProcess::startDetached(program, parts)) {
        if (error)
            *error = QStringLiteral("Could not start ") + program + QStringLiteral(". Set the command in Preferences > Scripting.");
        return false;
    }
    if (error) *error = program;
    return true;
}

} // namespace editor
