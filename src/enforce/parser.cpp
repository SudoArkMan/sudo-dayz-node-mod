// Recursive-descent parser for Enforce Script statement bodies.
//
// Built on the existing tokeniser. It never fails: a statement it cannot
// represent comes back as StmtKind::Raw holding that statement's original text
// and a note saying why, and parsing carries on with the next one. That
// property is what lets the importer stop dumping whole method bodies into a
// text box.
//
// Two conventions the tree uses that the header cannot show:
//   - a postfix `++`/`--` is a Unary whose op reads "post++" / "post--", so
//     `i++` and `++i` stay distinguishable;
//   - a brace initialiser `{1, 2, 3}` is ExprKind::New with an empty typeName,
//     since it constructs a value the same way and needs no new node kind.
//
// Formatting is canonical rather than preserved: a control body always prints
// with braces, and `if (x) y();` parses to the same tree as `if (x) { y(); }`.
// A braced control body is spliced straight into `body`, never wrapped in a
// Block, so parse -> print -> parse is stable instead of growing a brace level
// every pass.

#include "ast.h"

#include "lexer.h"

#include <QRegularExpression>
#include <QSet>

namespace {

// A body is user data, so every recursive walk has to terminate on a
// pathological one. 64 matches the generator's own nesting cutoff.
constexpr int kMaxDepth = 64;

struct Tok {
    TokenKind kind = TokenKind::Unknown;
    QString text;
    int start = 0;
    int length = 0;
    int line = 1;
};

// Thrown when a construct cannot be represented. Caught at the nearest
// statement boundary, where the original text becomes a Raw statement.
struct ParseFail {
    QString why;
};

QString indentOf(int n) { return QString(n, QLatin1Char('\t')); }

// Drops the block's own indentation from a captured raw statement, keeping
// what is relative to it. The first line starts at a token so it never has
// any. Without this, printing a raw block at a new depth and reading it back
// would move its inner lines further right every pass.
QString dedent(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() < 2) return text;
    int common = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QString &l = lines.at(i);
        if (l.trimmed().isEmpty()) continue;
        int n = 0;
        while (n < l.size() && (l.at(n) == QLatin1Char('\t') || l.at(n) == QLatin1Char(' '))) n++;
        common = common < 0 ? n : qMin(common, n);
    }
    if (common <= 0) return text;
    for (int i = 1; i < lines.size(); ++i)
        lines[i] = lines.at(i).size() >= common ? lines.at(i).mid(common)
                                                : lines.at(i).trimmed();
    return lines.join(QLatin1Char('\n'));
}

// Storage and access modifiers that can lead a declaration. `static` and
// `const` appear on locals; the access words only appear on members, but a
// caller can hand this a class body by mistake and the statement is still
// better recognised than raw.
const QSet<QString> &declModifiers()
{
    static const QSet<QString> s = {
        QStringLiteral("ref"),       QStringLiteral("autoptr"),   QStringLiteral("const"),
        QStringLiteral("notnull"),   QStringLiteral("owned"),     QStringLiteral("static"),
        QStringLiteral("private"),   QStringLiteral("protected"), QStringLiteral("public"),
        QStringLiteral("volatile"),  QStringLiteral("reference"), QStringLiteral("local"),
        QStringLiteral("out"),       QStringLiteral("inout"),
    };
    return s;
}

// Every operator, longest first. The tokeniser hands back a run like ">>=" as
// one token and the parser splits every operator into single characters, so
// this table is what puts them back together: maximal munch decides between the
// `>` that closes `array<int>` and the `>>=` in a shift-assign.
const QStringList &allOperators()
{
    static const QStringList ops = {
        QStringLiteral("<<="), QStringLiteral(">>="), QStringLiteral("=="),
        QStringLiteral("!="),  QStringLiteral("<="),  QStringLiteral(">="),
        QStringLiteral("&&"),  QStringLiteral("||"),  QStringLiteral("<<"),
        QStringLiteral(">>"),  QStringLiteral("++"),  QStringLiteral("--"),
        QStringLiteral("+="),  QStringLiteral("-="),  QStringLiteral("*="),
        QStringLiteral("/="),  QStringLiteral("%="),  QStringLiteral("&="),
        QStringLiteral("|="),  QStringLiteral("^="),  QStringLiteral("+"),
        QStringLiteral("-"),   QStringLiteral("*"),   QStringLiteral("/"),
        QStringLiteral("%"),   QStringLiteral("="),   QStringLiteral("<"),
        QStringLiteral(">"),   QStringLiteral("!"),   QStringLiteral("&"),
        QStringLiteral("|"),   QStringLiteral("^"),   QStringLiteral("~"),
        QStringLiteral("?"),   QStringLiteral(":"),
    };
    return ops;
}

const QSet<QString> &assignOperators()
{
    static const QSet<QString> s = {
        QStringLiteral("="),  QStringLiteral("+="),  QStringLiteral("-="),
        QStringLiteral("*="), QStringLiteral("/="),  QStringLiteral("%="),
        QStringLiteral("&="), QStringLiteral("|="),  QStringLiteral("^="),
        QStringLiteral("<<="), QStringLiteral(">>="),
    };
    return s;
}

// Binary precedence, loosest first. Comma is not here: it is only legal in a
// for-loop header, and letting it into general expressions would swallow call
// arguments.
const QVector<QStringList> &binaryLevels()
{
    static const QVector<QStringList> levels = {
        {QStringLiteral("||")},
        {QStringLiteral("&&")},
        {QStringLiteral("|")},
        {QStringLiteral("^")},
        {QStringLiteral("&")},
        {QStringLiteral("=="), QStringLiteral("!=")},
        {QStringLiteral("<"), QStringLiteral("<="), QStringLiteral(">"), QStringLiteral(">=")},
        {QStringLiteral("<<"), QStringLiteral(">>")},
        {QStringLiteral("+"), QStringLiteral("-")},
        {QStringLiteral("*"), QStringLiteral("/"), QStringLiteral("%")},
    };
    return levels;
}

// A vector in Enforce is written as a string of three numbers. Nothing in the
// text says which one it is, so the shape has to decide: getting it wrong only
// costs the lowering a more specific literal node.
bool looksLikeVectorLiteral(const QString &quoted)
{
    static const QRegularExpression re(
        QStringLiteral("^\"\\s*[-+]?[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?"
                       "(\\s+[-+]?[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?){2}\\s*\"$"));
    return re.match(quoted).hasMatch();
}

