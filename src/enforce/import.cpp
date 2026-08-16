// Enforce Script file to graph.
//
// Only the declaration layer is read here: the class header, its members and
// each method signature. Everything below a signature goes to parseEnforceBody
// and lowerToNodes, which already do that job.
//
// What decides whether a body arrives as nodes or as text is not a guess about
// how hard the code looks. Both shapes are put through the generator, and the
// nodes are kept only when the file they produce is the same file the text
// produces, character for character. That is what makes opening a script and
// saving it again leave the file alone.
#include "import.h"

#include "ast.h"
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "layout.h"
#include "lower.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace {

const QChar Space = QLatin1Char(' ');
const QChar Newline = QLatin1Char('\n');

// The generator writes its preserved region from the previous file, so anything
// inside it must not become a member or a method here or it would come back
// twice. Both spellings of the opening marker start with this.
const QString kUserBeginPrefix = QStringLiteral("// >>> user code");

// Words that may sit in front of a member. Only some of them reach the model;
// the rest are read so the name after them is found in the right place.
const QStringList &memberModifiers()
{
    static const QStringList words = {
        QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("static"),
        QStringLiteral("override"), QStringLiteral("proto"),    QStringLiteral("native"),
        QStringLiteral("const"),    QStringLiteral("ref"),      QStringLiteral("autoptr"),
        QStringLiteral("sealed"),   QStringLiteral("owned"),    QStringLiteral("external"),
        QStringLiteral("volatile"), QStringLiteral("event"),
    };
    return words;
}

// The same, in front of a method. `ref` and its friends are missing on purpose:
// in `override ref array<string> GetPaths()` the word belongs to the return
// type, and reading it as a modifier of the method drops it.
const QStringList &methodModifiers()
{
    static const QStringList words = {
        QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("static"),
        QStringLiteral("override"), QStringLiteral("proto"),    QStringLiteral("native"),
        QStringLiteral("sealed"),   QStringLiteral("external"), QStringLiteral("event"),
    };
    return words;
}

// ---------------------------------------------------------------- source prep

// The source with comments, string contents and preprocessor lines blanked out,
// every offset left where it was. Scanning happens on this copy and slicing on
// the original, so a brace inside a comment or a string cannot end a class and
// the text that comes back is still exactly what the author wrote.
QString maskedSource(const QString &src)
{
    QString out = src;
    const int n = src.size();
    const auto blank = [&out, n](int from, int to) {
        for (int i = qMax(0, from); i < qMin(n, to); ++i)
            if (out.at(i) != Newline) out[i] = Space;
    };

    int i = 0;
    bool freshLine = true; // nothing but whitespace so far on this line
    while (i < n) {
        const QChar c = src.at(i);
        if (c == Newline) {
            freshLine = true;
            i++;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && src.at(i + 1) == QLatin1Char('/')) {
            int j = i;
            while (j < n && src.at(j) != Newline) j++;
            blank(i, j);
            i = j;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && src.at(i + 1) == QLatin1Char('*')) {
            int j = i + 2;
            while (j + 1 < n && !(src.at(j) == QLatin1Char('*') && src.at(j + 1) == QLatin1Char('/')))
                j++;
            j = qMin(n, j + 2);
            blank(i, j);
            i = j;
            continue;
        }
        if (c == QLatin1Char('"')) {
            int j = i + 1;
            while (j < n && src.at(j) != QLatin1Char('"')) {
                if (src.at(j) == QLatin1Char('\\')) j++;
                j++;
            }
            // The quotes stay so the scan still sees a value; the inside goes,
            // because a brace or a semicolon in a string is not punctuation.
            blank(i + 1, j);
            i = qMin(n, j + 1);
            freshLine = false;
            continue;
        }
        // A directive only counts at the start of a line, and #define can carry
        // braces that would otherwise close a class early.
        if (c == QLatin1Char('#') && freshLine) {
            int j = i;
            while (j < n && src.at(j) != Newline) j++;
            blank(i, j);
            i = j;
            continue;
        }
        if (!c.isSpace()) freshLine = false;
        i++;
    }
    return out;
}

// Blanks the generator's preserved region, whole lines included.
void maskUserRegion(const QString &src, QString *masked)
{
    const int begin = src.indexOf(kUserBeginPrefix);
    if (begin < 0) return;
    const int end = src.indexOf(USER_END, begin);
    if (end < 0) return;
    int from = src.lastIndexOf(Newline, begin);
    from = from < 0 ? 0 : from + 1;
    int to = src.indexOf(Newline, end);
    if (to < 0) to = src.size();
    for (int i = from; i < to; ++i)
        if (masked->at(i) != Newline) (*masked)[i] = Space;
}

// Index of the brace matching the one at `open`, or -1.
int matchBrace(const QString &masked, int open)
{
    int depth = 0;
    for (int i = open; i < masked.size(); ++i) {
        const QChar c = masked.at(i);
        if (c == QLatin1Char('{')) depth++;
        else if (c == QLatin1Char('}') && --depth == 0) return i;
    }
    return -1;
}

int lineOf(const QString &src, int offset)
{
    int line = 1;
    for (int i = 0; i < offset && i < src.size(); ++i)
        if (src.at(i) == Newline) line++;
    return line;
}

// Leading modifier words, removed from `rest`.
QStringList takeMods(QString *rest, const QStringList &words)
{
    static const QRegularExpression word(QStringLiteral("^([A-Za-z_]\\w*)\\s+"));
    QStringList found;
    *rest = rest->trimmed();
    for (;;) {
        const auto m = word.match(*rest);
        if (!m.hasMatch() || !words.contains(m.captured(1))) break;
        found << m.captured(1);
        *rest = rest->mid(m.capturedLength(0)).trimmed();
    }
    return found;
}

// Whitespace runs collapsed, for comparing a signature against the catalogue.
QString normalisedType(const QString &t)
{
    static const QRegularExpression runs(QStringLiteral("\\s+"));
    return t.trimmed().replace(runs, QStringLiteral(" "));
}

