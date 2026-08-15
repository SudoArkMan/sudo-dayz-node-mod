#include "codeeditor.h"

#include "catalog.h"
#include "document.h"
#include "enforce/highlighter.h"
#include "enforce/lexer.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

namespace {

// Vanilla indents with tabs and codegen emits tabs, so the editor does too.
// The width is only how wide a tab draws.
constexpr int kTabWidth = 4;
constexpr int kCompletionLimit = 60;
// Bracket matching tokenises the whole buffer. Fine for a Raw node or a
// generated script; past this it would be felt on every caret move.
constexpr int kMatchLimit = 300000;

bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

bool isOpenBracket(QChar c)
{
    return c == QLatin1Char('(') || c == QLatin1Char('{') || c == QLatin1Char('[');
}

bool isCloseBracket(QChar c)
{
    return c == QLatin1Char(')') || c == QLatin1Char('}') || c == QLatin1Char(']');
}

QChar partnerOf(QChar c)
{
    switch (c.unicode()) {
    case '(': return QLatin1Char(')');
    case ')': return QLatin1Char('(');
    case '{': return QLatin1Char('}');
    case '}': return QLatin1Char('{');
    case '[': return QLatin1Char(']');
    case ']': return QLatin1Char('[');
    default: return QChar();
    }
}

// buildSearchIndex titles an event "Event OnFoo"; only the second half is a
// legal thing to type.
QString completionName(const SearchHit &hit)
{
    static const QString kEvent = QStringLiteral("Event ");
    return hit.title.startsWith(kEvent) ? hit.title.mid(kEvent.size()) : hit.title;
}

// "ref array<ref ItemBase>" -> "array", "autoptr PlayerBase" -> "PlayerBase".
QString baseTypeName(const QString &type)
{
    QString t = type.trimmed();
    const int lt = t.indexOf(QLatin1Char('<'));
    if (lt >= 0) t = t.left(lt);
    static const QStringList noise = {
        QStringLiteral("ref"), QStringLiteral("autoptr"), QStringLiteral("const"),
        QStringLiteral("static"), QStringLiteral("notnull"), QStringLiteral("owned"),
        QStringLiteral("out"), QStringLiteral("inout"),
    };
    QStringList parts = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    while (!parts.isEmpty() && noise.contains(parts.first())) parts.removeFirst();
    return parts.isEmpty() ? QString() : parts.first();
}

QString returnTypeOfCall(const Catalog *cat, const QString &name)
{
    if (!cat || name.isEmpty()) return {};
    SearchOptions opts;
    opts.limit = 8;
    for (const SearchHit &hit : cat->search(name, opts)) {
        if (completionName(hit) != name) continue;
        MethodSig sig;
        if (hit.key.startsWith(QLatin1Char('m'))) sig = cat->method(hit.key);
        else if (hit.key.startsWith(QLatin1Char('g'))) sig = cat->globalFn(hit.key);
        if (sig.valid && !sig.ret.isEmpty()) return baseTypeName(sig.ret);
    }
    return {};
}

// The class an expression before a '.' stands for, or "" when it cannot be
// worked out. Only the shapes that actually turn up in hand-written Enforce
// are handled: a bare name, `this`, and a call with no arguments. Anything
// deeper is left to the general search rather than guessed at.
QString receiverClass(const QString &beforeDot, const Document *doc)
{
    if (!doc) return {};

    LexState state = LexState::Normal;
    QVector<Token> sig;
    for (const Token &t : EnforceLexer::tokenize(beforeDot, state))
        if (t.kind != TokenKind::Whitespace && t.kind != TokenKind::Comment)
            sig.append(t);
    if (sig.isEmpty()) return {};

    QString name;
    bool isCall = false;
    const Token &last = sig.last();
    if (last.kind == TokenKind::Identifier || last.kind == TokenKind::Keyword) {
        name = last.text;
    } else if (last.text == QLatin1String(")") && sig.size() >= 3
               && sig.at(sig.size() - 2).text == QLatin1String("(")
               && sig.at(sig.size() - 3).kind == TokenKind::Identifier) {
        name = sig.at(sig.size() - 3).text;
        isCall = true;
    }
    if (name.isEmpty()) return {};

    const Catalog *cat = &doc->catalog();
    const Graph *graph = doc->activeGraph();

    if (isCall) {
        const QString ret = returnTypeOfCall(cat, name);
        return cat->classId(ret) >= 0 ? ret : QString();
    }

    // `this` is the class being authored, which the catalogue has never heard
    // of; its base is the nearest thing that answers "what can I call here".
    if (name == QLatin1String("this") && graph) {
        if (cat->classId(graph->className) >= 0) return graph->className;
        if (cat->classId(graph->baseClass) >= 0) return graph->baseClass;
        return {};
    }

    if (graph) {
        for (const GraphVariable &v : graph->variables) {
            if (v.name != name) continue;
            const QString t = baseTypeName(v.type);
            return cat->classId(t) >= 0 ? t : QString();
        }
        for (const GraphFunction &f : graph->functions) {
            for (const GraphParam &p : f.params) {
                if (p.name != name) continue;
                const QString t = baseTypeName(p.type);
                return cat->classId(t) >= 0 ? t : QString();
            }
        }
    }

    return cat->classId(name) >= 0 ? name : QString();
}

// Every name the surrounding graph declares, plus whatever the caller added.
QStringList symbolsFor(const Document *doc, const QStringList &extra)
{
    QStringList out = extra;
    const Graph *graph = doc ? doc->activeGraph() : nullptr;
    if (!graph) return out;
    for (const GraphVariable &v : graph->variables) out << v.name;
    for (const GraphFunction &f : graph->functions) {
        out << f.name;
        for (const GraphParam &p : f.params) out << p.name;
    }
    out.removeAll(QString());
    out.removeDuplicates();
    return out;
}

QString countPhrase(int n, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? singular : plural);
}