bool identLike(TokenKind k)
{
    return k == TokenKind::Identifier || k == TokenKind::Type || k == TokenKind::ClassName;
}

// Whether `X` in `X.Cast(...)` names a class rather than a variable. Cast and
// CastTo are statics, so the object is always a type; anything else keeps the
// plain method-call shape, which still generates the same text.
bool classNameShape(const QString &name)
{
    if (name.isEmpty()) return false;
    if (EnforceLexer::isType(name)) return true;
    return name.at(0).isUpper();
}

ExprPtr makeExpr(ExprKind k, int line)
{
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->line = line;
    return e;
}

StmtPtr makeStmt(StmtKind k, int line)
{
    auto s = std::make_unique<Stmt>();
    s->kind = k;
    s->line = line;
    return s;
}

class Parser {
public:
    explicit Parser(const QString &src) : m_src(src) { build(); }

    ParseResult run()
    {
        ParseResult result;
        // Copied after the walk, not before it: parsing itself reports a
        // close brace that belongs to nothing.
        result.statements = parseSequence(false);
        result.errors = m_errors;
        result.notes = m_notes;
        for (const StmtPtr &s : result.statements) count(s.get(), result);
        return result;
    }

private:
    QString m_src;
    QVector<Tok> m_toks;
    int m_pos = 0;
    int m_depth = 0;
    QStringList m_notes;
    QStringList m_errors;
    Tok m_eof;

    // ------------------------------------------------------------- tokens

    void build()
    {
        // The lexer works a line at a time, so the first line of a block comment
        // and a comment nobody closed are the same token: text that opens with
        // `/*` and does not close. Asking the lexer where it finished is the only
        // way to tell them apart, and getting it wrong reported a syntax error on
        // every body holding a comment over two lines, which stopped the whole
        // body from being lowered.
        LexState endState = LexState::Normal;
        const QVector<Token> raw = EnforceLexer::tokenizeAll(m_src, &endState);
        int cursor = 0;
        int line = 1;
        int paren = 0, brace = 0, bracket = 0;
        const bool unterminatedComment = endState == LexState::InBlockComment;
        bool unterminatedString = false;

        for (const Token &t : raw) {
            // Line numbers come from walking the gap since the last token, so
            // a multi-line comment or string counts every newline it holds.
            for (int i = cursor; i < t.start && i < m_src.size(); ++i)
                if (m_src.at(i) == QLatin1Char('\n')) line++;
            const int tokenLine = line;
            for (int i = t.start; i < t.start + t.length && i < m_src.size(); ++i)
                if (m_src.at(i) == QLatin1Char('\n')) line++;
            cursor = t.start + t.length;

            if (t.kind == TokenKind::Comment) continue;
            if (t.kind == TokenKind::Whitespace) continue;
            if (t.kind == TokenKind::String
                && (t.length < 2 || !t.text.endsWith(QLatin1Char('"'))))
                unterminatedString = true;

            if (t.kind == TokenKind::Operator) {
                // Split into single characters. Re-joining them by adjacency is
                // how `array<array<int>>` and `x >>= 2` can share a spelling.
                for (int i = 0; i < t.text.size(); ++i) {
                    Tok o;
                    o.kind = TokenKind::Operator;
                    o.text = t.text.mid(i, 1);
                    o.start = t.start + i;
                    o.length = 1;
                    o.line = tokenLine;
                    m_toks.append(o);
                }
                continue;
            }

            Tok tok;
            tok.kind = t.kind;
            tok.text = t.text;
            tok.start = t.start;
            tok.length = t.length;
            tok.line = tokenLine;
            m_toks.append(tok);

            if (t.kind == TokenKind::Punctuation) {
                if (t.text == QLatin1String("(")) paren++;
                else if (t.text == QLatin1String(")")) paren--;
                else if (t.text == QLatin1String("{")) brace++;
                else if (t.text == QLatin1String("}")) brace--;
                else if (t.text == QLatin1String("[")) bracket++;
                else if (t.text == QLatin1String("]")) bracket--;
            }
        }

        m_eof.kind = TokenKind::Unknown;
        m_eof.start = m_src.size();
        m_eof.line = line;

        // A body that does not close its own brackets cannot be cut into
        // statements at all, so the caller is told to leave the text alone
        // rather than handed a tree built out of guesses.
        if (brace != 0)
            m_errors << QStringLiteral("Braces are unbalanced (%1 %2).")
                            .arg(qAbs(brace))
                            .arg(brace > 0 ? QStringLiteral("unclosed") : QStringLiteral("extra"));
        if (paren != 0)
            m_errors << QStringLiteral("Parentheses are unbalanced (%1 %2).")
                            .arg(qAbs(paren))
                            .arg(paren > 0 ? QStringLiteral("unclosed") : QStringLiteral("extra"));
        if (bracket != 0)
            m_errors << QStringLiteral("Square brackets are unbalanced (%1 %2).")
                            .arg(qAbs(bracket))
                            .arg(bracket > 0 ? QStringLiteral("unclosed")
                                             : QStringLiteral("extra"));
        if (unterminatedString) m_errors << QStringLiteral("A string literal is not closed.");
        if (unterminatedComment) m_errors << QStringLiteral("A block comment is not closed.");
    }

    const Tok &peek(int ahead = 0) const
    {
        const int i = m_pos + ahead;
        return i >= 0 && i < m_toks.size() ? m_toks.at(i) : m_eof;
    }
    const Tok &cur() const { return peek(0); }
    bool atEnd() const { return m_pos >= m_toks.size(); }
    int line() const { return cur().line; }

    bool at(const char *text) const { return !atEnd() && cur().text == QLatin1String(text); }
    bool accept(const char *text)
    {
        if (!at(text)) return false;
        m_pos++;
        return true;
    }
    void expect(const char *text, const char *what)
    {
        if (accept(text)) return;
        throw ParseFail{QStringLiteral("expected '%1' %2 but found '%3'")
                            .arg(QLatin1String(text), QLatin1String(what),
                                 atEnd() ? QStringLiteral("end of code") : cur().text)};
    }