// ------------------------------------------------------------- class scanning

struct ClassSpan {
    bool modded = false;
    QString name;
    QString base;
    int headStart = 0;  // first character of `class` or `modded`
    int bodyOpen = 0;   // the `{`
    int bodyClose = 0;  // the matching `}`
    int end = 0;        // one past the declaration, trailing `;` included
};

QVector<ClassSpan> findClasses(const QString &masked)
{
    // `class X`, `class X extends Y`, `class X : Y`, and `modded` in front of
    // any of them. A forward declaration has no brace and is skipped by the
    // trailing `{`.
    static const QRegularExpression header(
        QStringLiteral("(?:^|[\\s;}])(modded\\s+)?class\\s+([A-Za-z_]\\w*)\\s*"
                       "(?::\\s*([A-Za-z_]\\w*)|extends\\s+([A-Za-z_]\\w*))?\\s*\\{"));

    QVector<ClassSpan> out;
    int from = 0;
    while (from < masked.size()) {
        const auto m = header.match(masked, from);
        if (!m.hasMatch()) break;
        const int open = m.capturedEnd(0) - 1;
        const int close = matchBrace(masked, open);
        if (close < 0) break;

        ClassSpan c;
        c.modded = !m.captured(1).isEmpty();
        c.name = m.captured(2);
        c.base = m.captured(3).isEmpty() ? m.captured(4) : m.captured(3);
        // capturedStart(0) may sit on the separator the pattern needed in front
        // of the keyword, so the header starts at the first word it captured.
        c.headStart = c.modded ? m.capturedStart(1) : m.capturedStart(0);
        while (c.headStart < masked.size() && masked.at(c.headStart).isSpace()) c.headStart++;
        c.bodyOpen = open;
        c.bodyClose = close;
        c.end = close + 1;
        // Enforce writes `};` as often as `}`; both belong to the declaration.
        int after = c.end;
        while (after < masked.size() && masked.at(after).isSpace()
               && masked.at(after) != Newline)
            after++;
        if (after < masked.size() && masked.at(after) == QLatin1Char(';')) c.end = after + 1;

        out.append(c);
        from = c.end;
    }
    return out;
}

// ---------------------------------------------------------- class body layout

struct MemberSpan {
    int start = 0;
    int end = 0; // one past the last character before the `;`
};

struct MethodSpan {
    int headStart = 0;
    int headEnd = 0;   // one past the `)`
    int bodyOpen = 0;
    int bodyClose = 0;
};

// True when an `=` sits outside every bracket. That is what tells a member with
// an initialiser from a method whose parameters carry defaults: `int m_Sizes[]
// = {1, 2};` has one and `void Step(EStage stage = EStage.Main)` does not, and
// reading the second as the first swallows the whole method.
bool hasTopLevelAssign(const QString &head)
{
    int depth = 0;
    for (const QChar c : head) {
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) depth++;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']')) depth--;
        else if (c == QLatin1Char('=') && depth <= 0) return true;
    }
    return false;
}

// Splits a class body into declarations that end at a semicolon and methods
// that carry a brace block.
void splitBody(const QString &masked, int from, int to, QVector<MemberSpan> *members,
               QVector<MethodSpan> *methods, QStringList *skipped)
{
    int i = from;
    int stmtStart = from;
    int nesting = 0; // parentheses and brackets

    const auto headIsEmpty = [&masked](int a, int b) {
        for (int k = a; k < b; ++k)
            if (!masked.at(k).isSpace()) return false;
        return true;
    };

    while (i < to) {
        const QChar c = masked.at(i);
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) {
            nesting++;
            i++;
            continue;
        }
        if (c == QLatin1Char(')') || c == QLatin1Char(']')) {
            nesting--;
            i++;
            continue;
        }
        // The semicolons in `for (a; b; c)` belong to the header, not to a
        // declaration, and the same goes for a default value in a parameter.
        if (c == QLatin1Char(';') && nesting > 0) {
            i++;
            continue;
        }
        if (c == QLatin1Char(';')) {
            if (!headIsEmpty(stmtStart, i)) members->append({stmtStart, i});
            i++;
            stmtStart = i;
            continue;
        }
        // A brace inside brackets belongs to whatever opened them: a Workbench
        // annotation carries a list in one, and reading that as a method body
        // takes the rest of the class with it.
        if (c == QLatin1Char('{') && nesting > 0) {
            const int close = matchBrace(masked, i);
            if (close < 0 || close >= to) break;
            i = close + 1;
            continue;
        }
        if (c == QLatin1Char('{')) {
            const int close = matchBrace(masked, i);
            if (close < 0 || close >= to) break;
            const QString head = masked.mid(stmtStart, i - stmtStart).trimmed();
            // `int m_Sizes[] = {1, 2};` is one declaration with a brace in it,
            // so the scan carries on to the semicolon that really ends it.
            if (hasTopLevelAssign(head)) {
                i = close + 1;
                continue;
            }
            if (head.isEmpty()) {
                i = close + 1;
                stmtStart = i;
                continue;
            }
            if (!head.contains(QLatin1Char('('))) {
                // A nested enum or a struct: no signature, so there is no
                // function to build out of it.
                skipped->append(head);
                i = close + 1;
                stmtStart = i;
                continue;
            }
            MethodSpan m;
            m.headStart = stmtStart;
            while (m.headStart < i && masked.at(m.headStart).isSpace()) m.headStart++;
            m.headEnd = i;
            m.bodyOpen = i;
            m.bodyClose = close;
            methods->append(m);
            i = close + 1;
            // A method body may be followed by `;`, which is not a declaration.
            int after = i;
            while (after < to && masked.at(after).isSpace()) after++;
            if (after < to && masked.at(after) == QLatin1Char(';')) i = after + 1;
            stmtStart = i;
            continue;
        }
        i++;
    }
}

// ------------------------------------------------------------------- members