// Lines carrying a bracket that never pairs up, or an unclosed string or
// comment. 1-based, to match the gutter.
//
// Walked line by line rather than over tokenizeAll: a block comment's opening
// line looks unterminated on its own, and only the lexer state at the end of
// the buffer says whether it ever closed.
QSet<int> problemLines(const QString &text)
{
    QSet<int> lines;
    if (text.isEmpty()) return lines;

    QVector<int> braces, parens, squares; // lines of the openers still waiting
    LexState state = LexState::Normal;
    int commentOpenedAt = 0;

    const QStringList source = text.split(QLatin1Char('\n'));
    for (int i = 0; i < source.size(); ++i) {
        const int line = i + 1;
        const LexState before = state;
        const QVector<Token> tokens = EnforceLexer::tokenize(source.at(i), state);
        if (before == LexState::Normal && state == LexState::InBlockComment)
            commentOpenedAt = line;

        for (const Token &t : tokens) {
            if (t.kind == TokenKind::String
                && (t.length < 2 || !t.text.endsWith(QLatin1Char('"')))) {
                lines.insert(line);
                continue;
            }
            if (t.kind != TokenKind::Punctuation || t.length != 1) continue;

            const QChar c = t.text.at(0);
            QVector<int> *stack =
                c == QLatin1Char('{') || c == QLatin1Char('}')   ? &braces
                : c == QLatin1Char('(') || c == QLatin1Char(')') ? &parens
                : c == QLatin1Char('[') || c == QLatin1Char(']') ? &squares
                                                                 : nullptr;
            if (!stack) continue;
            if (isOpenBracket(c)) stack->append(line);
            else if (stack->isEmpty()) lines.insert(line);
            else stack->removeLast();
        }
    }

    if (state == LexState::InBlockComment && commentOpenedAt > 0)
        lines.insert(commentOpenedAt);
    for (const QVector<int> *stack : {&braces, &parens, &squares})
        for (int line : *stack) lines.insert(line);
    return lines;
}

void showCompletions(QCompleter *completer, QPlainTextEdit *editor)
{
    QAbstractItemView *popup = completer->popup();
    if (completer->completionCount() == 0) {
        popup->hide();
        return;
    }
    popup->setCurrentIndex(completer->completionModel()->index(0, 0));

    QRect box = editor->cursorRect();
    box.setWidth(popup->sizeHintForColumn(0)
                 + popup->verticalScrollBar()->sizeHint().width() + 12);
    completer->complete(box);
}