    // Enforce lets the last statement in a block drop its semicolon, and
    // vanilla relies on it: `break` with no `;` before the closing brace
    // appears in several switch bodies. Code handed in on its own ends the same
    // way, which is how the expression inside a raw node parses at all.
    void expectSemicolon(const char *what)
    {
        if (accept(";") || atEnd() || at("}")) return;
        // Vanilla also drops it on a statement whose successor starts on the
        // next line (`ImageWidget badge_widget` in ingamehud.c). The compiler
        // takes it, so refusing would send working code to a raw node, and
        // regenerating puts the semicolon back.
        if (cur().line > peek(-1).line && opensStatement(cur())) return;
        throw ParseFail{QStringLiteral("expected ';' %1 but found '%2'")
                            .arg(QLatin1String(what), cur().text)};
    }

    // Longest operator starting here, assembled from adjacent single-character
    // operator tokens. Adjacency matters: `a > > b` is not a shift.
    QString peekOperator() const
    {
        QString run;
        for (int i = 0; i < 3; ++i) {
            const Tok &t = peek(i);
            if (t.kind != TokenKind::Operator) break;
            if (i > 0) {
                const Tok &prev = peek(i - 1);
                if (t.start != prev.start + prev.length) break;
            }
            run += t.text;
        }
        while (!run.isEmpty()) {
            if (allOperators().contains(run)) return run;
            run.chop(1);
        }
        return QString();
    }

    bool acceptOperator(const QString &op)
    {
        if (peekOperator() != op) return false;
        m_pos += op.size();
        return true;
    }

    // ------------------------------------------------------------ recovery

    // Whether a token can open a statement, and whether the one before it says
    // the statement is unfinished. Together they find the end of a broken
    // statement that never reached its semicolon.
    //
    // `{` is deliberately not an opener. Enforce puts the brace of a block on
    // its own line, so treating it as a new statement cuts a broken `switch`
    // off at its header and spills the case labels into the enclosing body.
    static bool opensStatement(const Tok &t)
    {
        return identLike(t.kind) || t.kind == TokenKind::Keyword
               || t.kind == TokenKind::Preprocessor;
    }
    static bool continuesStatement(const Tok &t)
    {
        if (t.kind == TokenKind::Operator) return true;
        return t.text == QLatin1String(",") || t.text == QLatin1String("(")
               || t.text == QLatin1String("[") || t.text == QLatin1String(".");
    }

    // Advance past the end of the statement that starts here, so one construct
    // the parser declines does not swallow the rest of the body. Stops after a
    // `;` at depth zero, or after the `}` that closes a block this statement
    // opened, or before a `}` that closes an enclosing block.
    void skipStatement()
    {
        const int from = m_pos;
        int paren = 0, bracket = 0, brace = 0;
        while (!atEnd()) {
            // Nothing is open, the line has changed and the next token starts
            // something new: the damage ends here rather than eating the
            // statement after it.
            if (m_pos > from && paren == 0 && brace == 0 && bracket == 0) {
                const Tok &prev = peek(-1);
                if (cur().line > prev.line && opensStatement(cur()) && !continuesStatement(prev))
                    return;
            }
            const QString t = cur().text;
            const bool punct = cur().kind == TokenKind::Punctuation;
            if (punct && t == QLatin1String("(")) paren++;
            else if (punct && t == QLatin1String(")")) { if (paren > 0) paren--; }
            else if (punct && t == QLatin1String("[")) bracket++;
            else if (punct && t == QLatin1String("]")) { if (bracket > 0) bracket--; }
            else if (punct && t == QLatin1String("{")) brace++;
            else if (punct && t == QLatin1String("}")) {
                if (brace == 0) return; // belongs to the enclosing block
                brace--;
                m_pos++;
                if (brace == 0 && paren == 0 && bracket == 0) return;
                continue;
            } else if (punct && t == QLatin1String(";") && paren == 0 && brace == 0
                       && bracket == 0) {
                m_pos++;
                return;
            }
            m_pos++;
        }
    }

    // Everything from token `from` up to the token before `to`, exactly as the
    // author wrote it, comments included.
    QString sourceBetween(int from, int to) const
    {
        if (from >= m_toks.size() || to <= from) return QString();
        const int start = m_toks.at(from).start;
        const int last = qMin(to, m_toks.size()) - 1;
        const int end = m_toks.at(last).start + m_toks.at(last).length;
        return m_src.mid(start, end - start);
    }

    StmtPtr makeRaw(int from, int to, const QString &why)
    {
        const int ln = from < m_toks.size() ? m_toks.at(from).line : 0;
        StmtPtr s = makeStmt(StmtKind::Raw, ln);
        s->text = dedent(sourceBetween(from, to).trimmed());
        if (s->text.isEmpty()) s->text = QStringLiteral(";");
        static const QRegularExpression runs(QStringLiteral("\\s+"));
        QString head = s->text;
        head.replace(runs, QStringLiteral(" "));
        head = head.trimmed();
        if (head.size() > 60) head = head.left(57) + QStringLiteral("...");
        m_notes << QStringLiteral("line %1: %2 (%3)").arg(ln).arg(why, head);
        return s;
    }

    // One statement, with the whole recovery path attached. Returns null only
    // when the token was a stray `;`.
    StmtPtr parseStatementSafely()
    {
        if (accept(";")) return nullptr;
        const int start = m_pos;
        try {
            StmtPtr s = parseStatement();
            if (s) return s;
        } catch (const ParseFail &f) {
            m_pos = start;
            skipStatement();
            if (m_pos <= start) m_pos = start + 1; // never stall on one token
            return makeRaw(start, m_pos, f.why);
        }
        m_pos = start;
        skipStatement();
        if (m_pos <= start) m_pos = start + 1;
        return makeRaw(start, m_pos, QStringLiteral("statement not recognised"));
    }

    // Statements until `}` (when nested) or end of input.
    std::vector<StmtPtr> parseSequence(bool nested)
    {
        std::vector<StmtPtr> out;
        while (!atEnd()) {
            if (at("}")) {
                if (nested) break;
                // A close brace with nothing open is a body that was cut in the
                // wrong place. Recorded, then stepped over so the rest still
                // parses.
                m_errors << QStringLiteral("Unexpected '}' on line %1.").arg(line());
                m_pos++;
                continue;
            }
            const int before = m_pos;
            StmtPtr s = parseStatementSafely();
            if (s) out.push_back(std::move(s));
            if (m_pos == before) m_pos++; // guaranteed progress
        }
        return out;
    }

