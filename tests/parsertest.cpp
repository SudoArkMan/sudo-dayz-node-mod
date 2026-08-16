// Enforce parser tests.
//
// Three things are being measured. That a statement survives a round trip
// through the tree unchanged, that the tree it produces has the right shape,
// and how much real code the parser has to give up on. The last one is the
// honest measure of the import feature, so it is printed as a percentage and
// nothing here rounds it in the parser's favour.

#include "enforce/ast.h"
#include "enforce/lexer.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>
#include <cstdio>

static int fails = 0;
static int qtMessages = 0;

// The whole report goes through one stream. A second QTextStream over the same
// handle keeps a buffer of its own, so lines arrive in whatever order the two
// buffers happen to fill rather than the order they were written, and whichever
// buffer is not empty when the process stops is lost.
static QTextStream *reportStream = nullptr;

static void reportLine(const QString &line)
{
    if (reportStream) {
        *reportStream << line << Qt::endl;
        return;
    }
    // Anything said before main opens the stream still has to be readable.
    const QByteArray utf8 = line.toUtf8();
    fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    fputc('\n', stdout);
}

// Qt answers a fatal by writing to stderr and then calling
// RaiseFailFastException, which ends the process without unwinding and without
// flushing anything. Both the message and the unwritten tail of this report go
// with it, and all that reaches the terminal is exit code 0xC0000602. Routing
// Qt's own messages through the report stream means a fatal names itself.
static void relayQtMessage(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    qtMessages++;
    const char *label = type == QtFatalMsg      ? "  Qt FATAL    "
                        : type == QtCriticalMsg ? "  Qt critical "
                        : type == QtWarningMsg  ? "  Qt warning  "
                                                : "  Qt info     ";
    reportLine(QString::fromLatin1(label) + msg);
    if (reportStream) reportStream->flush();
    fflush(stdout);
    if (type != QtFatalMsg) return;

    reportLine(QStringLiteral("    report cut short by the fatal above, after %1 "
                              "failed checks")
                   .arg(fails));
    if (reportStream) reportStream->flush();
    fflush(stdout);
}

static void check(bool ok, const QString &what)
{
    reportLine((ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) + what);
    if (!ok) fails++;
}

// Significant tokens joined by single spaces: the round-trip comparison has to
// ignore layout without ignoring anything else.
static QString normalise(const QString &code)
{
    QStringList parts;
    for (const Token &t : EnforceLexer::tokenizeAll(code)) {
        if (t.kind == TokenKind::Whitespace || t.kind == TokenKind::Comment) continue;
        parts << t.text;
    }
    return parts.join(QLatin1Char(' '));
}

// The canonical form supplies two things the author may have left out: braces
// around a single-statement body, and the semicolon on a fragment. Neither
// changes what the code does, so they are excluded here and every other token
// is compared exactly. Nesting is still pinned down, by the round-trip corpus
// below, which compares braces and all.
static QString contentTokens(const QString &code)
{
    QStringList parts;
    for (const Token &t : EnforceLexer::tokenizeAll(code)) {
        if (t.kind == TokenKind::Whitespace || t.kind == TokenKind::Comment) continue;
        // Operators go in one character at a time. The tokeniser chunks a run
        // by greed, so `!=-1` and `!= -1` are the same code but different
        // tokens, as are `array<X> >` and `array<X>>`.
        if (t.kind == TokenKind::Operator) {
            for (int i = 0; i < t.text.size(); ++i) parts << t.text.mid(i, 1);
            continue;
        }
        parts << t.text;
    }
    // A comma directly before a closing bracket is a trailing comma, which
    // vanilla writes in a few calls and initialisers and the printer drops.
    // This has to run while the brackets are still here to be seen.
    for (int i = parts.size() - 2; i >= 0; --i)
        if (parts.at(i) == QLatin1String(",")
            && (parts.at(i + 1) == QLatin1String(")") || parts.at(i + 1) == QLatin1String("}")))
            parts.removeAt(i);
    parts.removeAll(QStringLiteral("{"));
    parts.removeAll(QStringLiteral("}"));
    parts.removeAll(QStringLiteral(";"));
    return parts.join(QLatin1Char(' '));
}

static QString printAll(const ParseResult &r)
{
    QStringList out;
    for (const StmtPtr &s : r.statements) out << stmtToText(*s, 0);
    return out.join(QLatin1Char('\n'));
}