// True when the generator would put `ref` in front of a member of this type on
// its own. Mirrors isManaged in codegen.cpp, which is what has to be predicted.
bool refIsInferred(const Catalog &cat, const QString &type)
{
    if (cat.classId(type) < 0) return false;
    return cat.isA(type, QStringLiteral("Managed")) && !cat.isA(type, QStringLiteral("Object"));
}

// Splits `type name` at the last space outside <>, so `array<ref ItemBase> m_x`
// keeps its type in one piece.
bool splitTypeAndName(const QString &head, QString *type, QString *name)
{
    int depth = 0;
    int cut = -1;
    for (int i = 0; i < head.size(); ++i) {
        const QChar c = head.at(i);
        if (c == QLatin1Char('<')) depth++;
        else if (c == QLatin1Char('>')) depth--;
        else if (c.isSpace() && depth == 0) cut = i;
    }
    if (cut < 0) return false;
    *type = head.left(cut).trimmed();
    *name = head.mid(cut + 1).trimmed();
    return !type->isEmpty() && !name->isEmpty();
}

bool isPlainName(const QString &name)
{
    if (name.isEmpty()) return false;
    if (!name.at(0).isLetter() && name.at(0) != QLatin1Char('_')) return false;
    for (const QChar c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) return false;
    return true;
}

// A declared name, with a fixed array size kept on it. `string m_Names[4]` has
// to regenerate with its brackets, and the model has one field to hold them.
bool isDeclaredName(const QString &name)
{
    const int bracket = name.indexOf(QLatin1Char('['));
    if (bracket < 0) return isPlainName(name);
    return name.endsWith(QLatin1Char(']')) && isPlainName(name.left(bracket));
}

// Reads one member declaration. Returns the number of variables it produced,
// because `int a, b;` declares two.
int readMembers(const QString &src, const QString &masked, const MemberSpan &span,
                const Catalog &cat, QVector<GraphVariable> *out, QStringList *notes)
{
    int from = span.start;
    while (from < span.end && masked.at(from).isSpace()) from++;

    // A Workbench annotation sits in front of the member it decorates. It only
    // reaches the editor, never the running game, so the member is worth more
    // than the annotation is: keeping one and losing the other beats losing a
    // field the rest of the class refers to.
    if (from < span.end && masked.at(from) == QLatin1Char('[')) {
        const int close = masked.indexOf(QLatin1Char(']'), from);
        if (close > 0 && close < span.end) {
            from = close + 1;
            notes->append(QStringLiteral("Line %1: the annotation on `%2` is not carried back "
                                         "out.")
                              .arg(lineOf(src, span.start))
                              .arg(masked.mid(from, span.end - from).simplified()));
        }
    }

    QString rest = masked.mid(from, span.end - from);
    const QStringList mods = takeMods(&rest, memberModifiers());

    // A prototype has a signature and no body; there is nowhere in the model to
    // keep one, and inventing an empty method would change what the class does.
    if (rest.contains(QLatin1Char('('))) {
        notes->append(QStringLiteral("Line %1: `%2` declares a method with no body, which this "
                                     "tool has no place for.")
                          .arg(lineOf(src, span.start))
                          .arg(rest.simplified()));
        return 0;
    }

    // Everything after the first `=` is the initialiser, taken from the source
    // rather than the masked copy so a string keeps its contents.
    const int eq = rest.indexOf(QLatin1Char('='));
    const QString head = (eq >= 0 ? rest.left(eq) : rest).trimmed();
    QString value;
    if (eq >= 0) {
        const int eqInSource = masked.indexOf(QLatin1Char('='), from);
        if (eqInSource >= 0 && eqInSource < span.end)
            value = src.mid(eqInSource + 1, span.end - eqInSource - 1).trimmed();
    }

    QString type;
    QString names;
    if (!splitTypeAndName(head, &type, &names)) {
        notes->append(QStringLiteral("Line %1: `%2` is not a declaration this tool can read.")
                          .arg(lineOf(src, span.start))
                          .arg(head.simplified()));
        return 0;
    }

    // `autoptr` is the older spelling of the same strong reference, and the one
    // flag the model carries is written back as `ref`. Keeping the word on the
    // type is what brings the declaration back as the author wrote it, and
    // codegen strips storage words from a type before every lookup anyway.
    if (mods.contains(QStringLiteral("autoptr")) && !mods.contains(QStringLiteral("ref")))
        type = QStringLiteral("autoptr ") + type;

    // `int a, b;` is two members. The generator writes one per line, so the
    // file gains a line where the author had one; the alternative is losing b.
    QStringList declared;
    int depth = 0;
    QString current;
    for (const QChar c : names) {
        if (c == QLatin1Char('<')) depth++;
        else if (c == QLatin1Char('>')) depth--;
        if (c == QLatin1Char(',') && depth == 0) {
            declared << current.trimmed();
            current.clear();
            continue;
        }
        current.append(c);
    }
    declared << current.trimmed();

    int made = 0;
    for (const QString &name : declared) {
        if (!isDeclaredName(name)) {
            notes->append(QStringLiteral("Line %1: `%2 %3` is not a declaration this tool can "
                                         "read.")
                              .arg(lineOf(src, span.start))
                              .arg(type, name));
            continue;
        }
        GraphVariable v;
        v.id = QStringLiteral("v_%1").arg(name);
        v.name = name;
        v.type = type;
        v.def = declared.size() == 1 ? value : QString();
        v.isStatic = mods.contains(QStringLiteral("static"));
        v.isConst = mods.contains(QStringLiteral("const"));
        v.isPrivate = mods.contains(QStringLiteral("private"));
        v.isProtected = mods.contains(QStringLiteral("protected"));
        v.isRef = mods.contains(QStringLiteral("ref"));
        // `ref` is tri-state. The word being there is a decision; the word not
        // being there on a type the generator would infer `ref` for is also a
        // decision, and only an explicit false keeps it.
        v.hasRef = v.isRef || refIsInferred(cat, type);
        out->append(v);
        made++;
    }
    return made;
}

// ------------------------------------------------------------------- methods