    // The body of an if/for/while/foreach. A braced body is spliced in
    // directly: wrapping it in a Block would add a brace level on every
    // parse-print round trip.
    std::vector<StmtPtr> parseControlBody()
    {
        if (accept("{")) {
            std::vector<StmtPtr> body = parseSequence(true);
            expect("}", "to close the block");
            return body;
        }
        std::vector<StmtPtr> body;
        StmtPtr s = parseStatementSafely();
        if (s) body.push_back(std::move(s));
        return body;
    }

    // ----------------------------------------------------------- statements

    StmtPtr parseStatement()
    {
        if (m_depth >= kMaxDepth)
            throw ParseFail{QStringLiteral("nested deeper than %1 levels").arg(kMaxDepth)};
        const int ln = line();

        if (cur().kind == TokenKind::Preprocessor) {
            // Conditional compilation cuts across statements, so it cannot be a
            // statement itself. The directive line is kept verbatim and the
            // code around it still parses.
            const int start = m_pos;
            const int directiveLine = ln;
            while (!atEnd() && cur().line == directiveLine) m_pos++;
            return makeRaw(start, m_pos, QStringLiteral("preprocessor directive"));
        }

        if (at("{")) {
            m_pos++;
            StmtPtr s = makeStmt(StmtKind::Block, ln);
            m_depth++;
            s->body = parseSequence(true);
            m_depth--;
            expect("}", "to close the block");
            return s;
        }
        if (at("if")) return parseIf();
        if (at("for")) return parseFor();
        if (at("foreach")) return parseForEach();
        if (at("while")) return parseWhile();
        if (at("switch")) return parseSwitch();
        if (at("do"))
            throw ParseFail{QStringLiteral("do-while has no node shape")};
        if (at("class") || at("modded") || at("enum") || at("typedef"))
            throw ParseFail{QStringLiteral("declaration, not a statement")};
        if (at("case") || at("default"))
            throw ParseFail{QStringLiteral("case outside a switch")};
        if (at("else"))
            throw ParseFail{QStringLiteral("else with no if")};

        if (at("return")) {
            m_pos++;
            StmtPtr s = makeStmt(StmtKind::Return, ln);
            if (!at(";")) s->expr = parseExpression();
            expectSemicolon("after return");
            return s;
        }
        if (at("break")) {
            m_pos++;
            expectSemicolon("after break");
            return makeStmt(StmtKind::Break, ln);
        }
        if (at("continue")) {
            m_pos++;
            expectSemicolon("after continue");
            return makeStmt(StmtKind::Continue, ln);
        }
        if (at("delete")) {
            m_pos++;
            StmtPtr s = makeStmt(StmtKind::Delete, ln);
            s->expr = parseExpression();
            expectSemicolon("after delete");
            return s;
        }

        if (looksLikeDeclaration()) return parseVarDecl(true);
        if (looksLikeConstructedDeclaration())
            throw ParseFail{QStringLiteral("a declaration that runs a constructor has no node "
                                           "shape")};

        StmtPtr s = makeStmt(StmtKind::Expression, ln);
        s->expr = parseExpression();
        expectSemicolon("after the expression");
        return s;
    }

    StmtPtr parseIf()
    {
        const int ln = line();
        expect("if", "");
        expect("(", "after if");
        StmtPtr s = makeStmt(StmtKind::If, ln);
        s->expr = parseExpression();
        expect(")", "after the condition");
        m_depth++;
        s->body = parseControlBody();
        if (accept("else")) {
            s->elseBody = parseControlBody();
            // An else whose body is empty still has to leave something behind,
            // or printing the tree drops the branch and changes the code.
            if (s->elseBody.empty()) s->elseBody.push_back(makeStmt(StmtKind::Block, line()));
        }
        m_depth--;
        return s;
    }

    StmtPtr parseWhile()
    {
        const int ln = line();
        expect("while", "");
        expect("(", "after while");
        StmtPtr s = makeStmt(StmtKind::While, ln);
        s->expr = parseExpression();
        expect(")", "after the condition");
        m_depth++;
        s->body = parseControlBody();
        m_depth--;
        return s;
    }

    StmtPtr parseFor()
    {
        const int ln = line();
        expect("for", "");
        expect("(", "after for");
        StmtPtr s = makeStmt(StmtKind::For, ln);

        if (!accept(";")) {
            if (looksLikeDeclaration()) {
                s->forInit = parseVarDecl(true);
            } else {
                StmtPtr init = makeStmt(StmtKind::Expression, line());
                init->expr = parseCommaExpression();
                expect(";", "after the loop initialiser");
                s->forInit = std::move(init);
            }
        }
        if (!at(";")) s->forCond = parseExpression();
        expect(";", "after the loop condition");
        if (!at(")")) s->forStep = parseCommaExpression();
        expect(")", "after the loop step");

        m_depth++;
        s->body = parseControlBody();
        m_depth--;
        return s;
    }

    StmtPtr parseForEach()
    {
        const int ln = line();
        expect("foreach", "");
        expect("(", "after foreach");
        StmtPtr s = makeStmt(StmtKind::ForEach, ln);

        QString firstType = parseTypeName();
        if (!identLike(cur().kind))
            throw ParseFail{QStringLiteral("expected a loop variable name")};
        QString firstName = cur().text;
        m_pos++;

        if (accept(",")) {
            // Two variables: the first is the index or the map key, and its
            // type is not always int.
            s->eachIndexType = firstType;
            s->eachIndexName = firstName;
            s->typeName = parseTypeName();
            if (!identLike(cur().kind))
                throw ParseFail{QStringLiteral("expected a loop variable name")};
            s->eachValueName = cur().text;
            m_pos++;
        } else {
            s->typeName = firstType;
            s->eachValueName = firstName;
        }

        if (!acceptOperator(QStringLiteral(":")))
            throw ParseFail{QStringLiteral("expected ':' before the collection")};
        s->eachCollection = parseExpression();
        expect(")", "after the collection");
        m_depth++;
        s->body = parseControlBody();
        m_depth--;
        return s;
    }