// An s-expression of the tree, so precedence can be asserted on the shape
// rather than on text that would hide a wrong association behind brackets.
static QString shape(const Expr *e)
{
    if (!e) return QStringLiteral("<null>");
    switch (e->kind) {
    case ExprKind::Literal:
    case ExprKind::Name:
        return e->text;
    case ExprKind::Member:
        return QStringLiteral("(. ") + shape(e->target.get()) + QLatin1Char(' ') + e->text
               + QLatin1Char(')');
    case ExprKind::Call: {
        QString s = QStringLiteral("(call ") + shape(e->target.get());
        for (const ExprPtr &a : e->args) s += QLatin1Char(' ') + shape(a.get());
        return s + QLatin1Char(')');
    }
    case ExprKind::Index:
        return QStringLiteral("(idx ") + shape(e->target.get()) + QLatin1Char(' ')
               + shape(e->second.get()) + QLatin1Char(')');
    case ExprKind::Unary:
        return QLatin1Char('(') + e->op + QLatin1Char(' ') + shape(e->target.get())
               + QLatin1Char(')');
    case ExprKind::Binary:
    case ExprKind::Assign:
        return QLatin1Char('(') + e->op + QLatin1Char(' ') + shape(e->target.get())
               + QLatin1Char(' ') + shape(e->second.get()) + QLatin1Char(')');
    case ExprKind::Ternary:
        return QStringLiteral("(?: ") + shape(e->target.get()) + QLatin1Char(' ')
               + shape(e->second.get()) + QLatin1Char(' ') + shape(e->third.get())
               + QLatin1Char(')');
    case ExprKind::New: {
        QString s = QStringLiteral("(new ") + e->typeName;
        for (const ExprPtr &a : e->args) s += QLatin1Char(' ') + shape(a.get());
        return s + QLatin1Char(')');
    }
    case ExprKind::Cast:
        if (e->op == QLatin1String("prefix"))
            return QStringLiteral("(ccast ") + e->typeName + QLatin1Char(' ')
                   + shape(e->target.get()) + QLatin1Char(')');
        if (e->op == QLatin1String("CastTo"))
            return QStringLiteral("(castto ") + e->typeName + QLatin1Char(' ')
                   + shape(e->second.get()) + QLatin1Char(' ') + shape(e->target.get())
                   + QLatin1Char(')');
        return QStringLiteral("(cast ") + e->typeName + QLatin1Char(' ') + shape(e->target.get())
               + QLatin1Char(')');
    case ExprKind::Paren:
        return QStringLiteral("(paren ") + shape(e->target.get()) + QLatin1Char(')');
    }
    return QStringLiteral("<?>");
}

// Shape of the first statement's expression, for the precedence checks.
static QString firstExprShape(const QString &code)
{
    const ParseResult r = parseEnforceBody(code);
    if (r.statements.empty()) return QStringLiteral("<empty>");
    const Stmt *s = r.statements.front().get();
    if (s->kind == StmtKind::Raw) return QStringLiteral("<raw:") + s->text + QLatin1Char('>');
    return shape(s->expr.get());
}

static void checkShape(const QString &code, const QString &expected)
{
    const QString got = firstExprShape(code);
    check(got == expected, QStringLiteral("%1  ->  %2").arg(code, got)
                               + (got == expected ? QString()
                                                  : QStringLiteral("   expected ") + expected));
}

// parse -> print must reproduce the input, and printing must be stable: a
// second pass through the parser has to give exactly the same text or the
// importer would drift a little further from the user's code every time.
static void checkRoundTrip(const QString &code)
{
    const ParseResult r = parseEnforceBody(code);
    const QString printed = printAll(r);
    const bool same = normalise(printed) == normalise(code);
    const bool stable = printAll(parseEnforceBody(printed)) == printed;
    bool anyRaw = false;
    for (const StmtPtr &s : r.statements)
        if (s->kind == StmtKind::Raw) anyRaw = true;

    QString label = code;
    label.replace(QLatin1Char('\n'), QStringLiteral(" \\n "));
    label.replace(QLatin1Char('\t'), QString());
    if (label.size() > 62) label = label.left(59) + QStringLiteral("...");

    if (anyRaw) {
        check(false, label + QStringLiteral("   [fell back to raw]"));
        return;
    }
    if (!same) {
        QString got = printed;
        got.replace(QLatin1Char('\n'), QStringLiteral(" \\n "));
        reportLine(QStringLiteral("         got: ") + got);
    }
    check(same && stable, label + (stable ? QString() : QStringLiteral("   [not stable]")));
}

// ---------------------------------------------------------------- corpora

struct Corpus {
    int statements = 0;
    int raw = 0;
    int rawPreproc = 0; // a directive is not a statement, so it is counted apart
    int bodies = 0;
    int cleanBodies = 0; // bodies with no raw statement at all
    int withErrors = 0;
    int changed = 0;  // bodies whose code did not survive the round trip
    int unstable = 0; // bodies whose printed form changes again on the next pass
    QMap<QString, int> reasons;
    QStringList examples;
    QStringList changedExamples;