void indentSelection(QPlainTextEdit *editor, bool dedent)
{
    QTextDocument *doc = editor->document();
    QTextCursor cursor = editor->textCursor();
    const int firstBlock = doc->findBlock(cursor.selectionStart()).blockNumber();
    const int lastBlock = doc->findBlock(cursor.selectionEnd()).blockNumber();

    if (firstBlock == lastBlock && !dedent) {
        cursor.insertText(QStringLiteral("\t"));
        return;
    }

    cursor.beginEditBlock();
    for (int number = firstBlock; number <= lastBlock; ++number) {
        const QTextBlock block = doc->findBlockByNumber(number);
        if (!block.isValid()) continue;
        QTextCursor edit(block);
        edit.setPosition(block.position());
        if (!dedent) {
            edit.insertText(QStringLiteral("\t"));
            continue;
        }
        const QString text = block.text();
        int remove = 0;
        if (text.startsWith(QLatin1Char('\t'))) {
            remove = 1;
        } else {
            while (remove < kTabWidth && remove < text.size()
                   && text.at(remove) == QLatin1Char(' ')) ++remove;
        }
        if (remove == 0) continue;
        edit.setPosition(block.position() + remove, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    cursor.endEditBlock();

    // Keep the range selected so Tab can be pressed again.
    if (firstBlock == lastBlock) return;
    const QTextBlock first = doc->findBlockByNumber(firstBlock);
    const QTextBlock last = doc->findBlockByNumber(lastBlock);
    if (!first.isValid() || !last.isValid()) return;
    QTextCursor keep(first);
    keep.setPosition(first.position());
    keep.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    editor->setTextCursor(keep);
}

void insertNewlineWithIndent(QPlainTextEdit *editor)
{
    QTextCursor cursor = editor->textCursor();
    const QString text = cursor.block().text();
    const int column = qMin(cursor.positionInBlock(), text.size());
    const QString before = text.left(column);

    QString indent;
    for (const QChar c : before) {
        if (c != QLatin1Char('\t') && c != QLatin1Char(' ')) break;
        indent += c;
    }

    const bool opens = before.trimmed().endsWith(QLatin1Char('{'));
    const bool closes = text.mid(column).trimmed().startsWith(QLatin1Char('}'));

    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("\n") + indent
                      + (opens ? QStringLiteral("\t") : QString()));
    if (opens && closes) {
        // Splitting `{}` open: the closing brace gets its own line back at the
        // outer level and the caret stays in the body.
        const int caret = cursor.position();
        cursor.insertText(QStringLiteral("\n") + indent);
        cursor.setPosition(caret);
    }
    cursor.endEditBlock();
    editor->setTextCursor(cursor);
}

// True when the brace was handled here. Only fires on an otherwise blank line,
// where the user is closing a block and the auto-indent has left them a level
// too deep.
bool dedentClosingBrace(QPlainTextEdit *editor)
{
    QTextCursor cursor = editor->textCursor();
    if (cursor.hasSelection()) return false;

    const QString text = cursor.block().text();
    const int column = qMin(cursor.positionInBlock(), text.size());
    const QString before = text.left(column);
    if (before.isEmpty() || !before.trimmed().isEmpty()) return false;

    int remove = 0;
    if (before.endsWith(QLatin1Char('\t'))) {
        remove = 1;
    } else {
        while (remove < kTabWidth && remove < before.size()
               && before.at(before.size() - 1 - remove) == QLatin1Char(' ')) ++remove;
    }

    cursor.beginEditBlock();
    if (remove > 0) {
        cursor.setPosition(cursor.position() - remove, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    cursor.insertText(QStringLiteral("}"));
    cursor.endEditBlock();
    editor->setTextCursor(cursor);
    return true;
}

// Painted by the editor so the numbers share its font metrics and scrolling.
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override
    {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        m_editor->lineNumberAreaPaintEvent(event);
    }

private:
    CodeEditor *m_editor;
};

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent),
      m_lineNumberArea(new LineNumberArea(this)),
      m_highlighter(new EnforceHighlighter(document())),
      m_completer(new QCompleter(this)),
      m_completionModel(new QStringListModel(m_completer))
{
    setFont(theme::monoFont(9));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(kTabWidth
                       * fontMetrics().horizontalAdvance(QLatin1Char(' ')));

    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    // The catalogue ranks by substring, so prefix-only filtering here would
    // throw away most of what it just found.
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setModel(m_completionModel);
    m_completer->popup()->setFont(theme::monoFont(9));
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &CodeEditor::insertCompletion);

    connect(this, &CodeEditor::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &CodeEditor::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &CodeEditor::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);

    // validate() walks the whole buffer, so it runs behind a debounce rather
    // than on the keystroke.
    QTimer *validateTimer = new QTimer(this);
    validateTimer->setSingleShot(true);
    validateTimer->setInterval(150);
    connect(validateTimer, &QTimer::timeout, this, [this]() { validate(); });
    connect(this, &CodeEditor::textChanged,
            validateTimer, QOverload<>::of(&QTimer::start));

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

void CodeEditor::setDocumentContext(Document *doc)
{
    if (m_doc == doc) return;
    if (m_doc) disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;

    // EnforceHighlighter takes the catalogue at construction and offers no
    // setter, so wiring one up means replacing it.
    delete m_highlighter;
    m_highlighter = new EnforceHighlighter(document(), doc ? &doc->catalog() : nullptr);

    if (m_doc) {
        const auto refresh = [this]() {
            m_highlighter->setLocalSymbols(symbolsFor(m_doc, m_locals));
        };
        connect(m_doc, &Document::graphChanged, this, refresh);
        connect(m_doc, &Document::activeScriptChanged, this, refresh);
    }
    m_highlighter->setLocalSymbols(symbolsFor(m_doc, m_locals));
    validate();
}

void CodeEditor::setLocalSymbols(const QStringList &names)
{
    m_locals = names;
    m_highlighter->setLocalSymbols(symbolsFor(m_doc, m_locals));
}

CodeEditor::Status CodeEditor::validate()
{
    const EnforceScan scan = scanEnforce(toPlainText());

    Status status;
    status.braceBalance = scan.braceBalance;
    status.parenBalance = scan.parenBalance;

    if (scan.braceBalance > 0)
        status.problems << countPhrase(scan.braceBalance,
                                       QStringLiteral("unclosed brace"),
                                       QStringLiteral("unclosed braces"));
    else if (scan.braceBalance < 0)
        status.problems << countPhrase(-scan.braceBalance,
                                       QStringLiteral("extra closing brace"),
                                       QStringLiteral("extra closing braces"));

    if (scan.parenBalance > 0)
        status.problems << countPhrase(scan.parenBalance,
                                       QStringLiteral("unclosed parenthesis"),
                                       QStringLiteral("unclosed parentheses"));
    else if (scan.parenBalance < 0)
        status.problems << countPhrase(-scan.parenBalance,
                                       QStringLiteral("extra closing parenthesis"),
                                       QStringLiteral("extra closing parentheses"));

    if (scan.unterminatedString)
        status.problems << QStringLiteral("A string is left open");
    if (scan.unterminatedComment)
        status.problems << QStringLiteral("A block comment is left open");

    m_highlighter->setProblemLines(problemLines(toPlainText()));
    emit statusChanged(status);
    return status;
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10) ++digits;
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), theme::headerBg());

    const int edge = m_lineNumberArea->width() - 1;
    painter.setPen(theme::border());
    painter.drawLine(edge, event->rect().top(), edge, event->rect().bottom());

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();

    painter.setFont(font());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(number == current ? theme::text() : theme::textDim());
            painter.drawText(0, top, m_lineNumberArea->width() - 6,
                             fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());

    if (rect.contains(viewport()->rect())) updateLineNumberAreaWidth(0);
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> selections;

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection line;
        line.format.setBackground(theme::syntax::currentLine());
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        selections.append(line);
    }

    matchBrackets(selections);
    setExtraSelections(selections);
    m_lineNumberArea->update();
}