    StmtPtr parseSwitch()
    {
        const int ln = line();
        expect("switch", "");
        expect("(", "after switch");
        StmtPtr s = makeStmt(StmtKind::Switch, ln);
        s->expr = parseExpression();
        expect(")", "after the switch value");
        expect("{", "to open the switch body");

        bool started = false;
        m_depth++;
        while (!atEnd() && !at("}")) {
            if (at("case") || at("default")) {
                SwitchCase c;
                if (accept("case")) {
                    c.value = parseExpression();
                } else {
                    m_pos++; // default
                }
                if (!acceptOperator(QStringLiteral(":")))
                    throw ParseFail{QStringLiteral("expected ':' after a case label")};
                s->cases.push_back(std::move(c));
                started = true;
                continue;
            }
            if (!started) throw ParseFail{QStringLiteral("code before the first case label")};
            const int before = m_pos;
            StmtPtr inner = parseStatementSafely();
            if (inner) s->cases.back().body.push_back(std::move(inner));
            if (m_pos == before) m_pos++;
        }
        m_depth--;
        expect("}", "to close the switch body");
        return s;
    }

    // A declaration and an expression start the same way, so the type scan runs
    // speculatively: `Foo bar = x;` is one, `Foo.Bar(x);` and `foo = x;` are
    // not. The decider is a name directly after a complete type.
    bool looksLikeDeclaration()
    {
        const int save = m_pos;
        bool decl = false;
        try {
            const QString type = parseTypeName();
            if (!type.isEmpty() && identLike(cur().kind)) {
                const QString next = peek(1).text;
                decl = next == QLatin1String("=") || next == QLatin1String(",")
                       || next == QLatin1String(";") || next == QLatin1String("[")
                       // a declaration with no initialiser and no semicolon,
                       // which vanilla has in ingamehud.c
                       || peek(1).line > cur().line;
            }
        } catch (const ParseFail &) {
            decl = false;
        }
        m_pos = save;
        return decl;
    }

    // `ScriptInputUserData serializer();` declares a local and runs its
    // constructor. Vanilla writes it, so does CF, and the graph has no shape for
    // it. Naming the shape is worth the scan on its own: the reason the importer
    // showed came from the expression path, so authors were told their code was
    // missing a semicolon. Nothing here changes what is kept, because a refused
    // statement keeps its own text either way.
    bool looksLikeConstructedDeclaration()
    {
        const int save = m_pos;
        bool decl = false;
        try {
            const QString type = parseTypeName();
            decl = !type.isEmpty() && identLike(cur().kind)
                   && peek(1).kind == TokenKind::Punctuation
                   && peek(1).text == QLatin1String("(");
        } catch (const ParseFail &) {
            decl = false;
        }
        m_pos = save;
        return decl;
    }

    StmtPtr parseVarDecl(bool consumeSemicolon)
    {
        const int ln = line();
        StmtPtr s = makeStmt(StmtKind::VarDecl, ln);
        s->typeName = parseTypeName();
        do {
            if (!identLike(cur().kind))
                throw ParseFail{QStringLiteral("expected a variable name")};
            Declarator d;
            d.name = cur().text;
            m_pos++;
            if (at("["))
                throw ParseFail{QStringLiteral("fixed-size array declaration has no node shape")};
            if (acceptOperator(QStringLiteral("="))) d.init = parseExpression();
            s->decls.push_back(std::move(d));
        } while (accept(","));
        if (consumeSemicolon) expectSemicolon("after the declaration");
        return s;
    }

    // Modifiers, a base name, then generic arguments. Returns the type exactly
    // as written apart from spacing, since the lowering wants `ref` and the
    // element type of an `array<ref ItemBase>` both.
    QString parseTypeName()
    {
        QString out;
        while (!atEnd() && cur().kind == TokenKind::Keyword
               && declModifiers().contains(cur().text)) {
            out += cur().text + QLatin1Char(' ');
            m_pos++;
        }
        if (atEnd()) throw ParseFail{QStringLiteral("expected a type")};
        const bool usable = identLike(cur().kind)
                            || (cur().kind == TokenKind::Keyword
                                && cur().text == QLatin1String("auto"));
        if (!usable)
            throw ParseFail{QStringLiteral("expected a type but found '%1'").arg(cur().text)};
        out += cur().text;
        m_pos++;

        if (at("<")) {
            m_pos++;
            out += QLatin1Char('<');
            for (;;) {
                out += parseTypeName();
                if (accept(",")) {
                    out += QStringLiteral(", ");
                    continue;
                }
                break;
            }
            // Every operator is a single token here, so the `>>` that closes two
            // generics needs no special case.
            if (!at(">")) throw ParseFail{QStringLiteral("expected '>' to close the type")};
            m_pos++;
            out += QLatin1Char('>');
        }
        return out;
    }

    // ---------------------------------------------------------- expressions

    ExprPtr parseCommaExpression()
    {
        ExprPtr left = parseExpression();
        while (at(",")) {
            const int ln = line();
            m_pos++;
            ExprPtr e = makeExpr(ExprKind::Binary, ln);
            e->op = QStringLiteral(",");
            e->target = std::move(left);
            e->second = parseExpression();
            left = std::move(e);
        }
        return left;
    }

    ExprPtr parseExpression() { return parseAssign(); }

    ExprPtr parseAssign()
    {
        ExprPtr left = parseTernary();
        const QString op = peekOperator();
        if (!assignOperators().contains(op)) return left;
        const int ln = line();
        m_pos += op.size();
        ExprPtr e = makeExpr(ExprKind::Assign, ln);
        e->op = op;
        e->target = std::move(left);
        e->second = parseAssign(); // right associative
        return e;
    }

    ExprPtr parseTernary()
    {
        ExprPtr cond = parseBinary(0);
        if (peekOperator() != QLatin1String("?")) return cond;
        const int ln = line();
        m_pos++;
        ExprPtr e = makeExpr(ExprKind::Ternary, ln);
        e->target = std::move(cond);
        e->second = parseAssign();
        if (!acceptOperator(QStringLiteral(":")))
            throw ParseFail{QStringLiteral("expected ':' in a ternary")};
        e->third = parseTernary(); // right associative
        return e;
    }

    ExprPtr parseBinary(int level)
    {
        if (level >= binaryLevels().size()) return parseUnary();
        ExprPtr left = parseBinary(level + 1);
        for (;;) {
            const QString op = peekOperator();
            if (op.isEmpty() || !binaryLevels().at(level).contains(op)) return left;
            const int ln = line();
            m_pos += op.size();
            ExprPtr e = makeExpr(ExprKind::Binary, ln);
            e->op = op;
            e->target = std::move(left);
            e->second = parseBinary(level + 1);
            left = std::move(e);
        }
    }