    void add(const ParseResult &r, const QString &source)
    {
        bodies++;
        // The one thing that must never happen: code that comes back out
        // different from how it went in. A raw statement keeps its original
        // text, so this holds whether or not the parser understood the body.
        const QString printed = printAll(r);
        // Printing has to be a fixed point, or every import moves the code a
        // little further from what the author wrote.
        const QString again = printAll(parseEnforceBody(printed));
        if (r.errors.isEmpty() && again != printed) {
            unstable++;
            if (changedExamples.size() < 12) {
                int i = 0;
                while (i < printed.size() && i < again.size() && printed.at(i) == again.at(i)) i++;
                QString a = printed.mid(qMax(0, i - 30), 80);
                QString b = again.mid(qMax(0, i - 30), 80);
                a.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
                b.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
                changedExamples << QStringLiteral("unstable once: ...") + a;
                changedExamples << QStringLiteral("unstable twice: ...") + b;
            }
        }

        const QString before = contentTokens(source);
        const QString after = contentTokens(printed);
        if (r.errors.isEmpty() && before != after) {
            changed++;
            if (changedExamples.size() < 12) {
                // The first token that differs, with context, since the two
                // streams are usually identical for hundreds of characters.
                int i = 0;
                while (i < before.size() && i < after.size() && before.at(i) == after.at(i)) i++;
                const int from = qMax(0, i - 30);
                changedExamples << QStringLiteral("was: ...") + before.mid(from, 80);
                changedExamples << QStringLiteral("got: ...") + after.mid(from, 80);
            }
        }
        statements += r.statementCount;
        raw += r.rawCount;
        if (r.rawCount == 0) cleanBodies++;
        if (!r.errors.isEmpty()) withErrors++;
        for (const QString &n : r.notes) {
            // "line 12: reason (code)" -> "reason"
            QString reason = n.section(QStringLiteral(": "), 1);
            const int tail = reason.lastIndexOf(QStringLiteral(" ("));
            if (tail > 0 && reason.endsWith(QLatin1Char(')'))) reason = reason.left(tail);
            reasons[reason]++;
            if (reason == QLatin1String("preprocessor directive")) {
                rawPreproc++;
                continue;
            }
            if (examples.size() < 25) examples << n;
        }
    }

    double rawPercent() const
    {
        return statements > 0 ? 100.0 * double(raw) / double(statements) : 0.0;
    }
};

static bool verbose = false;

static void report(const QString &title, const Corpus &c)
{
    QTextStream &o = *reportStream;
    o << Qt::endl << "  " << title << Qt::endl;
    o << "    bodies              " << c.bodies << " (" << c.cleanBodies
      << " fully modelled, " << c.withErrors << " with syntax errors)" << Qt::endl;
    o << "    statements          " << c.statements << Qt::endl;
    o << "    raw fallbacks       " << c.raw << Qt::endl;
    o << "    RAW RATE            " << QString::number(c.rawPercent(), 'f', 2) << " %" << Qt::endl;
    // Split out, not deducted: a #ifdef line is not a statement anyone could
    // turn into a node, so it says something different about the parser than a
    // construct it failed to read.
    o << "      of which #directives  " << c.rawPreproc << Qt::endl;
    o << "    CODE CHANGED        " << c.changed << "  (must be 0)" << Qt::endl;
    o << "    PRINT UNSTABLE      " << c.unstable << "  (must be 0)" << Qt::endl;
    for (const QString &e : c.changedExamples) o << "      ! " << e << Qt::endl;
    if (verbose) {
        for (const QString &e : c.examples) o << "      | " << e << Qt::endl;
    }
    if (c.reasons.isEmpty()) return;
    QVector<QPair<int, QString>> sorted;
    for (auto it = c.reasons.constBegin(); it != c.reasons.constEnd(); ++it)
        sorted.append({it.value(), it.key()});
    std::sort(sorted.begin(), sorted.end(),
              [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                  return a.first > b.first;
              });
    o << "    top reasons" << Qt::endl;
    for (int i = 0; i < sorted.size() && i < 14; ++i)
        o << "      " << sorted.at(i).first << "  " << sorted.at(i).second << Qt::endl;
}

// CF and Dabs, the two frameworks most installed mods build on. Third-party
// code is the harder corpus and the one the mod importer meets, so the round
// trip is worth holding against it and not only against vanilla. Absent is not
// a failure, the same as P:/scripts. SUDO_THIRD_PARTY_SCRIPTS overrides, as a
// path list, for a machine that keeps them somewhere else.
static QStringList thirdPartyRoots()
{
    const QByteArray env = qgetenv("SUDO_THIRD_PARTY_SCRIPTS");
    QStringList roots;
    if (!env.isEmpty())
        roots = QString::fromLocal8Bit(env).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    else
        roots = QStringList{
            QStringLiteral("C:/Users/dilla/Downloads/DayZ-CommunityFramework-production"),
            QStringLiteral("C:/Users/dilla/Downloads/DayZ-Dabs-Framework-production"),
        };
    QStringList out;
    for (const QString &r : roots)
        if (QDir(r).exists()) out << r;
    return out;
}