void CodeEditor::matchBrackets(QList<QTextEdit::ExtraSelection> &selections)
{
    const QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    const int column = cursor.positionInBlock();

    // Cheap gate first: the whole-buffer tokenise below is only worth paying
    // for when the caret is actually touching a bracket.
    int offset = -1;
    if (column > 0 && (isOpenBracket(line.at(column - 1))
                       || isCloseBracket(line.at(column - 1))))
        offset = cursor.block().position() + column - 1;
    else if (column < line.size() && (isOpenBracket(line.at(column))
                                      || isCloseBracket(line.at(column))))
        offset = cursor.block().position() + column;
    if (offset < 0) return;

    const QString text = toPlainText();
    if (text.size() > kMatchLimit) return;

    const QVector<Token> tokens = EnforceLexer::tokenizeAll(text);
    int index = -1;
    for (int i = 0; i < tokens.size(); ++i) {
        if (tokens.at(i).start > offset) break;
        if (tokens.at(i).start == offset
            && tokens.at(i).kind == TokenKind::Punctuation) {
            index = i;
            break;
        }
    }
    // No punctuation token there means the bracket is inside a string or a
    // comment, where it pairs with nothing.
    if (index < 0) return;

    const QChar self = tokens.at(index).text.at(0);
    const QChar partner = partnerOf(self);
    if (partner.isNull()) return;

    const int step = isOpenBracket(self) ? 1 : -1;
    int depth = 0;
    int match = -1;
    for (int i = index; i >= 0 && i < tokens.size(); i += step) {
        const Token &t = tokens.at(i);
        if (t.kind != TokenKind::Punctuation || t.length != 1) continue;
        const QChar c = t.text.at(0);
        if (c == self) depth += step;
        else if (c == partner) depth -= step;
        else continue;
        if (depth == 0) { match = i; break; }
    }

    const QColor fill = match >= 0 ? theme::syntax::bracketMatch() : theme::errorColor();
    const auto mark = [&](int at) {
        QTextEdit::ExtraSelection sel;
        sel.cursor = textCursor();
        sel.cursor.setPosition(at);
        sel.cursor.setPosition(at + 1, QTextCursor::KeepAnchor);
        sel.format.setBackground(fill);
        sel.format.setForeground(theme::text());
        selections.append(sel);
    };
    mark(offset);
    if (match >= 0) mark(tokens.at(match).start);
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(
        QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::focusInEvent(QFocusEvent *event)
{
    m_completer->setWidget(this);
    QPlainTextEdit::focusInEvent(event);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    QAbstractItemView *popup = m_completer->popup();

    if (popup->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Escape:
            // Left for the completer's own filter: it accepts or dismisses
            // without the character ever reaching the document.
            event->ignore();
            return;
        default:
            break;
        }
    }

    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        indentSelection(this, event->key() == Qt::Key_Backtab
                                  || (event->modifiers() & Qt::ShiftModifier));
        return;
    }

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        insertNewlineWithIndent(this);
        return;
    }

    if (event->text() == QLatin1String("}") && dedentClosingBrace(this)) {
        popup->hide();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);

    const QChar typed = event->text().isEmpty() ? QChar() : event->text().at(0);
    const QString prefix = wordUnderCursor();

    if (typed == QLatin1Char('.')) {
        refreshCompletionModel(QString());
        m_completer->setCompletionPrefix(QString());
        showCompletions(m_completer, this);
        return;
    }

    const bool erasing = event->key() == Qt::Key_Backspace;
    if ((isIdentChar(typed) || erasing) && prefix.size() >= 2) {
        refreshCompletionModel(prefix);
        m_completer->setCompletionPrefix(prefix);
        showCompletions(m_completer, this);
        return;
    }

    if (popup->isVisible()) popup->hide();
}