    ExprPtr parseUnary()
    {
        const QString op = peekOperator();
        static const QStringList prefixes = {
            QStringLiteral("!"), QStringLiteral("-"), QStringLiteral("+"),
            QStringLiteral("~"), QStringLiteral("++"), QStringLiteral("--"),
        };
        if (!op.isEmpty() && prefixes.contains(op)) {
            const int ln = line();
            m_pos += op.size();
            ExprPtr e = makeExpr(ExprKind::Unary, ln);
            e->op = op;
            e->target = parseUnary();
            return e;
        }
        // Word-shaped prefixes. `thread Fn()` runs a call on its own thread and
        // `out x` marks an argument the callee writes; both have to survive a
        // round trip even though neither becomes its own node.
        if (cur().kind == TokenKind::Keyword
            && (at("thread") || at("out") || at("inout") || at("typeof") || at("sizeof"))) {
            const int ln = line();
            ExprPtr e = makeExpr(ExprKind::Unary, ln);
            e->op = cur().text;
            m_pos++;
            e->target = parseUnary();
            return e;
        }
        return parsePostfix();
    }

    // Arguments up to `closer`. A trailing comma before the close is accepted:
    // vanilla writes one in cameratoolsmenu.c and the compiler takes it.
    void parseArgs(std::vector<ExprPtr> *args, const char *closer)
    {
        if (at(closer)) return;
        for (;;) {
            args->push_back(parseExpression());
            if (!accept(",")) return;
            if (at(closer)) return;
        }
    }

    ExprPtr parsePostfix()
    {
        ExprPtr e = parsePrimary();
        for (;;) {
            if (at(".")) {
                const int ln = line();
                m_pos++;
                if (!identLike(cur().kind))
                    throw ParseFail{QStringLiteral("expected a member name after '.'")};
                ExprPtr m = makeExpr(ExprKind::Member, ln);
                m->text = cur().text;
                m->target = std::move(e);
                m_pos++;
                e = std::move(m);
                continue;
            }
            if (at("(")) {
                const int ln = line();
                m_pos++;
                std::vector<ExprPtr> args;
                parseArgs(&args, ")");
                expect(")", "to close the argument list");
                e = buildCall(std::move(e), std::move(args), ln);
                continue;
            }
            if (at("[")) {
                const int ln = line();
                m_pos++;
                ExprPtr ix = makeExpr(ExprKind::Index, ln);
                ix->target = std::move(e);
                ix->second = parseExpression();
                expect("]", "to close the index");
                e = std::move(ix);
                continue;
            }
            const QString op = peekOperator();
            if (op == QLatin1String("++") || op == QLatin1String("--")) {
                const int ln = line();
                m_pos += 2;
                ExprPtr u = makeExpr(ExprKind::Unary, ln);
                u->op = QStringLiteral("post") + op;
                u->target = std::move(e);
                e = std::move(u);
                continue;
            }
            return e;
        }
    }

    // A call whose callee is `SomeClass.Cast` or `SomeClass.CastTo` is the
    // Enforce cast, and lowering it to a Cast node is the whole reason this
    // parser exists. Anything else stays a plain call.
    ExprPtr buildCall(ExprPtr callee, std::vector<ExprPtr> args, int ln)
    {
        if (callee && callee->kind == ExprKind::Member && callee->target
            && callee->target->kind == ExprKind::Name
            && classNameShape(callee->target->text)) {
            const QString name = callee->text;
            if (name == QLatin1String("Cast") && args.size() == 1) {
                ExprPtr c = makeExpr(ExprKind::Cast, ln);
                c->typeName = callee->target->text;
                c->op = QStringLiteral("Cast");
                c->target = std::move(args.front());
                return c;
            }
            if (name == QLatin1String("CastTo") && args.size() == 2) {
                ExprPtr c = makeExpr(ExprKind::Cast, ln);
                c->typeName = callee->target->text;
                c->op = QStringLiteral("CastTo");
                c->second = std::move(args.at(0)); // destination
                c->target = std::move(args.at(1)); // value being cast
                return c;
            }
        }
        ExprPtr call = makeExpr(ExprKind::Call, ln);
        call->target = std::move(callee);
        call->args = std::move(args);
        return call;
    }