struct ParsedMethod {
    QString name;
    QString ret;
    QVector<GraphParam> params;
    QStringList localNames; // parameter names alone, for the lowering's scope
    bool isStatic = false;
    bool isPrivate = false;
    bool isProtected = false;
    bool isOverride = false;
    bool isCtor = false;
    bool isDtor = false;
    bool valid = false;
};

// `masked` and `plain` are the same signature, one with string contents blanked
// for scanning and one as the author wrote it. Punctuation is found in the
// first and text is taken from the second, so a default value like
// `vector offset = "0 0 0"` keeps what is inside its quotes.
ParsedMethod parseMethodHead(const QString &masked, const QString &plain,
                             const QString &className)
{
    ParsedMethod out;

    int at = 0;
    QStringList mods;
    while (at < masked.size() && masked.at(at).isSpace()) at++;
    for (;;) {
        int end = at;
        while (end < masked.size()
               && (masked.at(end).isLetterOrNumber() || masked.at(end) == QLatin1Char('_')))
            end++;
        const QString word = masked.mid(at, end - at);
        if (end == at || !methodModifiers().contains(word)) break;
        mods << word;
        at = end;
        while (at < masked.size() && masked.at(at).isSpace()) at++;
    }

    const int lp = masked.indexOf(QLatin1Char('('), at);
    if (lp < 0) return out;
    int depth = 0;
    int rp = -1;
    for (int i = lp; i < masked.size(); ++i) {
        if (masked.at(i) == QLatin1Char('(')) depth++;
        else if (masked.at(i) == QLatin1Char(')') && --depth == 0) {
            rp = i;
            break;
        }
    }
    if (rp < 0) return out;

    const QString prefix = masked.mid(at, lp - at).trimmed();
    QString ret;
    QString name;
    if (!splitTypeAndName(prefix, &ret, &name)) {
        // No space in front of the name: a constructor or a destructor.
        ret.clear();
        name = prefix;
    }
    if (!isPlainName(name.startsWith(QLatin1Char('~')) ? name.mid(1) : name)) return out;

    out.name = name;
    out.ret = ret;
    out.isDtor = name.startsWith(QLatin1Char('~'));
    out.isCtor = !out.isDtor && name == className;
    // A constructor and a destructor have no return type. The model still needs
    // one, and the generator writes `void` for an empty string either way.
    if (out.ret.isEmpty()) out.ret = QStringLiteral("void");

    // One pass over the parameter list, splitting at the commas that are not
    // inside a generic type or a nested call.
    int pieceStart = lp + 1;
    depth = 0;
    for (int i = lp + 1; i <= rp; ++i) {
        const QChar c = masked.at(i);
        if (c == QLatin1Char('<') || c == QLatin1Char('(') || c == QLatin1Char('[')) depth++;
        else if (c == QLatin1Char('>') || c == QLatin1Char(']')) depth--;
        else if (c == QLatin1Char(')')) {
            if (i < rp) depth--;
        }
        const bool boundary = (c == QLatin1Char(',') && depth == 0) || i == rp;
        if (!boundary) continue;

        const int pieceEnd = i;
        if (masked.mid(pieceStart, pieceEnd - pieceStart).trimmed().isEmpty()) {
            pieceStart = i + 1;
            continue;
        }
        int eq = -1;
        int inner = 0;
        for (int k = pieceStart; k < pieceEnd; ++k) {
            const QChar d = masked.at(k);
            if (d == QLatin1Char('(') || d == QLatin1Char('[')) inner++;
            else if (d == QLatin1Char(')') || d == QLatin1Char(']')) inner--;
            else if (d == QLatin1Char('=') && inner == 0) {
                eq = k;
                break;
            }
        }
        const int declEnd = eq >= 0 ? eq : pieceEnd;
        const QString decl = masked.mid(pieceStart, declEnd - pieceStart).trimmed();
        const QString def =
            eq >= 0 ? plain.mid(eq + 1, pieceEnd - eq - 1).trimmed() : QString();

        QString type;
        QString pname;
        if (!splitTypeAndName(decl, &type, &pname)) {
            // A parameter written as a bare type has no name to read. The type
            // is what the call has to keep, so it is the part kept.
            type = decl;
            pname.clear();
        }
        out.localNames << pname;
        // A default value has nowhere of its own to live, and dropping it
        // changes every call that relied on it, so it rides with the name.
        if (!def.isEmpty()) pname += QStringLiteral(" = ") + def;
        // `out`, `inout` and `notnull` change what the call does, so they stay
        // on the type rather than being read off and dropped.
        out.params.append({pname, type});
        pieceStart = i + 1;
    }

    out.isStatic = mods.contains(QStringLiteral("static"));
    out.isPrivate = mods.contains(QStringLiteral("private"));
    out.isProtected = mods.contains(QStringLiteral("protected"));
    out.isOverride = mods.contains(QStringLiteral("override"));
    out.valid = true;
    return out;
}

// The body between the braces, ready to hand to the generator as it stands.
// The generator writes the opening brace and the closing brace itself, so the
// newline after the first and the indent before the last are not part of it.
QString bodyText(const QString &src, int open, int close)
{
    QString body = src.mid(open + 1, close - open - 1);
    const int firstBreak = body.indexOf(Newline);
    if (firstBreak >= 0 && body.left(firstBreak).trimmed().isEmpty())
        body = body.mid(firstBreak + 1);
    // Only the indent in front of the closing brace goes. A blank line the
    // author left at the end of the body is part of the body and comes back.
    const int lastBreak = body.lastIndexOf(Newline);
    if (lastBreak >= 0 && body.mid(lastBreak + 1).trimmed().isEmpty())
        body = body.left(lastBreak);
    else if (lastBreak < 0 && body.trimmed().isEmpty())
        body.clear();
    return body;
}

// ------------------------------------------------------------ body formatting

// The indentation a body was written with. `base` is what stands in front of a
// statement at the top of it and `unit` is what one more level adds. The
// generator writes two tabs and one tab; every body written any other way comes
// back reformatted unless the graph carries these two strings.
struct BodyFormat {
    QString base = QStringLiteral("\t\t");
    QString unit = QStringLiteral("\t");
};

