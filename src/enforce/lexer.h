// Enforce Script tokeniser.
//
// Enforce is C-like but not C: it has `proto native`, `autoptr`, `ref`,
// `modded`, `typename`, `owned`, `out`/`inout` parameters and a preprocessor
// that only does #ifdef/#define. Keyword set checked against P:\scripts.
//
// One pass, no allocation per token beyond the vector, so the highlighter can
// run it on every visible block and the node canvas on every repaint.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

enum class TokenKind {
    Whitespace,
    Comment,       // // and /* */
    Keyword,       // if, for, class, override, proto, ...
    Type,          // int, float, string, vector, bool, void, array, ...
    Preprocessor,  // #ifdef, #define, ...
    String,        // "..." including escapes
    Number,        // 12, 1.5, 0x1f
    Identifier,
    ClassName,     // identifier that names a known catalogue class
    Operator,
    Punctuation,
    Unknown,
};

struct Token {
    TokenKind kind = TokenKind::Unknown;
    int start = 0;
    int length = 0;
    QString text;
};

// A block comment or string can span lines, so the highlighter has to carry
// state between blocks.
enum class LexState { Normal, InBlockComment };

class EnforceLexer {
public:
    // Tokenises one line. `state` is carried in and updated on the way out.
    static QVector<Token> tokenize(const QString &line, LexState &state);
    // Whole-text convenience; newlines are Whitespace tokens. `endState` reports
    // where the text left the lexer. A caller cannot work that out from the
    // tokens: the first line of a block comment and a genuinely unclosed one are
    // the same token shape, so anything asking "is this comment closed" has to
    // read the state rather than the text.
    static QVector<Token> tokenizeAll(const QString &text, LexState *endState = nullptr);

    static bool isKeyword(const QString &word);
    static bool isType(const QString &word);
    static const QStringList &keywords();
    static const QStringList &types();
};

// What a block of hand-written Enforce refers to and whether it is well formed.
// The analyser uses this to check Raw nodes instead of trusting them blindly,
// and the node canvas uses `summary` for the title.
struct EnforceScan {
    QStringList identifiers;   // every bare identifier, in order, deduplicated
    QStringList calls;         // identifiers immediately followed by '('
    QStringList members;       // things read as x.y, recorded as "y"
    QStringList assignedTo;    // left-hand sides of a plain assignment
    int braceBalance = 0;      // non-zero means the block cannot compile alone
    int parenBalance = 0;
    bool unterminatedString = false;
    bool unterminatedComment = false;
    int statements = 0;        // top-level `;` plus block statements
    QString summary;           // one line fit to show on a node header
};

EnforceScan scanEnforce(const QString &code);

// First meaningful line, trimmed and shortened, for a node title.
QString enforceSummary(const QString &code, int maxChars = 40);