    ExprPtr parsePrimary()
    {
        if (m_depth >= kMaxDepth)
            throw ParseFail{QStringLiteral("nested deeper than %1 levels").arg(kMaxDepth)};
        if (atEnd()) throw ParseFail{QStringLiteral("code ends in the middle of an expression")};
        const int ln = line();
        const Tok &t = cur();

        if (t.kind == TokenKind::Number) {
            ExprPtr e = makeExpr(ExprKind::Literal, ln);
            const bool hex = t.text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive);
            e->literalType = (!hex && (t.text.contains(QLatin1Char('.'))
                                       || t.text.contains(QLatin1Char('e'))
                                       || t.text.contains(QLatin1Char('E'))))
                                 ? LiteralType::Float
                                 : LiteralType::Int;
            e->text = t.text;
            m_pos++;
            return e;
        }
        if (t.kind == TokenKind::String) {
            ExprPtr e = makeExpr(ExprKind::Literal, ln);
            e->literalType =
                looksLikeVectorLiteral(t.text) ? LiteralType::Vector : LiteralType::String;
            e->text = t.text; // quotes kept, so the text goes back out unchanged
            m_pos++;
            return e;
        }
        if (t.kind == TokenKind::Keyword) {
            if (t.text == QLatin1String("true") || t.text == QLatin1String("false")) {
                ExprPtr e = makeExpr(ExprKind::Literal, ln);
                e->literalType = LiteralType::Bool;
                e->text = t.text;
                m_pos++;
                return e;
            }
            if (t.text == QLatin1String("null") || t.text == QLatin1String("NULL")) {
                ExprPtr e = makeExpr(ExprKind::Literal, ln);
                e->literalType = LiteralType::Null;
                e->text = t.text;
                m_pos++;
                return e;
            }
            if (t.text == QLatin1String("this") || t.text == QLatin1String("super")) {
                ExprPtr e = makeExpr(ExprKind::Name, ln);
                e->text = t.text;
                m_pos++;
                return e;
            }
            if (t.text == QLatin1String("new")) {
                m_pos++;
                ExprPtr e = makeExpr(ExprKind::New, ln);
                e->typeName = parseTypeName();
                if (accept("(")) {
                    e->op = QStringLiteral("()"); // `new array<int>` also exists
                    parseArgs(&e->args, ")");
                    expect(")", "to close the constructor arguments");
                }
                return e;
            }
        }
        if (identLike(t.kind)) {
            ExprPtr e = makeExpr(ExprKind::Name, ln);
            // A generic class used as a value: `JsonFileLoader<Cfg>.LoadFile(x)`
            // or `Param2<float, float>.Cast(p)`. Only taken when a `.` or `(`
            // follows the closing angle bracket, so `a < b` stays a comparison.
            if (peek(1).text == QLatin1String("<")) {
                const int save = m_pos;
                bool generic = false;
                QString name;
                try {
                    name = parseTypeName();
                    generic = at(".") || at("(");
                } catch (const ParseFail &) {
                    generic = false;
                }
                if (generic) {
                    e->text = name;
                    return e;
                }
                m_pos = save;
            }
            e->text = t.text;
            m_pos++;
            return e;
        }
        if (at("(")) {
            // `(int)value`. Only the primitive types take this form: a class
            // cast is written `Class.Cast(x)` in Enforce, and nothing else can
            // be told apart from a parenthesised expression, since no variable
            // can be named `int`.
            static const QSet<QString> castable = {
                QStringLiteral("int"),    QStringLiteral("float"),
                QStringLiteral("bool"),   QStringLiteral("string"),
                QStringLiteral("vector"),
            };
            if (peek(1).kind == TokenKind::Type && castable.contains(peek(1).text)
                && peek(2).text == QLatin1String(")")) {
                ExprPtr c = makeExpr(ExprKind::Cast, ln);
                c->typeName = peek(1).text;
                c->op = QStringLiteral("prefix");
                m_pos += 3;
                c->target = parseUnary();
                return c;
            }
            m_pos++;
            ExprPtr e = makeExpr(ExprKind::Paren, ln);
            m_depth++;
            e->target = parseExpression();
            m_depth--;
            expect(")", "to close the group");
            return e;
        }
        if (at("{")) {
            // A brace initialiser builds a value, so it is a New with no class.
            m_pos++;
            ExprPtr e = makeExpr(ExprKind::New, ln);
            m_depth++;
            parseArgs(&e->args, "}");
            m_depth--;
            expect("}", "to close the initialiser");
            return e;
        }
        throw ParseFail{QStringLiteral("unexpected '%1'").arg(t.text)};
    }

    // ------------------------------------------------------------- counting

    static void count(const Stmt *s, ParseResult &r)
    {
        if (!s) return;
        r.statementCount++;
        if (s->kind == StmtKind::Raw) {
            r.rawCount++;
            return;
        }
        for (const StmtPtr &c : s->body) count(c.get(), r);
        for (const StmtPtr &c : s->elseBody) count(c.get(), r);
        for (const SwitchCase &c : s->cases)
            for (const StmtPtr &b : c.body) count(b.get(), r);
        count(s->forInit.get(), r);
    }
};

// ------------------------------------------------------------------ printing

QString exprText(const Expr *e, int depth);

QString childText(const ExprPtr &p, int depth)
{
    return p ? exprText(p.get(), depth) : QString();
}

QString argList(const std::vector<ExprPtr> &args, int depth)
{
    QStringList parts;
    parts.reserve(int(args.size()));
    for (const ExprPtr &a : args) parts << childText(a, depth);
    return parts.join(QStringLiteral(", "));
}

QString exprText(const Expr *e, int depth)
{
    if (!e) return QString();
    // exprToText is public, so it can be handed a tree this parser did not
    // build. A cycle in one would otherwise take the process down.
    if (depth > kMaxDepth * 2) return QStringLiteral("/* expression too deep */");
    const int d = depth + 1;

    switch (e->kind) {
    case ExprKind::Literal:
    case ExprKind::Name:
        return e->text;
    case ExprKind::Member:
        return childText(e->target, d) + QLatin1Char('.') + e->text;
    case ExprKind::Call:
        return childText(e->target, d) + QLatin1Char('(') + argList(e->args, d) + QLatin1Char(')');
    case ExprKind::Index:
        return childText(e->target, d) + QLatin1Char('[') + childText(e->second, d)
               + QLatin1Char(']');
    case ExprKind::Unary: {
        if (e->op.startsWith(QLatin1String("post")))
            return childText(e->target, d) + e->op.mid(4);
        if (!e->op.isEmpty() && e->op.at(0).isLetter())
            return e->op + QLatin1Char(' ') + childText(e->target, d);
        return e->op + childText(e->target, d);
    }
    case ExprKind::Binary:
        if (e->op == QLatin1String(","))
            return childText(e->target, d) + QStringLiteral(", ") + childText(e->second, d);
        return childText(e->target, d) + QLatin1Char(' ') + e->op + QLatin1Char(' ')
               + childText(e->second, d);
    case ExprKind::Ternary:
        return childText(e->target, d) + QStringLiteral(" ? ") + childText(e->second, d)
               + QStringLiteral(" : ") + childText(e->third, d);
    case ExprKind::Assign:
        return childText(e->target, d) + QLatin1Char(' ') + e->op + QLatin1Char(' ')
               + childText(e->second, d);
    case ExprKind::New:
        if (e->typeName.isEmpty())
            return QLatin1Char('{') + argList(e->args, d) + QLatin1Char('}');
        return QStringLiteral("new ") + e->typeName
               + (e->op == QLatin1String("()")
                      ? QLatin1Char('(') + argList(e->args, d) + QLatin1Char(')')
                      : QString());
    case ExprKind::Cast:
        if (e->op == QLatin1String("CastTo"))
            return e->typeName + QStringLiteral(".CastTo(") + childText(e->second, d)
                   + QStringLiteral(", ") + childText(e->target, d) + QLatin1Char(')');
        if (e->op == QLatin1String("prefix"))
            return QLatin1Char('(') + e->typeName + QLatin1Char(')') + childText(e->target, d);
        return e->typeName + QStringLiteral(".Cast(") + childText(e->target, d) + QLatin1Char(')');
    case ExprKind::Paren:
        return QLatin1Char('(') + childText(e->target, d) + QLatin1Char(')');
    }
    return e->text;
}

QString blockText(const std::vector<StmtPtr> &body, int indent, int depth);

QString stmtText(const Stmt *s, int indent, int depth);