// Method bodies out of a vanilla file. A definition is the only place `) {`
// follows an identifier: every control statement that shape could match is
// introduced by a keyword instead.
static QStringList extractBodies(const QString &src)
{
    QVector<Token> sig;
    for (const Token &t : EnforceLexer::tokenizeAll(src))
        if (t.kind != TokenKind::Whitespace && t.kind != TokenKind::Comment) sig.append(t);

    QStringList bodies;
    for (int i = 0; i < sig.size(); ++i) {
        if (sig.at(i).text != QLatin1String("(")) continue;
        const TokenKind before = i > 0 ? sig.at(i - 1).kind : TokenKind::Unknown;
        if (before != TokenKind::Identifier && before != TokenKind::Type
            && before != TokenKind::ClassName)
            continue;
        int depth = 1;
        int j = i + 1;
        for (; j < sig.size() && depth > 0; ++j) {
            if (sig.at(j).text == QLatin1String("(")) depth++;
            else if (sig.at(j).text == QLatin1String(")")) depth--;
        }
        if (depth != 0 || j >= sig.size() || sig.at(j).text != QLatin1String("{")) continue;

        const int open = j;
        int braces = 1;
        int k = open + 1;
        for (; k < sig.size() && braces > 0; ++k) {
            if (sig.at(k).text == QLatin1String("{")) braces++;
            else if (sig.at(k).text == QLatin1String("}")) braces--;
        }
        if (braces != 0) break;
        const int close = k - 1;
        const int from = sig.at(open).start + 1;
        const int to = sig.at(close).start;
        if (to > from) bodies << src.mid(from, to - from);
        i = close; // do not descend into the body looking for more
    }
    return bodies;
}

static void scanVanilla(Corpus &c, const QString &root, int maxFiles)
{
    QDirIterator it(root, QStringList{QStringLiteral("*.c")}, QDir::Files,
                    QDirIterator::Subdirectories);
    QStringList files;
    while (it.hasNext()) files << it.next();
    files.sort();
    int used = 0;
    for (const QString &path : files) {
        if (used >= maxFiles) break;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(f.readAll());
        const QStringList bodies = extractBodies(text);
        if (bodies.isEmpty()) continue;
        used++;
        for (const QString &b : bodies) {
            if (b.trimmed().isEmpty()) continue;
            c.add(parseEnforceBody(b), b);
        }
    }
}

// Every bi.raw node in a project file, wherever the graph keeps it.
static void collectRawCode(const QJsonValue &v, QStringList *out)
{
    if (v.isArray()) {
        for (const QJsonValue &child : v.toArray()) collectRawCode(child, out);
        return;
    }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    const QString ref = o.value(QStringLiteral("ref")).toString();
    if (ref == QLatin1String("bi.raw") || ref == QLatin1String("bi.rawExpr")) {
        const QString code =
            o.value(QStringLiteral("opts")).toObject().value(QStringLiteral("code")).toString();
        if (!code.trimmed().isEmpty()) *out << code;
    }
    for (const QString &key : o.keys()) collectRawCode(o.value(key), out);
}

