#include "lexer.h"

#include <QHash>
#include <QSet>

namespace {

// Checked against P:\scripts. `event`, `sealed`, `volatile`, `owned` and
// `thread` are rare but real, and leaving them out makes vanilla-derived code
// highlight wrong in exactly the places worth reading carefully.
const QStringList kKeywords = {
    "auto", "autoptr", "break", "case", "class", "const", "continue", "default",
    "delete", "else", "enum", "event", "extends", "external", "for", "foreach",
    "if", "inout", "modded", "native", "new", "notnull", "out", "override",
    "owned", "private", "proto", "protected", "public", "ref", "reference",
    "return", "sealed", "sizeof", "static", "super", "switch", "this", "thread",
    "typedef", "typeof", "volatile", "while",
};

const QStringList kTypes = {
    "array", "bool", "Class", "float", "int", "map", "Managed", "multiMap",
    "set", "string", "TBoolArray", "TClassArray", "TFloatArray", "TIntArray",
    "TStringArray", "TTypenameArray", "TVectorArray", "typename", "vector",
    "void",
};

const QSet<QString> &keywordSet()
{
    static const QSet<QString> s(kKeywords.begin(), kKeywords.end());
    return s;
}

const QSet<QString> &typeSet()
{
    static const QSet<QString> s(kTypes.begin(), kTypes.end());
    return s;
}

// null/true/false are values, not keywords, but they read as keywords and
// vanilla code is full of them.
const QSet<QString> &literalWords()
{
    static const QSet<QString> s = {"null", "true", "false", "NULL"};
    return s;
}

bool isIdentStart(QChar c) { return c.isLetter() || c == '_'; }
bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == '_'; }

void push(QVector<Token> &out, TokenKind kind, const QString &line, int start, int len)
{
    if (len <= 0) return;
    Token t;
    t.kind = kind;
    t.start = start;
    t.length = len;
    t.text = line.mid(start, len);
    out.append(t);
}

} // namespace

const QStringList &EnforceLexer::keywords() { return kKeywords; }
const QStringList &EnforceLexer::types() { return kTypes; }
bool EnforceLexer::isKeyword(const QString &w) { return keywordSet().contains(w); }
bool EnforceLexer::isType(const QString &w) { return typeSet().contains(w); }

QVector<Token> EnforceLexer::tokenize(const QString &line, LexState &state)
{
    QVector<Token> out;
    const int n = line.size();
    int i = 0;

    if (state == LexState::InBlockComment) {
        const int end = line.indexOf(QStringLiteral("*/"));
        if (end < 0) {
            push(out, TokenKind::Comment, line, 0, n);
            return out;
        }
        push(out, TokenKind::Comment, line, 0, end + 2);
        state = LexState::Normal;
        i = end + 2;
    }

    while (i < n) {
        const QChar c = line.at(i);

        if (c.isSpace()) {
            const int start = i;
            while (i < n && line.at(i).isSpace()) i++;
            push(out, TokenKind::Whitespace, line, start, i - start);
            continue;
        }

        if (c == '/' && i + 1 < n && line.at(i + 1) == '/') {
            push(out, TokenKind::Comment, line, i, n - i);
            break;
        }

        if (c == '/' && i + 1 < n && line.at(i + 1) == '*') {
            const int end = line.indexOf(QStringLiteral("*/"), i + 2);
            if (end < 0) {
                push(out, TokenKind::Comment, line, i, n - i);
                state = LexState::InBlockComment;
                break;
            }
            push(out, TokenKind::Comment, line, i, end + 2 - i);
            i = end + 2;
            continue;
        }

        if (c == '#') {
            const int start = i;
            i++;
            while (i < n && isIdentChar(line.at(i))) i++;
            push(out, TokenKind::Preprocessor, line, start, i - start);
            continue;
        }

        if (c == '"') {
            const int start = i;
            i++;
            while (i < n) {
                if (line.at(i) == '\\' && i + 1 < n) { i += 2; continue; }
                if (line.at(i) == '"') { i++; break; }
                i++;
            }
            push(out, TokenKind::String, line, start, i - start);
            continue;
        }

        if (c.isDigit()
            || (c == '.' && i + 1 < n && line.at(i + 1).isDigit())) {
            const int start = i;
            if (c == '0' && i + 1 < n && (line.at(i + 1) == 'x' || line.at(i + 1) == 'X')) {
                i += 2;
                while (i < n && (line.at(i).isDigit()
                                 || QStringLiteral("abcdefABCDEF").contains(line.at(i)))) i++;
            } else {
                while (i < n && (line.at(i).isDigit() || line.at(i) == '.')) i++;
                if (i < n && (line.at(i) == 'e' || line.at(i) == 'E')) {
                    i++;
                    if (i < n && (line.at(i) == '+' || line.at(i) == '-')) i++;
                    while (i < n && line.at(i).isDigit()) i++;
                }
            }
            push(out, TokenKind::Number, line, start, i - start);
            continue;
        }

        if (isIdentStart(c)) {
            const int start = i;
            while (i < n && isIdentChar(line.at(i))) i++;
            const QString word = line.mid(start, i - start);
            TokenKind kind = TokenKind::Identifier;
            if (keywordSet().contains(word) || literalWords().contains(word))
                kind = TokenKind::Keyword;
            else if (typeSet().contains(word))
                kind = TokenKind::Type;
            push(out, kind, line, start, i - start);
            continue;
        }

        if (QStringLiteral("+-*/%=<>!&|^~?:").contains(c)) {
            const int start = i;
            while (i < n && QStringLiteral("+-*/%=<>!&|^~?:").contains(line.at(i))) i++;
            push(out, TokenKind::Operator, line, start, i - start);
            continue;
        }

        push(out, TokenKind::Punctuation, line, i, 1);
        i++;
    }

    return out;
}