// `{`, the statements, `}`, each on its own line at this indent. Always braced,
// so printing is stable: an unbraced body read back in produces the same tree.
QString blockText(const std::vector<StmtPtr> &body, int indent, int depth)
{
    const QString pad = indentOf(indent);
    QStringList out;
    out << pad + QLatin1Char('{');
    for (const StmtPtr &st : body) out << stmtText(st.get(), indent + 1, depth + 1);
    out << pad + QLatin1Char('}');
    return out.join(QLatin1Char('\n'));
}

// A statement rendered on one line with no trailing `;`, for a for-loop header.
QString headerText(const Stmt *s, int depth)
{
    QString t = stmtText(s, 0, depth).replace(QLatin1Char('\n'), QLatin1Char(' '));
    while (t.endsWith(QLatin1Char(';'))) t.chop(1);
    return t;
}

QString stmtText(const Stmt *s, int indent, int depth)
{
    if (!s) return QString();
    if (depth > kMaxDepth * 2) return indentOf(indent) + QStringLiteral("// statement too deep");
    const QString pad = indentOf(indent);
    const int d = depth + 1;

    switch (s->kind) {
    case StmtKind::Expression:
        return pad + exprText(s->expr.get(), 0) + QLatin1Char(';');

    case StmtKind::VarDecl: {
        QStringList parts;
        for (const Declarator &decl : s->decls) {
            QString one = decl.name;
            if (decl.init) one += QStringLiteral(" = ") + exprText(decl.init.get(), 0);
            parts << one;
        }
        return pad + s->typeName + QLatin1Char(' ') + parts.join(QStringLiteral(", "))
               + QLatin1Char(';');
    }

    case StmtKind::If: {
        QStringList out;
        out << pad + QStringLiteral("if (") + exprText(s->expr.get(), 0) + QLatin1Char(')');
        out << blockText(s->body, indent, d);
        if (!s->elseBody.empty()) {
            // An empty else is held as an empty Block, so print it as the
            // empty braces it came from rather than a block inside a block.
            const Stmt *only = s->elseBody.size() == 1 ? s->elseBody.front().get() : nullptr;
            if (only && only->kind == StmtKind::Block && only->body.empty()) {
                out << pad + QStringLiteral("else");
                out << blockText(std::vector<StmtPtr>(), indent, d);
                return out.join(QLatin1Char('\n'));
            }
            // `else if` keeps its shape: braces here would nest one level per
            // branch and re-parse into a different tree.
            if (s->elseBody.size() == 1 && s->elseBody.front()
                && s->elseBody.front()->kind == StmtKind::If) {
                const QString inner = stmtText(s->elseBody.front().get(), indent, d);
                out << pad + QStringLiteral("else ") + inner.mid(pad.size());
            } else {
                out << pad + QStringLiteral("else");
                out << blockText(s->elseBody, indent, d);
            }
        }
        return out.join(QLatin1Char('\n'));
    }

    case StmtKind::For: {
        // Each part is optional, and `for (;;)` must not print as `for (; ; )`.
        const QString init = s->forInit ? headerText(s->forInit.get(), d) : QString();
        const QString cond = exprText(s->forCond.get(), 0);
        const QString step = exprText(s->forStep.get(), 0);
        QString head = QStringLiteral("for (") + init + QLatin1Char(';');
        if (!cond.isEmpty()) head += QLatin1Char(' ') + cond;
        head += QLatin1Char(';');
        if (!step.isEmpty()) head += QLatin1Char(' ') + step;
        head += QLatin1Char(')');
        QStringList out;
        out << pad + head;
        out << blockText(s->body, indent, d);
        return out.join(QLatin1Char('\n'));
    }

    case StmtKind::ForEach: {
        QString head = QStringLiteral("foreach (");
        if (!s->eachIndexName.isEmpty()) {
            const QString it =
                s->eachIndexType.isEmpty() ? QStringLiteral("int") : s->eachIndexType;
            head += it + QLatin1Char(' ') + s->eachIndexName + QStringLiteral(", ");
        }
        head += s->typeName + QLatin1Char(' ') + s->eachValueName + QStringLiteral(" : ")
                + exprText(s->eachCollection.get(), 0) + QLatin1Char(')');
        QStringList out;
        out << pad + head;
        out << blockText(s->body, indent, d);
        return out.join(QLatin1Char('\n'));
    }

    case StmtKind::While: {
        QStringList out;
        out << pad + QStringLiteral("while (") + exprText(s->expr.get(), 0) + QLatin1Char(')');
        out << blockText(s->body, indent, d);
        return out.join(QLatin1Char('\n'));
    }

    case StmtKind::Return:
        return s->expr ? pad + QStringLiteral("return ") + exprText(s->expr.get(), 0)
                             + QLatin1Char(';')
                       : pad + QStringLiteral("return;");

    case StmtKind::Break:
        return pad + QStringLiteral("break;");

    case StmtKind::Continue:
        return pad + QStringLiteral("continue;");

    case StmtKind::Block:
        return blockText(s->body, indent, d);

    case StmtKind::Switch: {
        QStringList out;
        out << pad + QStringLiteral("switch (") + exprText(s->expr.get(), 0) + QLatin1Char(')');
        out << pad + QLatin1Char('{');
        for (const SwitchCase &c : s->cases) {
            out << indentOf(indent + 1)
                       + (c.value ? QStringLiteral("case ") + exprText(c.value.get(), 0)
                                        + QLatin1Char(':')
                                  : QStringLiteral("default:"));
            for (const StmtPtr &b : c.body) out << stmtText(b.get(), indent + 2, d);
        }
        out << pad + QLatin1Char('}');
        return out.join(QLatin1Char('\n'));
    }

    case StmtKind::Delete:
        return pad + QStringLiteral("delete ") + exprText(s->expr.get(), 0) + QLatin1Char(';');

    case StmtKind::Raw: {
        QStringList out;
        for (const QString &l : s->text.split(QLatin1Char('\n'))) out << pad + l;
        return out.join(QLatin1Char('\n'));
    }
    }
    return pad + s->text;
}

} // namespace

ParseResult parseEnforceBody(const QString &code)
{
    Parser p(code);
    return p.run();
}

QString exprToText(const Expr &e) { return exprText(&e, 0); }

QString stmtToText(const Stmt &s, int indent) { return stmtText(&s, indent, 0); }