int main(int argc, char **argv)
{
    // Nothing may sit in a buffer waiting for a clean exit. The numbers this run
    // exists to print are the last thing written, so a process that dies without
    // unwinding takes exactly the part worth reading.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // Installed before the application object, because construction can warn.
    qInstallMessageHandler(relayQtMessage);

    QCoreApplication app(argc, argv);
    QTextStream o(stdout);
    reportStream = &o;
    QString resources = QStringLiteral("resources");
    bool scanEverything = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("-v")) verbose = true;
        else if (arg == QLatin1String("-all")) scanEverything = true;
        else resources = arg;
    }

    o << "round trip" << Qt::endl;
    const QStringList corpus = {
        // the user's own code, the reason this exists
        QStringLiteral("m_BaseUrl = baseUrl;"),
        QStringLiteral("m_SessionToken = \"\";"),
        QStringLiteral("m_Linked = false;"),
        QStringLiteral("m_RestApi = CreateRestApi();"),
        QStringLiteral("if (!m_RestApi)\n{\n\treturn;\n}"),
        QStringLiteral("m_RestApi.EnableDebug(false);"),
        // declarations
        QStringLiteral("int count = 0;"),
        QStringLiteral("float a = 0, b = 1;"),
        QStringLiteral("string name;"),
        QStringLiteral("ref array<ref ItemBase> items = new array<ref ItemBase>();"),
        QStringLiteral("map<string, ref array<int>> lookup = new map<string, ref array<int>>();"),
        QStringLiteral("autoptr Timer t = new Timer(CALL_CATEGORY_SYSTEM);"),
        QStringLiteral("static const int MAX = 10;"),
        QStringLiteral("TStringArray parts = new TStringArray();"),
        // control flow
        QStringLiteral("if (a > b)\n{\n\tA();\n}\nelse\n{\n\tB();\n}"),
        QStringLiteral("if (a)\n{\n\tA();\n}\nelse if (b)\n{\n\tB();\n}\nelse\n{\n\tC();\n}"),
        QStringLiteral("for (int i = 0; i < n; i++)\n{\n\tPrint(i);\n}"),
        QStringLiteral("for (;;)\n{\n\tbreak;\n}"),
        QStringLiteral("for (int i = 0, j = n; i < j; i++, j--)\n{\n\tSwap(i, j);\n}"),
        QStringLiteral("foreach (ItemBase item : items)\n{\n\titem.Delete();\n}"),
        QStringLiteral("foreach (int i, ItemBase item : items)\n{\n\tPrint(i);\n}"),
        QStringLiteral("foreach (string key, int value : m_Scores)\n{\n\tPrint(key);\n}"),
        QStringLiteral("while (i < 10)\n{\n\ti++;\n}"),
        QStringLiteral("switch (state)\n{\n\tcase 1:\n\t\tA();\n\t\tbreak;\n\tcase 2:\n"
                       "\tcase 3:\n\t\tB();\n\t\tbreak;\n\tdefault:\n\t\tC();\n}"),
        QStringLiteral("{\n\tint x = 0;\n}"),
        QStringLiteral("return;"),
        QStringLiteral("return m_Linked;"),
        QStringLiteral("continue;"),
        QStringLiteral("delete m_RestApi;"),
        // expressions
        QStringLiteral("Class.CastTo(item, obj);"),
        QStringLiteral("ItemBase item = ItemBase.Cast(obj);"),
        QStringLiteral("if (Class.CastTo(item, obj))\n{\n\titem.Delete();\n}"),
        QStringLiteral("GetGame().GetPlayer().SetHealth(100);"),
        QStringLiteral("super.EEInit(other);"),
        QStringLiteral("this.Foo();"),
        QStringLiteral("m_Array[0] = 5;"),
        QStringLiteral("x = a ? b : c;"),
        QStringLiteral("y = (a + b) * c;"),
        QStringLiteral("z = !a && b || c;"),
        QStringLiteral("i += 1;"),
        QStringLiteral("i++;"),
        QStringLiteral("++i;"),
        QStringLiteral("m_Pos = \"1 0 0\";"),
        QStringLiteral("Print(string.Format(\"%1 %2\", a, b));"),
        QStringLiteral("flags = flags | 0x10;"),
        QStringLiteral("shift = value >> 2;"),
        QStringLiteral("value <<= 3;"),
        QStringLiteral("if (a == b && c != d)\n{\n\tDoThing();\n}"),
        QStringLiteral("total = -count;"),
        QStringLiteral("h = 1.5e-3;"),
        QStringLiteral("obj = null;"),
        QStringLiteral("m_Callback = new JsonApiStruct();"),
        QStringLiteral("arr = {1, 2, 3};"),
        // shapes vanilla itself uses
        QStringLiteral("m_valuesBool.Insert(key, (int)value);"),
        QStringLiteral("Param2<float, float> p = Param2<float, float>.Cast(param);"),
        QStringLiteral("if (!JsonFileLoader<ref CTSaveStructure>.LoadFile(path, data))\n"
                       "{\n\treturn;\n}"),
        // playerbase.c line 796. A block comment over several lines used to be
        // read as a comment nobody closed, which reported a syntax error on the
        // whole body and stopped any of it being modelled.
        QStringLiteral("if (allow)\n{\n\t/*\n\tPrint(\"ON Enabling effect \" + trigger);\n"
                       "\t*/\n\tm_CurrentEffectTrigger = trigger;\n}"),
    };
    for (const QString &c : corpus) checkRoundTrip(c);

    o << Qt::endl << "precedence" << Qt::endl;
    checkShape(QStringLiteral("a + b * c;"), QStringLiteral("(+ a (* b c))"));
    checkShape(QStringLiteral("a * b + c;"), QStringLiteral("(+ (* a b) c)"));
    checkShape(QStringLiteral("a - b - c;"), QStringLiteral("(- (- a b) c)"));
    checkShape(QStringLiteral("!a && b;"), QStringLiteral("(&& (! a) b)"));
    checkShape(QStringLiteral("a || b && c;"), QStringLiteral("(|| a (&& b c))"));
    checkShape(QStringLiteral("a = b = c;"), QStringLiteral("(= a (= b c))"));
    checkShape(QStringLiteral("a ? b : c ? d : e;"), QStringLiteral("(?: a b (?: c d e))"));
    checkShape(QStringLiteral("x.y().z[0] + 1;"),
               QStringLiteral("(+ (idx (. (call (. x y)) z) 0) 1)"));
    checkShape(QStringLiteral("m_RestApi = CreateRestApi();"),
               QStringLiteral("(= m_RestApi (call CreateRestApi))"));
    checkShape(QStringLiteral("a == b == c;"), QStringLiteral("(== (== a b) c)"));
    checkShape(QStringLiteral("a & b | c ^ d;"), QStringLiteral("(| (& a b) (^ c d))"));
    checkShape(QStringLiteral("a + b << 2;"), QStringLiteral("(<< (+ a b) 2)"));
    checkShape(QStringLiteral("i++;"), QStringLiteral("(post++ i)"));
    checkShape(QStringLiteral("++i;"), QStringLiteral("(++ i)"));
    checkShape(QStringLiteral("x = ItemBase.Cast(obj);"),
               QStringLiteral("(= x (cast ItemBase obj))"));
    checkShape(QStringLiteral("Class.CastTo(dst, src);"),
               QStringLiteral("(castto Class dst src)"));
    checkShape(QStringLiteral("x = new Timer(a, b);"), QStringLiteral("(= x (new Timer a b))"));
    checkShape(QStringLiteral("x = -a * b;"), QStringLiteral("(= x (* (- a) b))"));
    checkShape(QStringLiteral("x = (a + b) * c;"), QStringLiteral("(= x (* (paren (+ a b)) c))"));
    checkShape(QStringLiteral("x = (int)value;"), QStringLiteral("(= x (ccast int value))"));
    checkShape(QStringLiteral("x = JsonFileLoader<Cfg>.Load(p);"),
               QStringLiteral("(= x (call (. JsonFileLoader<Cfg> Load) p))"));
    // a generic only closes when a call or a member access follows, so this
    // stays two comparisons rather than becoming a type
    checkShape(QStringLiteral("b = a < c;"), QStringLiteral("(= b (< a c))"));

    o << Qt::endl << "structure" << Qt::endl;
    {
        const ParseResult braced = parseEnforceBody(QStringLiteral("if (x) { y(); }"));
        const ParseResult bare = parseEnforceBody(QStringLiteral("if (x) y();"));
        check(!braced.statements.empty() && !bare.statements.empty()
                  && stmtToText(*braced.statements.front(), 0)
                         == stmtToText(*bare.statements.front(), 0),
              "a braced body and a bare body give the same tree");
        check(!bare.statements.empty() && bare.statements.front()->body.size() == 1
                  && bare.statements.front()->body.front()->kind == StmtKind::Expression,
              "a control body is spliced in, not wrapped in a block");
    }
    {
        const ParseResult r = parseEnforceBody(
            QStringLiteral("if (!m_RestApi)\n{\n\tSUDO_Log.Error(\"gone\");\n\treturn;\n}"));
        const Stmt *s = r.statements.empty() ? nullptr : r.statements.front().get();
        check(s && s->kind == StmtKind::If && shape(s->expr.get()) == QLatin1String("(! m_RestApi)")
                  && s->body.size() == 2,
              "the branch the whole feature is for");
    }
    {
        const ParseResult r = parseEnforceBody(
            QStringLiteral("foreach (string key, int value : m_Scores) { Print(key); }"));
        const Stmt *s = r.statements.empty() ? nullptr : r.statements.front().get();
        check(s && s->kind == StmtKind::ForEach && s->eachIndexType == QLatin1String("string")
                  && s->eachIndexName == QLatin1String("key")
                  && s->typeName == QLatin1String("int"),
              "a map foreach keeps the key type");
    }
    {
        const ParseResult r =
            parseEnforceBody(QStringLiteral("array<array<int>> grid = new array<array<int>>();"));
        const Stmt *s = r.statements.empty() ? nullptr : r.statements.front().get();
        check(s && s->kind == StmtKind::VarDecl
                  && s->typeName == QLatin1String("array<array<int>>"),
              "nested generics close without eating the shift operator");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("int a = 1;\nint b = 2;"));
        check(r.statements.size() == 2 && r.statementCount == 2 && r.rawCount == 0,
              "statements are counted");
    }

    o << Qt::endl << "never fails" << Qt::endl;
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("@@@ ###\nint a = 1;"));
        check(r.rawCount >= 1, "garbage becomes raw");
        check(r.statements.size() >= 2 && r.notes.size() == r.rawCount,
              "recovery continues to the next statement, with one note each");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("if (a) {\n\tb();"));
        check(!r.errors.isEmpty(), "an unbalanced brace is reported as an error");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("a();\n}\nb();"));
        check(!r.errors.isEmpty() && r.statementCount == 2,
              "a stray close brace is an error and the code around it still parses");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("do\n{\n\tx();\n}\nwhile (a);"));
        check(r.rawCount >= 1 && r.errors.isEmpty(), "do-while falls back to raw, not an error");
    }
    {
        const ParseResult r =
            parseEnforceBody(QStringLiteral("#ifdef SERVER\nDoThing();\n#endif"));
        check(r.rawCount == 2 && r.statementCount == 3,
              "a preprocessor line is raw on its own and the code around it still parses");
    }
    {
        const ParseResult r = parseEnforceBody(QString());
        check(r.statements.empty() && r.errors.isEmpty(), "empty input is not an error");
    }

    o << Qt::endl << "block comments" << Qt::endl;
    {
        // playerbase.c line 796, the shape that cost 266 vanilla bodies. The
        // lexer works a line at a time, so the opening line of a block comment
        // and one that is never closed are the same token.
        const ParseResult r = parseEnforceBody(
            QStringLiteral("if (allow)\n{\n\t/*\n\tPrint(\"ON Enabling effect \" + trigger);\n"
                           "\t*/\n\tm_CurrentEffectTrigger = trigger;\n}"));
        check(r.errors.isEmpty(), "a block comment over several lines is not a syntax error");
        check(r.statementCount == 2 && r.rawCount == 0,
              "and the code around it is modelled rather than kept as text");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("/* {\n} */\nint a = 1;"));
        check(r.errors.isEmpty() && r.statementCount == 1 && r.rawCount == 0,
              "braces inside a block comment are not braces");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("int a = 1;\n/* opened\nand left"));
        check(!r.errors.isEmpty(), "a comment that really is not closed is still an error");
    }
    {
        // Three characters, and the shortest closed block comment is four, so
        // this one is open. Reading the token text rather than the lexer's state
        // gets this backwards.
        const ParseResult r = parseEnforceBody(QStringLiteral("/*/\nint a = 1;"));
        check(!r.errors.isEmpty(), "`/*/` opens a comment, it does not open and close one");
    }
    {
        const ParseResult r = parseEnforceBody(QStringLiteral("/* a */ int x = 1; /* b */"));
        check(r.errors.isEmpty() && r.statementCount == 1,
              "two closed comments on one line leave nothing open");
    }

    o << Qt::endl << "declarations the graph has no shape for" << Qt::endl;
    {
        // dayzplayerinventory.c line 2641. Enforce declares a local and runs its
        // constructor in one statement. The graph has no shape for it, so the
        // statement keeps its text; what it must not do is come apart into a
        // bare `ScriptInputUserData` and a call to a function of that name.
        const ParseResult r = parseEnforceBody(
            QStringLiteral("ScriptInputUserData serializer();\nserializer.Write(reason);"));
        check(r.statements.size() == 2 && r.rawCount == 1
                  && r.statements.front()->kind == StmtKind::Raw
                  && r.statements.front()->text
                         == QLatin1String("ScriptInputUserData serializer();"),
              "a declaration that runs a constructor is one statement, kept whole");
    }
    {
        // CF_MathExpression.c line 2. The same shape with a generic type, which
        // used to be turned down as an unexpected `ref`.
        const ParseResult r = parseEnforceBody(
            QStringLiteral("array<ref CF_ExpressionCompileToken> dataStackStore();"));
        check(r.statements.size() == 1 && r.rawCount == 1
                  && r.statements.front()->text
                         == QLatin1String("array<ref CF_ExpressionCompileToken> "
                                          "dataStackStore();"),
              "and the same with a generic type");
    }
    {
        // actiondigoutstash.c line 23, the shape with constructor arguments.
        const ParseResult r = parseEnforceBody(
            QStringLiteral("DigOutStashLambda lambda(stash, \"\", action_data.m_Player);"));
        check(r.statements.size() == 1 && r.rawCount == 1,
              "constructor arguments do not make it a call to `lambda`");
    }
    {
        check(firstExprShape(QStringLiteral("Spawn(item);"))
                  == QLatin1String("(call Spawn item)"),
              "a plain call is still a call");
    }
    {
        // endebug.c line 9, and 176 more across vanilla. Refused on purpose: the
        // graph has no field for the size, and a VarDecl without it would come
        // back out as `vector pts;`. Pinned so the refusal stays a decision.
        const ParseResult r = parseEnforceBody(QStringLiteral("vector pts[5];"));
        check(r.statements.size() == 1 && r.rawCount == 1
                  && r.statements.front()->text == QLatin1String("vector pts[5];"),
              "a fixed-size array declaration keeps its text, size and all");
    }
    {
        QString deep;
        for (int i = 0; i < 400; ++i) deep += QStringLiteral("if (a) {");
        deep += QStringLiteral("b();");
        for (int i = 0; i < 400; ++i) deep += QLatin1Char('}');
        const ParseResult r = parseEnforceBody(deep);
        check(r.statementCount > 0, "400 levels of nesting terminates");
    }
    {
        QString wide;
        for (int i = 0; i < 300; ++i) wide += QStringLiteral("a + ");
        wide += QStringLiteral("b;");
        const ParseResult r = parseEnforceBody(wide);
        check(r.statementCount == 1, "a 300 term expression terminates");
    }

    // ---------------------------------------------------------- the numbers
    o << Qt::endl << "corpora" << Qt::endl;

    Corpus vanilla;
    if (QDir(QStringLiteral("P:/scripts")).exists()) {
        // Capped so the test stays quick. `-all` reads every file under
        // P:/scripts, which is the number to quote when the parser changes.
        const int cap = scanEverything ? 100000 : 60;
        scanVanilla(vanilla, QStringLiteral("P:/scripts/4_world"), cap);
        scanVanilla(vanilla, QStringLiteral("P:/scripts/5_mission"), scanEverything ? cap : 40);
        scanVanilla(vanilla, QStringLiteral("P:/scripts/3_game"), scanEverything ? cap : 50);
        if (scanEverything) {
            scanVanilla(vanilla, QStringLiteral("P:/scripts/1_core"), cap);
            scanVanilla(vanilla, QStringLiteral("P:/scripts/2_gamelib"), cap);
        }
        report(QStringLiteral("vanilla DayZ gameplay code (P:/scripts)"), vanilla);
        check(vanilla.statements > 5000,
              QStringLiteral("parsed %1 vanilla statements").arg(vanilla.statements));
        check(vanilla.rawPercent() < 10.0,
              QStringLiteral("vanilla raw rate %1 %% is under 10 %%")
                  .arg(QString::number(vanilla.rawPercent(), 'f', 2)));
        check(vanilla.changed == 0,
              QStringLiteral("%1 vanilla bodies came back as different code")
                  .arg(vanilla.changed));
        check(vanilla.unstable == 0,
              QStringLiteral("%1 vanilla bodies do not print to a fixed point")
                  .arg(vanilla.unstable));
    } else {
        o << "  P: is not mounted, skipping the vanilla corpus" << Qt::endl;
    }

    const QStringList third = thirdPartyRoots();
    if (!third.isEmpty()) {
        Corpus frameworks;
        for (const QString &root : third) scanVanilla(frameworks, root, 100000);
        report(QStringLiteral("third-party frameworks (CF, Dabs)"), frameworks);
        check(frameworks.statements > 5000,
              QStringLiteral("parsed %1 third-party statements").arg(frameworks.statements));
        check(frameworks.rawPercent() < 10.0,
              QStringLiteral("third-party raw rate %1 %% is under 10 %%")
                  .arg(QString::number(frameworks.rawPercent(), 'f', 2)));
        check(frameworks.changed == 0,
              QStringLiteral("%1 third-party bodies came back as different code")
                  .arg(frameworks.changed));
        check(frameworks.unstable == 0,
              QStringLiteral("%1 third-party bodies do not print to a fixed point")
                  .arg(frameworks.unstable));
    } else {
        o << "  CF and Dabs are not on disk, skipping the third-party corpus" << Qt::endl;
    }

    const QString projectPath = resources + QStringLiteral("/SUDO_Link.sdzn");
    QFile pf(projectPath);
    if (pf.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(pf.readAll(), &err);
        QStringList blobs;
        collectRawCode(QJsonValue(doc.object()), &blobs);
        Corpus user;
        for (const QString &b : blobs) user.add(parseEnforceBody(b), b);
        report(QStringLiteral("SUDO_Link raw nodes (the user's own project)"), user);
        o << "    raw nodes that become real nodes   " << user.cleanBodies << " of " << user.bodies
          << Qt::endl;
        check(!blobs.isEmpty(), QStringLiteral("read %1 raw nodes from SUDO_Link.sdzn")
                                    .arg(blobs.size()));
        check(user.rawPercent() < 10.0,
              QStringLiteral("SUDO_Link raw rate %1 %% is under 10 %%")
                  .arg(QString::number(user.rawPercent(), 'f', 2)));
        check(user.changed == 0,
              QStringLiteral("%1 raw nodes came back as different code").arg(user.changed));
        check(user.unstable == 0,
              QStringLiteral("%1 raw nodes do not print to a fixed point").arg(user.unstable));
    } else {
        o << "  " << projectPath << " not found, skipping the project corpus" << Qt::endl;
    }

    // A Qt message in a run that has no GUI is the shape of a bug that ends the
    // process with RaiseFailFastException and no explanation. Named here, it is
    // a failing check rather than a hex code.
    o << Qt::endl;
    check(qtMessages == 0,
          QStringLiteral("Qt said nothing during the run (%1 messages)").arg(qtMessages));

    o << Qt::endl << (fails == 0 ? QStringLiteral("PARSER OK")
                                 : QStringLiteral("%1 FAILURES").arg(fails))
      << Qt::endl;

    // The stream goes out of scope on the next line; the handler must not reach
    // for it after that.
    o.flush();
    reportStream = nullptr;
    return fails == 0 ? 0 : 1;
}