// The whitespace between the start of a line and `at`. False when something
// other than whitespace sits in front of it, which is a statement sharing a
// line with the one before it.
bool leadBefore(const QString &text, int at, QString *lead)
{
    if (at < 0 || at > text.size()) return false;
    int from = at > 0 ? text.lastIndexOf(Newline, at - 1) : -1;
    from = from < 0 ? 0 : from + 1;
    *lead = text.mid(from, at - from);
    for (const QChar c : *lead)
        if (c != Space && c != QLatin1Char('\t')) return false;
    return true;
}

BodyFormat readBodyFormat(const QString &text, const std::vector<StmtPtr> &stmts)
{
    BodyFormat f;
    // The first statement of the body sits at the top level, so what stands in
    // front of it is the base every deeper level is measured from.
    bool haveBase = false;
    for (const StmtPtr &s : stmts) {
        QString lead;
        if (!s || s->srcStart < 0 || !leadBefore(text, s->srcStart, &lead)) continue;
        f.base = lead;
        haveBase = true;
        break;
    }
    if (!haveBase) return f;

    // One level in. Most real bodies never show a second level at all, in which
    // case the unit is not observable and the default stands: it is only ever
    // written out again if the user nests something new.
    QString unit;
    const auto consider = [&](const Stmt *inner) {
        QString lead;
        if (!inner || inner->srcStart < 0 || !leadBefore(text, inner->srcStart, &lead)) return;
        if (!lead.startsWith(f.base) || lead.size() <= f.base.size()) return;
        const QString step = lead.mid(f.base.size());
        if (unit.isEmpty() || step.size() < unit.size()) unit = step;
    };
    for (const StmtPtr &s : stmts) {
        if (!s) continue;
        for (const StmtPtr &inner : s->body) consider(inner.get());
        for (const StmtPtr &inner : s->elseBody) consider(inner.get());
    }
    if (!unit.isEmpty()) f.unit = unit;
    return f;
}

// Writes the format onto the node that owns the method, and only where it
// differs from what the generator would write anyway, so a project the user
// authored keeps no keys it has no use for.
void applyBodyFormat(GraphNode *n, const BodyFormat &f, const QString &endTrivia)
{
    if (!n) return;
    if (f.base != QLatin1String("\t\t")) n->opts.insert(nodefmt::keyBase(), f.base);
    if (f.unit != QLatin1String("\t")) n->opts.insert(nodefmt::keyUnit(), f.unit);
    if (!endTrivia.isEmpty()) n->opts.insert(nodefmt::keyEnd(), endTrivia);
}

// The catalogue event this method overrides, or an empty key.
QString eventKeyFor(const Catalog &cat, const QString &selfClass, const QString &name)
{
    if (selfClass.isEmpty() || name.isEmpty() || cat.classId(selfClass) < 0) return {};
    SearchOptions opts;
    opts.limit = 40;
    opts.category = QStringLiteral("Events");
    opts.ofClass = selfClass;
    const QString title = QStringLiteral("Event ") + name;
    for (const SearchHit &h : cat.search(name, opts))
        if (h.title == title) return h.key;
    return {};
}

// An event node regenerates its signature from the catalogue rather than from
// the file, so the two have to already agree or the method would come back
// declared differently from how it was written.
bool signatureMatches(const MethodSig &sig, const ParsedMethod &fn)
{
    const QString ret = sig.ret.isEmpty() ? QStringLiteral("void") : sig.ret;
    if (normalisedType(ret) != normalisedType(fn.ret)) return false;
    if (sig.params.size() != fn.params.size()) return false;
    for (int i = 0; i < sig.params.size(); ++i) {
        if (normalisedType(sig.params.at(i).type) != normalisedType(fn.params.at(i).type))
            return false;
        if (sig.params.at(i).name != fn.params.at(i).name) return false;
    }
    return true;
}

// The line the generator writes in front of an event body when it calls super,
// and the line it writes after it. Both are matched against the file so a body
// that already opens with the super call does not end up with two of them.
void superFraming(const MethodSig &sig, QString *head, QString *tail)
{
    QStringList names;
    for (const MethodSig::Param &p : sig.params) names << p.name;
    const QString invoke = QStringLiteral("super.") + sig.name + QLatin1Char('(')
                           + names.join(QStringLiteral(", ")) + QLatin1Char(')');
    const QString ret = sig.ret.isEmpty() ? QStringLiteral("void") : sig.ret;
    if (ret == QLatin1String("void")) {
        *head = invoke + QLatin1Char(';');
        tail->clear();
        return;
    }
    if (sig.name == QLatin1String("OnStoreLoad") && ret == QLatin1String("bool")) {
        *head = QStringLiteral("if (!") + invoke + QStringLiteral(") return false;");
        *tail = QStringLiteral("return true;");
        return;
    }
    *head = ret + QStringLiteral(" superRet = ") + invoke + QLatin1Char(';');
    *tail = QStringLiteral("return superRet;");
}

// ------------------------------------------------------------- building a plan

// One method, as either text or nodes. Both shapes go through the generator so
// the choice between them can be measured rather than guessed.
struct MethodPlan {
    QVector<GraphNode> nodes;
    QVector<GraphEdge> edges;
    QVector<GraphVariable> vars;
    GraphFunction fn;
    bool hasFunction = false;
    int lowered = 0;
    int statements = 0;
};

Graph scratchWith(const Graph &shell, const MethodPlan &plan)
{
    Graph g = shell;
    for (const GraphNode &n : plan.nodes) g.nodes.append(n);
    for (const GraphEdge &e : plan.edges) g.edges.append(e);
    for (const GraphVariable &v : plan.vars) g.variables.append(v);
    if (plan.hasFunction) g.functions.append(plan.fn);
    return g;
}