void CodeEditor::insertCompletion(const QString &completion)
{
    // Replace what was typed rather than appending a suffix: filtering is
    // MatchContains, so the chosen name does not always start with it.
    QTextCursor cursor = textCursor();
    const int typed = wordUnderCursor().size();
    if (typed > 0)
        cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, typed);
    cursor.insertText(completion);
    setTextCursor(cursor);
}

QString CodeEditor::wordUnderCursor() const
{
    const QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    const int end = qMin(cursor.positionInBlock(), line.size());
    int start = end;
    while (start > 0 && isIdentChar(line.at(start - 1))) --start;
    return line.mid(start, end - start);
}

void CodeEditor::refreshCompletionModel(const QString &prefix)
{
    QStringList names;
    QSet<QString> seen;
    const auto add = [&names, &seen](const QString &name) {
        if (name.isEmpty() || seen.contains(name)) return;
        seen.insert(name);
        names << name;
    };

    const QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    int start = qMin(cursor.positionInBlock(), line.size());
    while (start > 0 && isIdentChar(line.at(start - 1))) --start;
    const bool member = start > 0 && line.at(start - 1) == QLatin1Char('.');

    const Catalog *cat = m_doc ? &m_doc->catalog() : nullptr;

    if (member) {
        SearchOptions opts;
        opts.limit = kCompletionLimit;
        opts.ofClass = receiverClass(line.left(start - 1), m_doc);
        if (cat)
            for (const SearchHit &hit : cat->search(prefix, opts))
                add(completionName(hit));
    } else {
        for (const QString &local : symbolsFor(m_doc, m_locals))
            if (prefix.isEmpty() || local.contains(prefix, Qt::CaseInsensitive))
                add(local);

        if (cat) {
            SearchOptions opts;
            opts.limit = kCompletionLimit;
            for (const SearchHit &hit : cat->search(prefix, opts))
                add(completionName(hit));
        }

        for (const QString &word : EnforceLexer::keywords())
            if (word.startsWith(prefix, Qt::CaseInsensitive)) add(word);
        for (const QString &word : EnforceLexer::types())
            if (word.startsWith(prefix, Qt::CaseInsensitive)) add(word);
    }

    static_cast<QStringListModel *>(m_completionModel)->setStringList(names);
}