QVector<Token> EnforceLexer::tokenizeAll(const QString &text)
{
    QVector<Token> out;
    LexState state = LexState::Normal;
    int offset = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int li = 0; li < lines.size(); ++li) {
        const QString &line = lines.at(li);
        for (Token t : EnforceLexer::tokenize(line, state)) {
            t.start += offset;
            out.append(t);
        }
        offset += line.size() + 1;
        if (li + 1 < lines.size()) {
            Token nl;
            nl.kind = TokenKind::Whitespace;
            nl.start = offset - 1;
            nl.length = 1;
            nl.text = QStringLiteral("\n");
            out.append(nl);
        }
    }
    return out;
}

EnforceScan scanEnforce(const QString &code)
{
    EnforceScan scan;
    const QVector<Token> tokens = EnforceLexer::tokenizeAll(code);

    QSet<QString> seenIdent, seenCall, seenMember, seenAssign;
    // Walk the significant tokens; whitespace and comments carry no meaning
    // here and would only complicate the lookahead.
    QVector<Token> sig;
    sig.reserve(tokens.size());
    for (const Token &t : tokens)
        if (t.kind != TokenKind::Whitespace && t.kind != TokenKind::Comment)
            sig.append(t);

    for (int i = 0; i < sig.size(); ++i) {
        const Token &t = sig.at(i);

        if (t.kind == TokenKind::Punctuation) {
            if (t.text == QLatin1String("{")) scan.braceBalance++;
            else if (t.text == QLatin1String("}")) scan.braceBalance--;
            else if (t.text == QLatin1String("(")) scan.parenBalance++;
            else if (t.text == QLatin1String(")")) scan.parenBalance--;
            else if (t.text == QLatin1String(";")) scan.statements++;
            continue;
        }

        if (t.kind == TokenKind::String && !t.text.endsWith(QLatin1Char('"')))
            scan.unterminatedString = true;

        if (t.kind != TokenKind::Identifier) continue;

        const bool afterDot = i > 0 && sig.at(i - 1).kind == TokenKind::Punctuation
                              && sig.at(i - 1).text == QLatin1String(".");
        const bool beforeParen = i + 1 < sig.size()
                                 && sig.at(i + 1).kind == TokenKind::Punctuation
                                 && sig.at(i + 1).text == QLatin1String("(");
        const bool beforeAssign = i + 1 < sig.size()
                                  && sig.at(i + 1).kind == TokenKind::Operator
                                  && sig.at(i + 1).text == QLatin1String("=");

        if (afterDot) {
            if (!seenMember.contains(t.text)) { seenMember.insert(t.text); scan.members << t.text; }
        } else if (!seenIdent.contains(t.text)) {
            seenIdent.insert(t.text);
            scan.identifiers << t.text;
        }
        if (beforeParen && !seenCall.contains(t.text)) {
            seenCall.insert(t.text);
            scan.calls << t.text;
        }
        if (beforeAssign && !afterDot && !seenAssign.contains(t.text)) {
            seenAssign.insert(t.text);
            scan.assignedTo << t.text;
        }
    }

    LexState endState = LexState::Normal;
    for (const QString &line : code.split(QLatin1Char('\n')))
        EnforceLexer::tokenize(line, endState);
    scan.unterminatedComment = endState == LexState::InBlockComment;

    if (scan.statements == 0 && !sig.isEmpty()) scan.statements = 1;
    scan.summary = enforceSummary(code);
    return scan;
}

QString enforceSummary(const QString &code, int maxChars)
{
    QString first;
    int extraLines = 0;
    LexState state = LexState::Normal;
    for (const QString &raw : code.split(QLatin1Char('\n'))) {
        // Skip comment-only and blank lines: the point of the summary is what
        // the block DOES, and a leading comment hides that.
        QString stripped;
        for (const Token &t : EnforceLexer::tokenize(raw, state))
            if (t.kind != TokenKind::Comment) stripped += t.text;
        stripped = stripped.trimmed();
        if (stripped.isEmpty()) continue;
        if (first.isEmpty()) first = stripped;
        else extraLines++;
    }
    if (first.isEmpty()) first = code.trimmed().split(QLatin1Char('\n')).value(0).trimmed();

    if (first.size() > maxChars) first = first.left(maxChars - 1).trimmed() + QStringLiteral("...");
    if (extraLines > 0)
        first += QStringLiteral("  +%1").arg(extraLines);
    return first;
}