// Reads of a parameter come out of the lowering as a raw expression holding the
// name, because nothing else could say it while the entry node was unknown.
// Now that it is known, each one becomes a wire from the pin it arrives on.
void wireParameters(MethodPlan *plan, const QString &entryId, const QStringList &params)
{
    if (entryId.isEmpty() || params.isEmpty()) return;
    QHash<QString, QString> pinOf;
    for (int i = 0; i < params.size(); ++i)
        pinOf.insert(params.at(i), QStringLiteral("o%1").arg(i));

    for (int i = plan->nodes.size() - 1; i >= 0; --i) {
        const GraphNode &n = plan->nodes.at(i);
        if (n.ref != QLatin1String("bi.rawExpr")) continue;
        const QString pin = pinOf.value(n.opts.value(QStringLiteral("code")).trimmed());
        if (pin.isEmpty()) continue;
        const QString id = n.id;
        for (GraphEdge &e : plan->edges)
            if (e.from.node == id) e.from = {entryId, pin};
        plan->nodes.removeAt(i);
    }
    // An edge whose ends no longer both exist is not a wire.
    for (int i = plan->edges.size() - 1; i >= 0; --i) {
        const GraphEdge &e = plan->edges.at(i);
        bool fromOk = e.from.node == entryId;
        bool toOk = e.to.node == entryId;
        for (const GraphNode &n : plan->nodes) {
            if (n.id == e.from.node) fromOk = true;
            if (n.id == e.to.node) toOk = true;
        }
        if (!fromOk || !toOk) plan->edges.removeAt(i);
    }
}

struct LoweredBody {
    LowerResult result;
    bool usable = false;
};

// Lowers a body that has already been parsed. The parse is handed in rather
// than done here because the same statements are lowered more than once, as an
// event and as a plain function, and parsing a file four times over is most of
// what opening it costs.
LoweredBody lowerBody(const ParseResult &parsed, const QString &text, const Catalog &cat,
                      const Builtins &builtins, const Graph &shell, const Project &project,
                      const QString &selfClass, const QHash<QString, QString> &locals)
{
    LoweredBody out;
    // Unbalanced braces and the like: the text is the only thing that still
    // means what the author wrote, so nothing is lowered.
    if (!parsed.errors.isEmpty()) return out;
    LowerOptions opts;
    opts.selfClass = selfClass;
    opts.knownLocals = locals;
    // The statements carry offsets into this, which is how the blank lines and
    // the comments between them are read back.
    opts.sourceText = text;
    out.result = lowerToNodes(parsed.statements, cat, builtins, shell, project, opts);
    out.usable = !out.result.nodes.isEmpty() && !out.result.entryNode.isEmpty();
    return out;
}

} // namespace

// ------------------------------------------------------------------- results

int ImportResult::totalLowered() const
{
    int n = 0;
    for (const ImportedScript &s : scripts) n += s.statementsLowered;
    return n;
}

int ImportResult::totalStatements() const
{
    int n = 0;
    for (const ImportedScript &s : scripts) n += s.statementsTotal;
    return n;
}

