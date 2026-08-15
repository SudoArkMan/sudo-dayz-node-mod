#include "highlighter.h"

#include "catalog.h"
#include "theme.h"

#include <QTextDocument>

namespace {

QTextCharFormat plain(const QColor &c, bool italic = false)
{
    QTextCharFormat f;
    f.setForeground(c);
    if (italic) f.setFontItalic(true);
    return f;
}

} // namespace

EnforceHighlighter::EnforceHighlighter(QTextDocument *doc, const Catalog *cat)
    : QSyntaxHighlighter(doc), m_cat(cat)
{
    m_formats.insert(TokenKind::Keyword, plain(theme::syntax::keyword()));
    m_formats.insert(TokenKind::Type, plain(theme::syntax::type()));
    m_formats.insert(TokenKind::String, plain(theme::syntax::stringLit()));
    m_formats.insert(TokenKind::Number, plain(theme::syntax::number()));
    m_formats.insert(TokenKind::Comment, plain(theme::syntax::comment(), true));
    m_formats.insert(TokenKind::Preprocessor, plain(theme::syntax::preprocessor()));
    m_formats.insert(TokenKind::Operator, plain(theme::textDim()));
    m_formats.insert(TokenKind::Punctuation, plain(theme::textDim()));
    m_formats.insert(TokenKind::Identifier, plain(theme::text()));
    m_formats.insert(TokenKind::ClassName, plain(theme::syntax::className()));
    m_formats.insert(TokenKind::Unknown, plain(theme::text()));

    m_classFormat = plain(theme::syntax::className());
    m_localFormat = plain(theme::syntax::localName());
    m_unknownFormat = plain(theme::syntax::unknownName());
    m_unknownFormat.setUnderlineStyle(QTextCharFormat::DotLine);
    m_unknownFormat.setUnderlineColor(theme::syntax::unknownName());

    m_problemFormat.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    m_problemFormat.setUnderlineColor(theme::errorColor());
}

void EnforceHighlighter::setLocalSymbols(const QStringList &names)
{
    const QSet<QString> next(names.begin(), names.end());
    if (next == m_locals) return;
    m_locals = next;
    rehighlight();
}

void EnforceHighlighter::setProblemLines(const QSet<int> &lines)
{
    if (lines == m_problemLines) return;
    m_problemLines = lines;
    rehighlight();
}

void EnforceHighlighter::highlightBlock(const QString &text)
{
    // previousBlockState() is -1 on the first block and on any block Qt has not
    // laid out yet, which is the same situation as "not inside a comment".
    LexState state = previousBlockState() == int(LexState::InBlockComment)
                         ? LexState::InBlockComment
                         : LexState::Normal;

    // Line numbers here are 1-based, matching the gutter and the messages
    // validate() hands to the user. CodeEditor is the only caller.
    const int line = currentBlock().blockNumber() + 1;
    const bool problem = m_problemLines.contains(line);

    const QVector<Token> tokens = EnforceLexer::tokenize(text, state);

    // Next and previous tokens that carry meaning, for the two questions the
    // identifier rule below asks.
    const auto neighbour = [&tokens](int from, int step) -> const Token * {
        for (int i = from + step; i >= 0 && i < tokens.size(); i += step) {
            if (tokens.at(i).kind != TokenKind::Whitespace
                && tokens.at(i).kind != TokenKind::Comment)
                return &tokens.at(i);
        }
        return nullptr;
    };

    for (int i = 0; i < tokens.size(); ++i) {
        const Token &t = tokens.at(i);
        if (t.kind == TokenKind::Whitespace) continue;

        if (t.kind == TokenKind::Identifier) {
            // classId is a hash lookup. Nothing in this loop may reach for
            // search(): it runs on every visible block on every keystroke.
            if (m_cat && m_cat->classId(t.text) >= 0) {
                setFormat(t.start, t.length, m_classFormat);
                continue;
            }
            if (m_locals.contains(t.text)) {
                setFormat(t.start, t.length, m_localFormat);
                continue;
            }

            // Only a bare name on a flagged line can be called unresolvable
            // from here. A call may well be a catalogue method, and a member
            // belongs to a receiver this class cannot see, so marking either
            // would be saying more than the highlighter knows. The lookups
            // stay behind `problem` because most lines never need them.
            bool unresolved = false;
            if (problem) {
                const Token *next = neighbour(i, 1);
                const Token *prev = neighbour(i, -1);
                unresolved = !(next && next->text == QLatin1String("("))
                             && !(prev && prev->text == QLatin1String("."));
            }
            setFormat(t.start, t.length,
                      unresolved ? m_unknownFormat
                                 : m_formats.value(TokenKind::Identifier));
            continue;
        }

        setFormat(t.start, t.length, m_formats.value(t.kind));
    }

    if (problem) {
        // From the first real character: an underline dragged out under the
        // indent points at nothing and reads as a second, wider problem.
        int from = 0;
        while (from < text.size() && text.at(from).isSpace()) ++from;
        // Merged per character, so the underline rides over the colours set
        // above instead of flattening the line to one format.
        for (int i = from; i < text.size(); ++i) {
            QTextCharFormat f = format(i);
            f.merge(m_problemFormat);
            setFormat(i, 1, f);
        }
    }

    setCurrentBlockState(int(state));
}