namespace {

// Builds one class. `src` is the whole file and `masked` its scanning copy.
ImportedScript buildScript(const ClassSpan &cls, const QString &src, const QString &masked,
                           const Catalog &cat, const Builtins &builtins, const Project &project,
                           QStringList *notes)
{
    ImportedScript script;
    script.className = cls.name;
    // A modded class reopens an existing one and must not also extend it, so
    // the base a `modded class X : Y` might carry is dropped rather than
    // regenerated into something that does not compile.
    script.baseClass = cls.modded ? QString() : cls.base;
    script.modded = cls.modded;

    Graph &graph = script.graph;
    graph.className = script.className;
    graph.baseClass = script.baseClass;
    graph.modded = script.modded;

    QVector<MemberSpan> memberSpans;
    QVector<MethodSpan> methodSpans;
    QStringList skipped;
    splitBody(masked, cls.bodyOpen + 1, cls.bodyClose, &memberSpans, &methodSpans, &skipped);
    for (const QString &head : skipped)
        notes->append(QStringLiteral("`%1` is a declaration this tool has no place for, so it is "
                                     "not carried back out.")
                          .arg(head.simplified()));

    for (const MemberSpan &m : memberSpans)
        readMembers(src, masked, m, cat, &graph.variables, notes);

    const QString selfClass = cls.modded ? cls.name : cls.base;

    // The shell every candidate is measured against: the class as declared,
    // with its members and no methods at all. It grows as members are added,
    // so a later method sees the same class the finished file will declare.
    Graph shell = graph;

    // The generator writes the constructor first, then every event, then every
    // declared function. A method may only take one of the earlier shapes while
    // the file still reads in that order, or converting it would move it.
    bool ctorSlotOpen = true;
    bool eventSlotOpen = true;

    double originY = 0;
    for (int index = 0; index < methodSpans.size(); ++index) {
        const MethodSpan &span = methodSpans.at(index);
        const int headLen = span.headEnd - span.headStart;
        const QString head = masked.mid(span.headStart, headLen);
        const ParsedMethod fn =
            parseMethodHead(head, src.mid(span.headStart, headLen), cls.name);
        if (!fn.valid) {
            notes->append(QStringLiteral("Line %1: `%2` is not a signature this tool can read, so "
                                         "that method is not carried back out.")
                              .arg(lineOf(src, span.headStart))
                              .arg(head.simplified()));
            ctorSlotOpen = false;
            eventSlotOpen = false;
            continue;
        }

        const QString body = bodyText(src, span.bodyOpen, span.bodyClose);
        const ParseResult counted = parseEnforceBody(body);
        script.statementsTotal += counted.statementCount;

        // The text shape, which is exact by construction: the generator writes
        // a raw body out as it came in.
        MethodPlan asText;
        asText.hasFunction = true;
        asText.fn.id = QStringLiteral("f%1").arg(index);
        asText.fn.name = fn.name;
        asText.fn.returns = fn.ret;
        asText.fn.params = fn.params;
        asText.fn.isStatic = fn.isStatic;
        asText.fn.isPrivate = fn.isPrivate;
        asText.fn.isProtected = fn.isProtected;
        asText.fn.isOverride = fn.isOverride;
        asText.fn.rawBody = body;
        asText.fn.hasRawBody = true;
        asText.statements = counted.statementCount;

        const QString reference = generateEnforce(scratchWith(shell, asText), cat, builtins,
                                                  project)
                                      .code;

        QHash<QString, QString> locals;
        for (int i = 0; i < fn.localNames.size() && i < fn.params.size(); ++i)
            locals.insert(fn.localNames.at(i), fn.params.at(i).type);

        MethodPlan chosen = asText;
        bool tookNodes = false;

        // ---- the constructor, as the lifecycle node that owns that moment
        if (!tookNodes && fn.isCtor && ctorSlotOpen && fn.params.isEmpty()) {
            const LoweredBody low = lowerBody(counted, body, cat, builtins, shell, project,
                                              selfClass, locals);
            if (low.usable) {
                MethodPlan plan;
                GraphNode begin;
                begin.id = QStringLiteral("n_ctor%1").arg(index);
                begin.kind = NodeKind::Builtin;
                begin.ref = bi::Begin;
                begin.opts.insert(QStringLiteral("when"), QStringLiteral("construct"));
                applyBodyFormat(&begin, readBodyFormat(body, counted.statements),
                                low.result.endTrivia);
                plan.nodes.append(begin);
                for (const GraphNode &n : low.result.nodes) plan.nodes.append(n);
                for (const GraphEdge &e : low.result.edges) plan.edges.append(e);
                for (const GraphVariable &v : low.result.variables) plan.vars.append(v);
                plan.edges.append({QStringLiteral("e_ctor%1").arg(index),
                                   {begin.id, QStringLiteral("exec")},
                                   {low.result.entryNode, QStringLiteral("exec")},
                                   {}});
                plan.lowered = low.result.statementsLowered;
                plan.statements = counted.statementCount;
                if (generateEnforce(scratchWith(shell, plan), cat, builtins, project).code
                    == reference) {
                    chosen = plan;
                    tookNodes = true;
                }
            }
        }

        // ---- an overridden catalogue event, as an event node
        QString eventKey;
        if (!tookNodes && fn.isOverride && eventSlotOpen && !fn.isCtor && !fn.isDtor)
            eventKey = eventKeyFor(cat, selfClass, fn.name);
        if (!eventKey.isEmpty()) {
            const MethodSig sig = cat.method(eventKey);
            if (sig.valid && signatureMatches(sig, fn)) {
                QString superHead;
                QString superTail;
                superFraming(sig, &superHead, &superTail);

                QStringList lines = body.split(Newline);
                bool callsSuper = !lines.isEmpty() && lines.first().trimmed() == superHead;
                if (callsSuper && !superTail.isEmpty())
                    callsSuper = lines.size() >= 2 && lines.last().trimmed() == superTail;
                if (callsSuper) {
                    lines.removeFirst();
                    if (!superTail.isEmpty()) lines.removeLast();
                }
                // Only the super call was taken off the front, so the rest of
                // the body is the parse that is already in hand.
                const ParseResult stripped =
                    callsSuper ? parseEnforceBody(lines.join(Newline)) : ParseResult();
                const ParseResult &parsed = callsSuper ? stripped : counted;

                QHash<QString, QString> eventLocals;
                for (const MethodSig::Param &p : sig.params)
                    eventLocals.insert(p.name, p.type);
                const QString inner = callsSuper ? lines.join(Newline) : body;
                const LoweredBody low = lowerBody(parsed, inner, cat, builtins, shell, project,
                                                  selfClass, eventLocals);

                MethodPlan plan;
                GraphNode event;
                event.id = QStringLiteral("n_ev%1").arg(index);
                event.kind = NodeKind::Event;
                event.ref = eventKey;
                if (!callsSuper) event.opts.insert(QStringLiteral("noSuper"), QStringLiteral("1"));
                // The super call the node writes for itself sits at the same
                // indent as the rest of the body, so the format is read off the
                // text the chain was lowered from either way.
                applyBodyFormat(&event, readBodyFormat(inner, parsed.statements),
                                low.result.endTrivia);
                plan.nodes.append(event);
                if (low.usable) {
                    for (const GraphNode &n : low.result.nodes) plan.nodes.append(n);
                    for (const GraphEdge &e : low.result.edges) plan.edges.append(e);
                    for (const GraphVariable &v : low.result.variables) plan.vars.append(v);
                    plan.edges.append({QStringLiteral("e_ev%1").arg(index),
                                       {event.id, QStringLiteral("exec")},
                                       {low.result.entryNode, QStringLiteral("exec")},
                                       {}});
                    plan.lowered = low.result.statementsLowered;
                }
                QStringList paramNames;
                for (const MethodSig::Param &p : sig.params) paramNames << p.name;
                wireParameters(&plan, event.id, paramNames);
                plan.statements = counted.statementCount;

                if (generateEnforce(scratchWith(shell, plan), cat, builtins, project).code
                    == reference) {
                    chosen = plan;
                    tookNodes = true;
                }
            }
        }

        // ---- anything else, as a declared function with a body of nodes
        if (!tookNodes && !fn.isDtor) {
            const LoweredBody low = lowerBody(counted, body, cat, builtins, shell, project,
                                              selfClass, locals);
            if (low.usable) {
                MethodPlan plan = asText;
                plan.fn.rawBody.clear();
                plan.fn.hasRawBody = false;
                GraphNode entry;
                entry.id = QStringLiteral("n_fn%1").arg(index);
                entry.kind = NodeKind::Event;
                entry.ref = QStringLiteral("fn.entry.") + plan.fn.id;
                applyBodyFormat(&entry, readBodyFormat(body, counted.statements),
                                low.result.endTrivia);
                plan.nodes.append(entry);
                for (const GraphNode &n : low.result.nodes) plan.nodes.append(n);
                for (const GraphEdge &e : low.result.edges) plan.edges.append(e);
                for (const GraphVariable &v : low.result.variables) plan.vars.append(v);
                plan.edges.append({QStringLiteral("e_fn%1").arg(index),
                                   {entry.id, QStringLiteral("exec")},
                                   {low.result.entryNode, QStringLiteral("exec")},
                                   {}});
                wireParameters(&plan, entry.id, fn.localNames);
                plan.lowered = low.result.statementsLowered;
                plan.statements = counted.statementCount;

                if (generateEnforce(scratchWith(shell, plan), cat, builtins, project).code
                    == reference) {
                    chosen = plan;
                    tookNodes = true;
                }
            }
        }

        const bool isEventNode = tookNodes && !chosen.hasFunction;
        // The constructor slot only ever belonged to the first method, and the
        // event slot closes as soon as one method has to keep its text: the
        // generator writes the constructor, then the events, then the declared
        // functions, so a node shape taken after a text one would move up the
        // file past it.
        ctorSlotOpen = false;
        if (!isEventNode) eventSlotOpen = false;

        // Each method is laid out on its own before it joins the class, and
        // then dropped into a band of its own below the last one. Laying it out
        // in place instead would measure every node it moves against every node
        // already on the canvas, once per method, which is the square of the
        // size of the class: on real vanilla files that was most of the time
        // the whole import took.
        if (!chosen.nodes.isEmpty()) {
            Graph band;
            band.nodes = chosen.nodes;
            band.edges = chosen.edges;
            QSet<QString> ids;
            for (const GraphNode &n : chosen.nodes) ids.insert(n.id);
            layoutNodes(band, ids);

            double top = 0;
            for (const GraphNode &n : band.nodes) top = qMin(top, n.y);
            double lowest = originY;
            for (GraphNode n : band.nodes) {
                n.y += originY - top;
                lowest = qMax(lowest, n.y);
                graph.nodes.append(n);
            }
            originY = lowest + 220;
        }
        for (const GraphEdge &e : chosen.edges) graph.edges.append(e);
        for (const GraphVariable &v : chosen.vars) {
            graph.variables.append(v);
            shell.variables.append(v);
        }
        if (chosen.hasFunction) graph.functions.append(chosen.fn);
        script.statementsLowered += chosen.lowered;
    }

    return script;
}

} // namespace

ImportResult importEnforceText(const QString &text, const Catalog &cat, const Builtins &builtins,
                               const Project &project)
{
    ImportResult result;

    QString src = text;
    // Line endings are normalised once, before anything records an offset, so
    // every span found below still slices the string it was found in.
    //
    // The pair, never the character. A carriage return standing on its own ends
    // no line in Enforce source: inside a string literal or a comment it is a
    // byte the compiler reads, and taking it out changes what `Print("a\rb")`
    // compiles to. What the file used is recorded below and put back on the
    // finished text, so nothing between here and there has to carry an ending.
    src.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    // Read before anything can turn the file down. A file holding no class is
    // still a file somebody may write back, and the header says this field has
    // an answer for one, so it cannot be filled in past the early return below.
    result.eol = nodefmt::fileEol(text);

    QString masked = maskedSource(src);
    maskUserRegion(src, &masked);

    const QVector<ClassSpan> classes = findClasses(masked);

    // Everything between the classes: an enum, a global function, a #define.
    // The generator has no room for it, so it is handed back to the caller
    // rather than quietly dropped.
    QStringList outside;
    int at = 0;
    for (const ClassSpan &c : classes) {
        const QString gap = src.mid(at, c.headStart - at).trimmed();
        if (!gap.isEmpty()) outside << gap;
        at = c.end;
    }
    const QString tail = src.mid(at).trimmed();
    if (!tail.isEmpty()) outside << tail;
    result.preamble = outside.join(QStringLiteral("\n\n"));

    for (const ClassSpan &c : classes)
        result.scripts.append(buildScript(c, src, masked, cat, builtins, project, &result.notes));

    if (result.scripts.isEmpty()) {
        result.error = src.trimmed().isEmpty()
                           ? QStringLiteral("This file is empty, so there is no class to show as "
                                            "a graph.")
                           : QStringLiteral("This file declares no class, so there is nothing to "
                                            "show as a graph.");
        return result;
    }
    if (!result.preamble.isEmpty())
        result.notes.append(QStringLiteral("This file also holds code outside any class, which "
                                           "the graph has no room for. It is kept as it is."));

    // The ending belongs to the file, not to any method in it, so the answer
    // read above is carried on every graph the file produced. The generator
    // puts it back over the whole file, which is the only layer that can speak
    // for the class header and the braces as well as for the statements.
    for (ImportedScript &s : result.scripts) s.graph.eol = result.eol;
    if (result.eol.isEmpty())
        result.notes.append(QStringLiteral("This file is written with both kinds of line ending, "
                                           "and no single one puts it back as it was. Writing it "
                                           "from here gives every line a bare newline, so the "
                                           "lines that had a carriage return will change."));

    result.ok = true;
    return result;
}

ImportResult importEnforceFile(const QString &path, const Catalog &cat, const Builtins &builtins,
                               const Project &project)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        ImportResult result;
        result.error = QStringLiteral("Could not read %1: %2")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    // Vanilla source is not always valid UTF-8, and a decoder that refuses
    // takes the whole file with it, so the bytes are read as Latin-1 the way
    // the Enforce compiler reads them.
    const QString text = QString::fromLatin1(file.readAll());
    file.close();

    ImportResult result = importEnforceText(text, cat, builtins, project);
    const QString module = moduleForPath(path);
    if (!module.isEmpty())
        for (ImportedScript &s : result.scripts) s.graph.module = module;
    return result;
}

QString moduleForPath(const QString &path)
{
    // Deepest wins: a mod folder can sit inside a path that mentions another
    // module, and the one nearest the file is the one it compiles under.
    const QStringList parts = QFileInfo(path).absoluteFilePath().split(QLatin1Char('/'));
    for (int i = parts.size() - 1; i >= 0; --i) {
        const QString &p = parts.at(i);
        if (p.compare(QLatin1String("3_Game"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("3_Game");
        if (p.compare(QLatin1String("4_World"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("4_World");
        if (p.compare(QLatin1String("5_Mission"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("5_Mission");
    }
    return QString();
}
