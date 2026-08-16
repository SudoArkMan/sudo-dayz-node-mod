// Enforce file to graph, and back again.
//
// The bar is the round trip, and it is measurable because the generator is the
// inverse of the importer: read a file into a graph, generate a file from that
// graph, and the two have to say the same thing. Three numbers are printed for
// the vanilla corpus, and none of them is smoothed over:
//
//   exact       byte for byte, the original file and the generated one
//   restored    the same, once the generator's own furniture is set aside: the
//               preserved region it writes into every file, where it puts its
//               braces, and blank lines
//   same code   the same again, once spacing inside a line stops counting
//
// Anything else that changes is a loss, and the run says what it was. A file
// this app generated itself is held to the first of the three: it has to come
// back byte for byte, since clicking a script must not rewrite it.
//
//   ./tests/importtest ../resources [-v] [sample size]
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "enforce/import.h"
#include "enforce/lexer.h"
#include "graph.h"
#include "project.h"
#include "widgets/newscriptdialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cstdio>

static int failures = 0;
static int qtMessages = 0;

// One stream for the whole report. A second QTextStream over the same handle
// keeps a buffer of its own, so lines would arrive in whichever order the two
// buffers happen to fill.
static QTextStream *reportStream = nullptr;

static void reportLine(const QString &line)
{
    if (reportStream) {
        *reportStream << line << Qt::endl;
        return;
    }
    const QByteArray utf8 = line.toUtf8();
    fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    fputc('\n', stdout);
}

// Qt answers a fatal by writing to stderr and ending the process without
// unwinding, which takes the unwritten tail of this report with it. Routed
// here, a fatal names itself instead of arriving as an exit code.
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
    reportLine(QStringLiteral("       report cut short by the fatal above, after %1 failed checks")
                   .arg(failures));
    if (reportStream) reportStream->flush();
    fflush(stdout);
}

static void check(bool ok, const QString &what)
{
    reportLine((ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) + what);
    if (!ok) failures++;
}

namespace {

// Indentation in a corpus case is written with a leading bar so no editor
// setting can turn a tab into spaces and quietly change what is tested.
QString enf(const char *text)
{
    QStringList out;
    for (const QString &line : QString::fromUtf8(text).split(QLatin1Char('\n'))) {
        int bars = 0;
        while (bars < line.size() && line.at(bars) == QLatin1Char('|')) bars++;
        out << QString(bars, QLatin1Char('\t')) + line.mid(bars);
    }
    return out.join(QLatin1Char('\n'));
}

// ------------------------------------------------------------- comparison

// Every brace onto a line of its own, keeping the indent of the line it came
// from. `class X : Y {}` and the four lines it comes back as are the same
// declaration, and a trailing `;` after a closing brace is the same brace.
void appendBraceSplit(QStringList *out, const QString &line)
{
    int lead = 0;
    while (lead < line.size() && line.at(lead).isSpace()) lead++;
    const QString indent = line.left(lead);
    QString piece;
    const auto flush = [&]() {
        const QString t = piece.trimmed();
        piece.clear();
        if (!t.isEmpty() && t != QLatin1String(";")) out->append(indent + t);
    };
    for (int i = lead; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c != QLatin1Char('{') && c != QLatin1Char('}')) {
            piece.append(c);
            continue;
        }
        flush();
        if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char(';')) i++;
        out->append(indent + c);
    }
    flush();
}

// The generator writes the modifiers of a declaration in one order. The model
// carries flags, not an order, so `override protected void X()` can only come
// back as `protected override void X()`, which is the same declaration.
QString canonicalModifiers(const QString &body)
{
    static const QStringList order = {
        QStringLiteral("private"),  QStringLiteral("protected"), QStringLiteral("static"),
        QStringLiteral("override"), QStringLiteral("const"),     QStringLiteral("ref"),
    };
    QStringList found;
    QString rest = body;
    for (;;) {
        bool took = false;
        for (const QString &word : order) {
            if (!rest.startsWith(word + QLatin1Char(' '))) continue;
            found << word;
            rest = rest.mid(word.size() + 1).trimmed();
            took = true;
            break;
        }
        if (!took) break;
    }
    if (found.size() < 2) return body;
    QStringList sorted;
    for (const QString &word : order)
        if (found.contains(word)) sorted << word;
    return sorted.join(QLatin1Char(' ')) + QLatin1Char(' ') + rest;
}

// The generator's own furniture, taken off both sides. Five things, and only
// these five: the preserved region it writes into every file whether or not the
// author had one, the placeholder comment it puts in a class with no methods,
// where it puts its braces, the one spelling of a base class it writes where
// Enforce accepts two, and blank lines, which mean nothing in Enforce and which
// it inserts between methods on its own. Everything else that moves is a loss
// and is counted as one.
QStringList restored(const QString &code)
{
    static const QRegularExpression colonBase(
        QStringLiteral("^(modded\\s+)?class\\s+([A-Za-z_]\\w*)\\s*:\\s*([A-Za-z_]\\w*)\\s*$"));
    const QString placeholder =
        QStringLiteral("// add an Event node and chain nodes from its exec pin");

    QStringList split;
    bool inUserRegion = false;
    for (const QString &raw : code.split(QLatin1Char('\n'))) {
        QString line = raw;
        while (!line.isEmpty() && line.at(line.size() - 1).isSpace()) line.chop(1);
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1String("// >>> user code"))) {
            inUserRegion = true;
            continue;
        }
        if (t.startsWith(QLatin1String("// <<< user code"))) {
            inUserRegion = false;
            continue;
        }
        if (inUserRegion || t.isEmpty() || t == placeholder) continue;
        appendBraceSplit(&split, line);
    }

    QStringList out;
    for (const QString &line : split) {
        int lead = 0;
        while (lead < line.size() && line.at(lead).isSpace()) lead++;
        const QString indent = line.left(lead);
        const QString body = line.mid(lead);

        const auto colon = colonBase.match(body);
        if (colon.hasMatch()) {
            out << indent + colon.captured(1) + QStringLiteral("class ") + colon.captured(2)
                       + QStringLiteral(" extends ") + colon.captured(3);
            continue;
        }
        out << indent + canonicalModifiers(body);
    }
    return out;
}

// What a run of text ends its lines with, said in words. Used to report a
// corpus rather than to decide anything, so it names both cases a single ending
// cannot answer for instead of hiding them behind one: text that mixes the two,
// and text with no line break in it to read an ending off. A class written
// entirely on one line is the second of those, and comparing it against the
// several lines it comes back as would be comparing nothing with something.
QString endingName(const QString &text)
{
    if (!text.contains(QLatin1Char('\n'))) return QStringLiteral("none");
    const QString eol = nodefmt::fileEol(text);
    if (eol == QLatin1String("\r\n")) return QStringLiteral("CRLF");
    if (eol.isEmpty()) return QStringLiteral("mixed");
    return QStringLiteral("LF");
}

// Everything the compiler sees, whitespace inside a line aside. `void Foo( int
// a )` and `void Foo(int a)` declare the same method, and the generator has one
// way of spacing a parameter list. Comments stay in: losing one is a loss.
QStringList codeTokens(const QStringList &lines)
{
    QStringList out;
    for (const Token &t : EnforceLexer::tokenizeAll(lines.join(QLatin1Char('\n')))) {
        if (t.kind == TokenKind::Whitespace) continue;
        out << t.text;
    }
    return out;
}

struct Diff {
    bool exact = false;
    bool same = false;     // same once the generator's furniture is set aside
    bool sameCode = false; // and once whitespace inside a line stops counting
    QString wasLine;       // first line that differs, as the author wrote it
    QString nowLine;       // and as it came back
};

Diff compare(const QString &original, const QString &generated)
{
    Diff d;
    d.exact = original == generated;
    const QStringList a = restored(original);
    const QStringList b = restored(generated);
    d.same = a == b;
    d.sameCode = d.same || codeTokens(a) == codeTokens(b);
    if (d.same) return d;
    // The first line that differs in what it says, not in how it is spaced:
    // a parameter list written with room inside the brackets is the same
    // declaration, and pointing at it would name the wrong reason.
    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const QString x = i < a.size() ? a.at(i) : QString();
        const QString y = i < b.size() ? b.at(i) : QString();
        if (x == y) continue;
        if (!d.sameCode && codeTokens({x}) == codeTokens({y})) continue;
        d.wasLine = x;
        d.nowLine = y;
        break;
    }
    return d;
}

// Why a class came back different, in a form that groups. The line the author
// wrote is what decides it: the generator cannot keep what the graph has no
// field for, and knowing which of those comes up most is the point of counting.
QString reasonFor(const Diff &d)
{
    const QString was = d.wasLine.trimmed();
    const QString now = d.nowLine.trimmed();
    if (was.isEmpty()) return QStringLiteral("the generator added a line");
    if (was.startsWith(QLatin1String("//")) || was.startsWith(QLatin1String("/*"))
        || was.startsWith(QLatin1Char('*')))
        return QStringLiteral("a comment outside a method body");
    if (was.contains(QLatin1String("//")) || was.contains(QLatin1String("/*")))
        return QStringLiteral("a comment on the same line as a declaration");
    if (was.startsWith(QLatin1Char('#'))) return QStringLiteral("a preprocessor directive");
    if (was.endsWith(QLatin1Char(';')) && was.contains(QLatin1Char('(')))
        return QStringLiteral("a method declared with no body");
    if (was.startsWith(QLatin1String("class ")) || was.startsWith(QLatin1String("modded class ")))
        return QStringLiteral("the class header came back differently");
    if (was.startsWith(QLatin1String("enum ")) || was.startsWith(QLatin1String("typedef ")))
        return QStringLiteral("a type declared inside a class");
    if (now.isEmpty()) return QStringLiteral("a line that did not come back");
    if (was.contains(QLatin1Char('(')) && now.contains(QLatin1Char('(')))
        return QStringLiteral("a signature or a call came back differently");
    return QStringLiteral("a declaration came back differently");
}

// ------------------------------------------------------- slicing one class out

// The importer works per class, so the comparison does too: a file may hold
// several classes and code outside all of them, and the generator writes one
// class at a time.
QString classTextOf(const QString &src, const QString &className)
{
    // A scanning copy with comments and string contents blanked, offsets kept.
    QString scan = src;
    const int n = src.size();
    for (int i = 0; i < n;) {
        const QChar c = src.at(i);
        int stop = i;
        if (c == QLatin1Char('/') && i + 1 < n && src.at(i + 1) == QLatin1Char('/')) {
            stop = i;
            while (stop < n && src.at(stop) != QLatin1Char('\n')) stop++;
        } else if (c == QLatin1Char('/') && i + 1 < n && src.at(i + 1) == QLatin1Char('*')) {
            stop = i + 2;
            while (stop + 1 < n
                   && !(src.at(stop) == QLatin1Char('*') && src.at(stop + 1) == QLatin1Char('/')))
                stop++;
            stop = qMin(n, stop + 2);
        } else if (c == QLatin1Char('"')) {
            stop = i + 1;
            while (stop < n && src.at(stop) != QLatin1Char('"')) {
                if (src.at(stop) == QLatin1Char('\\')) stop++;
                stop++;
            }
            stop = qMin(n, stop + 1);
        } else {
            i++;
            continue;
        }
        for (int k = i; k < stop; ++k)
            if (scan.at(k) != QLatin1Char('\n')) scan[k] = QLatin1Char(' ');
        i = stop;
    }

    const QRegularExpression header(
        QStringLiteral("(?:^|[\\s;}])(modded\\s+)?class\\s+") + QRegularExpression::escape(className)
        + QStringLiteral("\\s*(?::\\s*[A-Za-z_]\\w*|extends\\s+[A-Za-z_]\\w*)?\\s*\\{"));
    const auto m = header.match(scan);
    if (!m.hasMatch()) return {};

    int start = m.capturedStart(1) >= 0 ? m.capturedStart(1) : m.capturedStart(0);
    while (start < n && scan.at(start).isSpace()) start++;
    const int open = m.capturedEnd(0) - 1;
    int depth = 0;
    int close = -1;
    for (int i = open; i < n; ++i) {
        if (scan.at(i) == QLatin1Char('{')) depth++;
        else if (scan.at(i) == QLatin1Char('}') && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close < 0) return {};
    int end = close + 1;
    int after = end;
    while (after < n && scan.at(after).isSpace() && scan.at(after) != QLatin1Char('\n')) after++;
    if (after < n && scan.at(after) == QLatin1Char(';')) end = after + 1;
    return src.mid(start, end - start);
}

// ------------------------------------------------------------------ counting

struct Tally {
    int classes = 0;
    int exact = 0;
    int same = 0;
    int sameCode = 0;
    int methods = 0;
    int asNodes = 0;
    int asText = 0;
    // Line endings, counted apart from everything else. A file opened and saved
    // with its endings changed is a diff over every line of it, which is a
    // bigger change than any single method, and it is invisible to the three
    // comparisons above: all of them drop trailing whitespace.
    int endingKept = 0;   // the class came back ending its lines as it did
    int endingLost = 0;   // it did not, which is a loss
    int endingMixed = 0;  // the source used both, so there was no answer to keep
    int endingNone = 0;   // the class was written on one line, so it ends none
    QHash<QString, int> reasons;
    // A few examples of each reason, so a bucket that hides a bug in the reader
    // can be told apart from one the model has no field for.
    QHash<QString, QStringList> samples;
};

// How much of a script stopped being a text box.
void countMethods(const Graph &g, Tally *tally)
{
    for (const GraphFunction &f : g.functions) {
        tally->methods++;
        if (f.hasRawBody) tally->asText++;
        else tally->asNodes++;
    }
    for (const GraphNode &n : g.nodes) {
        const bool isEntry = n.ref.startsWith(QLatin1String("fn.entry."));
        const bool isEvent = n.kind == NodeKind::Event && !isEntry;
        const bool isLifecycle = n.ref == QLatin1String("bi.begin")
                                 || n.ref == QLatin1String("bi.end");
        if (!isEvent && !isLifecycle) continue;
        tally->methods++;
        tally->asNodes++;
    }
}

QString pct(int n, int of)
{
    return of > 0 ? QString::number(100.0 * n / of, 'f', 1) : QStringLiteral("0.0");
}

// Reads one file, generates each class back out, and records what changed.
Diff roundTripClass(const QString &src, const ImportedScript &script, const Catalog &cat,
                    const Builtins &builtins, const Project &project, QString *originalOut,
                    QString *generatedOut = nullptr)
{
    const QString original = classTextOf(src, script.className);
    if (originalOut) *originalOut = original;
    const GenResult gen = generateEnforce(script.graph, cat, builtins, project);
    if (generatedOut) *generatedOut = gen.code;
    return compare(original, gen.code);
}

} // namespace

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    qInstallMessageHandler(relayQtMessage);

    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    reportStream = &out;

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("resources");
    // `-v` prints an example of every loss. A number widens the vanilla sample,
    // which is the only part of the run that takes real time: lowering a body
    // resolves every call in it against the catalogue.
    bool verbose = false;
    int sampleSize = 200;
    for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("-v")) verbose = true;
        bool isNumber = false;
        const int n = arg.toInt(&isNumber);
        if (isNumber && n > 0) sampleSize = n;
    }

    Catalog cat;
    if (!cat.load(root + "/catalog.json")) {
        out << "cannot load catalog.json: " << cat.error() << Qt::endl;
        return 1;
    }
    Builtins builtins;
    Project project;

    // ------------------------------------------------ what the app itself writes
    out << "the four skeletons the New script dialog writes" << Qt::endl;
    {
        struct Skeleton {
            const char *label;
            NewScriptOptions opts;
            // The skeleton for a base with no known override is one comment and
            // nothing else. A graph has no field for a comment outside a method
            // body, so that line cannot come back, and saying it round trips
            // would be saying something untrue.
            bool commentOnly = false;
        };
        const QVector<Skeleton> skeletons = {
            {"new class", {QStringLiteral("SUDO_Probe"), QString(), ScriptKind::NewClass}, false},
            {"modded class with an example override",
             {QStringLiteral("PlayerInfo"), QStringLiteral("MissionServer"),
              ScriptKind::ModdedClass},
             false},
            {"modded class with no example override",
             {QStringLiteral("SUDO_Patch"), QStringLiteral("SUDO_Unknown"),
              ScriptKind::ModdedClass},
             true},
            {"empty", {QStringLiteral("SUDO_Blank"), QString(), ScriptKind::Empty}, false},
        };

        for (const Skeleton &s : skeletons) {
            const QString text = scriptSkeleton(s.opts, &cat);
            const ImportResult r = importEnforceText(text, cat, builtins, project);
            if (text.trimmed().isEmpty()) {
                check(!r.ok && !r.error.isEmpty(),
                      QStringLiteral("%1: refused with a reason rather than an empty graph")
                          .arg(QString::fromUtf8(s.label)));
                continue;
            }
            if (!r.ok || r.scripts.size() != 1) {
                check(false, QStringLiteral("%1: imported (%2)")
                                 .arg(QString::fromUtf8(s.label), r.error));
                continue;
            }
            QString original;
            const Diff d = roundTripClass(text, r.scripts.first(), cat, builtins, project,
                                          &original);
            if (s.commentOnly) {
                const Graph &g = r.scripts.first().graph;
                check(g.modded && g.className == s.opts.baseClass && g.nodes.isEmpty()
                          && g.functions.isEmpty() && g.variables.isEmpty(),
                      QStringLiteral("%1: read as an empty modded class")
                          .arg(QString::fromUtf8(s.label)));
                out << "         its one line is a comment, and a graph has nowhere to keep a"
                    << Qt::endl
                    << "         comment outside a method body, so it does not come back."
                    << Qt::endl;
                continue;
            }
            check(d.same, QStringLiteral("%1: round trips%2")
                              .arg(QString::fromUtf8(s.label),
                                   d.exact ? QStringLiteral(", byte for byte") : QString()));
            if (!d.same) {
                out << "         was: " << d.wasLine << Qt::endl;
                out << "         now: " << d.nowLine << Qt::endl;
            }
        }

        // The shape the user reported: a script the app wrote, clicked in the
        // Mod Explorer, has to arrive as an event node rather than as text.
        const NewScriptOptions opts = {QStringLiteral("PlayerInfo"),
                                       QStringLiteral("MissionServer"), ScriptKind::ModdedClass};
        const ImportResult r = importEnforceText(scriptSkeleton(opts, &cat), cat, builtins,
                                                 project);
        if (r.ok && r.scripts.size() == 1) {
            const Graph &g = r.scripts.first().graph;
            check(g.modded && g.className == QLatin1String("MissionServer"),
                  QStringLiteral("modded class MissionServer is read as a modded class"));
            int events = 0;
            bool superKept = false;
            for (const GraphNode &n : g.nodes) {
                if (n.kind != NodeKind::Event) continue;
                events++;
                superKept = !n.opts.contains(QStringLiteral("noSuper"));
            }
            check(events == 1, QStringLiteral("OnInit arrives as one event node (%1)").arg(events));
            check(superKept, QStringLiteral("the super call is the node's, not a second one"));
            check(g.functions.isEmpty(),
                  QStringLiteral("nothing was left as a text body (%1)").arg(g.functions.size()));
        }
    }

    // ------------------------------------------- a file this app wrote itself
    // The claim the whole feature rests on: open a script the tool generated
    // and save it again, and the file on disk does not move. Nothing is set
    // aside here. The two files are compared character for character.
    out << Qt::endl << "generate, open, save again" << Qt::endl;
    for (const QString &name : {QStringLiteral("SUDO_Link"), QStringLiteral("Showcase")}) {
        Project saved;
        QString err;
        if (!loadProject(root + QLatin1Char('/') + name + QStringLiteral(".sdzn"), saved, &err)) {
            out << "       (" << name << ".sdzn not present: " << err << ")" << Qt::endl;
            continue;
        }
        int same = 0;
        int moved = 0;
        for (const ScriptEntry &s : saved.scripts) {
            const QString first = generateEnforce(s.graph, cat, builtins, saved).code;
            const ImportResult r = importEnforceText(first, cat, builtins, saved);
            if (!r.ok || r.scripts.size() != 1) {
                moved++;
                if (moved <= 3)
                    out << "       ---- " << s.name << ": " << r.error << Qt::endl;
                continue;
            }
            const QString second =
                generateEnforce(r.scripts.first().graph, cat, builtins, saved, first).code;
            if (first == second) {
                same++;
                continue;
            }
            moved++;
            if (moved <= 3) {
                const QStringList a = first.split(QLatin1Char('\n'));
                const QStringList b = second.split(QLatin1Char('\n'));
                for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
                    const QString x = i < a.size() ? a.at(i) : QString();
                    const QString y = i < b.size() ? b.at(i) : QString();
                    if (x == y) continue;
                    out << "       ---- " << s.name << " line " << i + 1 << Qt::endl
                        << "            was: " << x << Qt::endl
                        << "            now: " << y << Qt::endl;
                    break;
                }
            }
        }
        check(moved == 0, QStringLiteral("%1: %2 of %3 scripts come back byte for byte")
                              .arg(name)
                              .arg(same)
                              .arg(saved.scripts.size()));

        // The same claim for a file written on Windows, which is what three
        // quarters of the installed mods are. Nothing is set aside here either:
        // open a file whose every line ends with a carriage return and save it,
        // and every one of those carriage returns has to still be there.
        int keptCrlf = 0;
        int movedCrlf = 0;
        for (const ScriptEntry &s : saved.scripts) {
            QString first = generateEnforce(s.graph, cat, builtins, saved).code;
            first.replace(QLatin1String("\n"), QLatin1String("\r\n"));
            const ImportResult r = importEnforceText(first, cat, builtins, saved);
            if (!r.ok || r.scripts.size() != 1) {
                movedCrlf++;
                continue;
            }
            const QString second =
                generateEnforce(r.scripts.first().graph, cat, builtins, saved, first).code;
            if (first == second) {
                keptCrlf++;
                continue;
            }
            movedCrlf++;
            if (movedCrlf <= 3) {
                const QStringList a = first.split(QLatin1Char('\n'));
                const QStringList b = second.split(QLatin1Char('\n'));
                for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
                    const QString x = i < a.size() ? a.at(i) : QString();
                    const QString y = i < b.size() ? b.at(i) : QString();
                    if (x == y) continue;
                    out << "       ---- " << s.name << " line " << i + 1 << Qt::endl
                        << "            was: " << QString(x).replace('\r', "\\r") << Qt::endl
                        << "            now: " << QString(y).replace('\r', "\\r") << Qt::endl;
                    break;
                }
            }
        }
        check(movedCrlf == 0,
              QStringLiteral("%1: %2 of %3 come back byte for byte when the file is written "
                             "with CRLF endings")
                  .arg(name)
                  .arg(keptCrlf)
                  .arg(saved.scripts.size()));
    }

    // ------------------------------------------------------ the mod template
    out << Qt::endl << "resources/mod-template" << Qt::endl;
    {
        QStringList files;
        QDirIterator it(root + "/mod-template", QStringList{QStringLiteral("*.c")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        std::sort(files.begin(), files.end());
        check(!files.isEmpty(), QStringLiteral("%1 template scripts found").arg(files.size()));

        for (const QString &path : files) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            // The bytes as they are. Taking the carriage returns off here would
            // hide the one thing a mod author sees first, which is the whole
            // file moving because its line endings changed.
            const QString src = QString::fromLatin1(f.readAll());
            f.close();

            const ImportResult r = importEnforceFile(path, cat, builtins, project);
            const QString name = QDir(root).relativeFilePath(path);
            if (!r.ok) {
                check(false, QStringLiteral("%1: %2").arg(name, r.error));
                continue;
            }
            for (const ImportedScript &s : r.scripts) {
                QString original;
                QString generated;
                const Diff d = roundTripClass(src, s, cat, builtins, project, &original,
                                              &generated);
                check(d.same, QStringLiteral("%1: %2 round trips%3")
                                  .arg(name, s.className,
                                       d.exact ? QStringLiteral(", byte for byte") : QString()));
                if (!d.same) {
                    out << "         was: " << d.wasLine << Qt::endl;
                    out << "         now: " << d.nowLine << Qt::endl;
                }
                if (endingName(original) != QLatin1String("none"))
                    check(endingName(generated) == endingName(original),
                          QStringLiteral("%1: %2 keeps its %3 line endings")
                              .arg(name, s.className, endingName(original)));
            }
            // The global functions in an init.c are not part of any class, and
            // losing them would break the mission.
            if (!r.preamble.isEmpty())
                check(r.preamble.contains(QLatin1String("void main()")),
                      QStringLiteral("%1: code outside the class is kept").arg(name));
        }
    }

    // ---------------------------------------------------------------- corpus
    out << Qt::endl << "hand-written corpus" << Qt::endl;
    {
        enum class Want { Same, Different };
        struct Case {
            const char *name;
            const char *code;
            Want want;
        };
        const QVector<Case> cases = {
            {"a class with no base",
             "class SUDO_Thing\n{\n|int m_Count;\n}\n", Want::Same},
            {"a class with a base",
             "class SUDO_Thing extends ItemBase\n{\n|int m_Count = 3;\n}\n", Want::Same},
            {"a modded class",
             "modded class ItemBase\n{\n|int m_Count;\n}\n", Want::Same},
            {"a base written with a colon",
             "class SUDO_Thing: ItemBase\n{\n|int m_Count;\n}\n", Want::Same},
            {"every member modifier",
             "class SUDO_Thing extends ItemBase\n{\n"
             "|private int m_A;\n"
             "|protected string m_B;\n"
             "|static int m_C;\n"
             "|const int m_D = 4;\n"
             "|private static const float m_E = 1.5;\n"
             "}\n",
             Want::Same},
            {"a member with a string default",
             "class SUDO_Thing extends ItemBase\n{\n|string m_Name = \"link\";\n}\n", Want::Same},
            {"a managed member declared with ref",
             "class SUDO_Thing extends ItemBase\n{\n|ref Timer m_Timer;\n}\n", Want::Same},
            // The generator would put `ref` in front of a Timer on its own, so
            // the author leaving it off has to survive as an explicit decision.
            {"a managed member declared without ref",
             "class SUDO_Thing extends ItemBase\n{\n|Timer m_Timer;\n}\n", Want::Same},
            {"an entity member stays unref'd",
             "class SUDO_Thing extends ItemBase\n{\n|EntityAI m_Owner;\n}\n", Want::Same},
            {"an event that calls super",
             "modded class ItemBase\n{\n|override void EEInit()\n|{\n||super.EEInit();\n|}\n}\n",
             Want::Same},
            {"an event that does not call super",
             "modded class ItemBase\n{\n|override void EEInit()\n|{\n||SetQuantity(1);\n|}\n}\n",
             Want::Same},
            {"an event with parameters",
             "modded class ItemBase\n{\n|override void EEItemAttached(EntityAI item, string slot_name)"
             "\n|{\n||super.EEItemAttached(item, slot_name);\n|}\n}\n",
             Want::Same},
            {"a helper function",
             "class SUDO_Thing extends ItemBase\n{\n|void Reset(int count)\n|{\n||SetQuantity(count);"
             "\n|}\n}\n",
             Want::Same},
            {"a helper that returns a value",
             "class SUDO_Thing extends ItemBase\n{\n|int Doubled(int count)\n|{\n||return count;"
             "\n|}\n}\n",
             Want::Same},
            {"a static helper",
             "class SUDO_Thing extends ItemBase\n{\n|static int Zero()\n|{\n||return 0;\n|}\n}\n",
             Want::Same},
            {"a constructor and a destructor",
             "class SUDO_Thing\n{\n|void SUDO_Thing()\n|{\n|}\n\n|void ~SUDO_Thing()\n|{\n|}\n}\n",
             Want::Same},
            {"a constructor with a body",
             "class SUDO_Thing extends ItemBase\n{\n|void SUDO_Thing()\n|{\n||SetQuantity(1);\n|}\n}\n",
             Want::Same},
            {"a body with a branch in it",
             "modded class ItemBase\n{\n|override void EEInit()\n|{\n||super.EEInit();\n"
             "||if (GetQuantity() > 0)\n||{\n|||SetQuantity(1);\n||}\n|}\n}\n",
             Want::Same},
            {"a body the graph keeps as text",
             "modded class ItemBase\n{\n|override void EEInit()\n|{\n||super.EEInit();\n"
             "||switch (GetQuantity())\n||{\n|||case 1:\n||||SetQuantity(2);\n||||break;\n||}\n|}\n}\n",
             Want::Same},
            {"members and methods together",
             "modded class ItemBase\n{\n|int m_Count;\n|ref Timer m_Timer;\n\n"
             "|override void EEInit()\n|{\n||super.EEInit();\n||m_Count = 1;\n|}\n}\n",
             Want::Same},
            {"two classes in one file",
             "class SUDO_A\n{\n|int m_A;\n}\n\nclass SUDO_B extends SUDO_A\n{\n|int m_B;\n}\n",
             Want::Same},
            {"a member declared with an array size",
             "class SUDO_Thing\n{\n|int m_Sizes[4];\n}\n", Want::Same},
            {"a member with a brace initialiser",
             "class SUDO_Thing\n{\n|static const int m_Sizes[] = {1, 2, 3};\n}\n", Want::Same},
            // The word is the older spelling of the flag the model carries, and
            // the generator writes that flag as `ref`.
            {"a member declared with autoptr",
             "class SUDO_Thing extends ItemBase\n{\n|autoptr Timer m_Timer;\n}\n", Want::Same},
            // A default value written as a string used to come back with its
            // contents blanked, because the scan reads a masked copy.
            {"a method with default parameters",
             "class SUDO_Thing extends ItemBase\n"
             "{\n|void Place(string name = \"\", vector at = \"0 0 0\", int flags = 0)\n|{\n|}\n}\n",
             Want::Same},
            // `ref` in front of a return type belongs to the type, not to the
            // method, and reading it as a modifier dropped it.
            {"a method returning a ref type",
             "class SUDO_Thing extends ItemBase\n{\n|ref array<string> GetPaths()\n|{\n"
             "||return null;\n|}\n}\n",
             Want::Same},
            {"a method with an out parameter",
             "class SUDO_Thing extends ItemBase\n{\n|void Read(out int count)\n|{\n|}\n}\n",
             Want::Same},
        };

        for (const Case &c : cases) {
            const QString text = enf(c.code);
            const ImportResult r = importEnforceText(text, cat, builtins, project);
            if (!r.ok || r.scripts.isEmpty()) {
                check(false, QStringLiteral("%1: imported (%2)")
                                 .arg(QString::fromUtf8(c.name), r.error));
                continue;
            }
            bool allSame = true;
            Diff worst;
            for (const ImportedScript &s : r.scripts) {
                QString original;
                const Diff d = roundTripClass(text, s, cat, builtins, project, &original);
                if (!d.same) {
                    allSame = false;
                    worst = d;
                }
            }
            const bool pass = c.want == Want::Same ? allSame : !allSame;
            check(pass, QString::fromUtf8(c.name));
            if (!pass && c.want == Want::Same) {
                out << "         was: " << worst.wasLine << Qt::endl;
                out << "         now: " << worst.nowLine << Qt::endl;
            }
        }

        // Reading a declaration is not enough on its own: the fields the model
        // carries have to come back with the right values.
        const ImportResult r = importEnforceText(
            enf("class SUDO_Thing extends ItemBase\n{\n"
                "|private static const int m_Limit = 4;\n"
                "|Timer m_Timer;\n"
                "|ref array<ref ItemBase> m_Items;\n"
                "}\n"),
            cat, builtins, project);
        if (r.ok && !r.scripts.isEmpty()) {
            const Graph &g = r.scripts.first().graph;
            check(g.className == QLatin1String("SUDO_Thing")
                      && g.baseClass == QLatin1String("ItemBase") && !g.modded,
                  QStringLiteral("the class header is read"));
            check(g.variables.size() == 3,
                  QStringLiteral("%1 members read").arg(g.variables.size()));
            if (g.variables.size() == 3) {
                const GraphVariable &limit = g.variables.at(0);
                check(limit.isPrivate && limit.isStatic && limit.isConst
                          && limit.type == QLatin1String("int")
                          && limit.name == QLatin1String("m_Limit")
                          && limit.def == QLatin1String("4"),
                      QStringLiteral("modifiers, type, name and initialiser"));
                const GraphVariable &timer = g.variables.at(1);
                check(timer.hasRef && !timer.isRef,
                      QStringLiteral("a managed member with no ref keeps an explicit false"));
                const GraphVariable &items = g.variables.at(2);
                check(items.hasRef && items.isRef
                          && items.type == QLatin1String("array<ref ItemBase>"),
                      QStringLiteral("ref and a generic type survive"));
            }
        }

        // The point of the whole feature: a body the tool can model has to
        // arrive as nodes, not as a text box with the code in it. The graph is
        // only taken when it regenerates the body it came from, so the shape
        // here is one the generator writes the same way the author did.
        const ImportResult modelled = importEnforceText(
            enf("modded class ItemBase\n{\n|int m_Count;\n\n|override void EEInit()\n|{\n"
                "||super.EEInit();\n||if (m_Count)\n||{\n|||m_Count = 1;\n||}\n|}\n}\n"),
            cat, builtins, project);
        if (modelled.ok && !modelled.scripts.isEmpty()) {
            const Graph &g = modelled.scripts.first().graph;
            int branches = 0;
            int sets = 0;
            int raws = 0;
            for (const GraphNode &n : g.nodes) {
                if (n.ref == QLatin1String("bi.branch")) branches++;
                if (n.kind == NodeKind::VarSet) sets++;
                if (n.ref == QLatin1String("bi.raw")) raws++;
            }
            check(g.functions.isEmpty() && branches == 1 && sets == 1 && raws == 0,
                  QStringLiteral("the body arrives as nodes (%1 branch, %2 set, %3 raw, %4 text "
                                 "bodies)")
                      .arg(branches)
                      .arg(sets)
                      .arg(raws)
                      .arg(g.functions.size()));
        }

        // An annotation has no field in the model. Losing it is a change to the
        // file, and losing the member it decorates as well would be worse.
        const ImportResult annotated = importEnforceText(
            enf("class SUDO_Thing\n{\n|[Attribute(\"1\", \"flags\", \"Flags\")]\n|int m_Flags;\n}\n"),
            cat, builtins, project);
        check(annotated.ok && !annotated.scripts.isEmpty()
                  && annotated.scripts.first().graph.variables.size() == 1,
              QStringLiteral("an annotated member is kept, and the annotation is reported"));

        // Text outside a class has nowhere to live in a graph, so it comes back
        // to the caller rather than being dropped.
        const ImportResult globals = importEnforceText(
            enf("enum SUDO_Mode\n{\n|Off,\n|On,\n}\n\nclass SUDO_Thing\n{\n|int m_A;\n}\n"
                "\nvoid SUDO_Helper()\n{\n}\n"),
            cat, builtins, project);
        check(globals.ok && globals.scripts.size() == 1,
              QStringLiteral("a file with an enum and a global function still opens"));
        check(globals.preamble.contains(QLatin1String("enum SUDO_Mode"))
                  && globals.preamble.contains(QLatin1String("void SUDO_Helper()")),
              QStringLiteral("the enum and the global function are kept"));

        // A file the app generated carries a user region. Its contents belong
        // to the author, and reading them as members would double them.
        const QString withRegion =
            enf("class SUDO_Thing\n{\n|int m_A;\n\n|void Reset()\n|{\n|}\n\n") + QLatin1Char('\t')
            + USER_BEGIN + enf("\n|int m_Hidden;\n|void Helper()\n|{\n|}\n") + QLatin1Char('\t')
            + USER_END + QStringLiteral("\n};\n");
        const ImportResult region = importEnforceText(withRegion, cat, builtins, project);
        if (region.ok && !region.scripts.isEmpty()) {
            const Graph &g = region.scripts.first().graph;
            check(g.variables.size() == 1 && g.functions.size() == 1,
                  QStringLiteral("the preserved region is not read as declarations (%1 members, "
                                 "%2 methods)")
                      .arg(g.variables.size())
                      .arg(g.functions.size()));
            const GenResult gen = generateEnforce(g, cat, builtins, project, withRegion);
            check(gen.code.count(QLatin1String("m_Hidden")) == 1,
                  QStringLiteral("what the author wrote in it comes back once"));
        }
    }

    // ------------------------------------------------------------ line endings
    //
    // What the file was written with is the file's own, not any one method's,
    // and it is put back over the whole text. These are the three cases: a file
    // that ends every line the same way, a carriage return that ends no line at
    // all, and a file that uses both.
    out << Qt::endl << "line endings" << Qt::endl;
    {
        const QString lf =
            enf("modded class ItemBase\n{\n|override void EEInit()\n|{\n||super.EEInit();\n"
                "||SetQuantity(1);\n|}\n}\n");
        QString crlf = lf;
        crlf.replace(QLatin1String("\n"), QLatin1String("\r\n"));

        const ImportResult r = importEnforceText(crlf, cat, builtins, project);
        check(r.ok && r.scripts.size() == 1 && r.eol == QLatin1String("\r\n"),
              QStringLiteral("a file written with CRLF is read as one"));
        if (r.ok && r.scripts.size() == 1) {
            const QString code = generateEnforce(r.scripts.first().graph, cat, builtins,
                                                 project).code;
            int bare = 0;
            for (int i = 0; i < code.size(); ++i)
                if (code.at(i) == QLatin1Char('\n')
                    && (i == 0 || code.at(i - 1) != QLatin1Char('\r')))
                    bare++;
            check(bare == 0,
                  QStringLiteral("and comes back with no line left on a bare newline (%1)")
                      .arg(bare));
            // The header, the members and the preserved region are the file's,
            // not any method's, so a rule that spoke for method bodies alone
            // would leave exactly these behind.
            check(code.contains(QStringLiteral("modded class ItemBase\r\n"))
                      && code.contains(USER_END + QStringLiteral("\r\n")),
                  QStringLiteral("the class header and the preserved region take it too"));
        }

        // The one that used to be deleted outright. A carriage return with no
        // newline behind it is a byte of the line it sits on, and inside a
        // string literal it is a byte the Enforce compiler reads, so removing
        // it changes what the file compiles to.
        const QString withCr =
            enf("modded class ItemBase\n{\n|override void EEInit()\n|{\n||super.EEInit();\n"
                "||Print(\"a\rb\");\n|}\n}\n");
        const ImportResult lone = importEnforceText(withCr, cat, builtins, project);
        check(lone.ok && lone.scripts.size() == 1, QStringLiteral("a lone carriage return "
                                                                  "does not stop the file "
                                                                  "opening"));
        if (lone.ok && lone.scripts.size() == 1) {
            const QString code = generateEnforce(lone.scripts.first().graph, cat, builtins,
                                                 project).code;
            check(code.contains(QStringLiteral("Print(\"a\rb\");")),
                  QStringLiteral("a carriage return inside a string literal comes back where "
                                 "the author put it"));
            check(lone.eol == QLatin1String("\n"),
                  QStringLiteral("and does not make the file look like a CRLF one"));
        }

        // A file that uses both. There is no single ending that puts it back as
        // it was, and picking the commoner one would rewrite every line
        // carrying the other, so nothing is restored and the file is written
        // with bare newlines, which is one of the two endings it already had.
        // What must not happen is that it goes past the user in silence.
        QString mixed = crlf;
        mixed.replace(QStringLiteral("SetQuantity(1);\r\n"), QStringLiteral("SetQuantity(1);\n"));
        const ImportResult both = importEnforceText(mixed, cat, builtins, project);
        check(both.ok && both.eol.isEmpty(),
              QStringLiteral("a file that mixes its endings is read, and no ending is claimed "
                             "for it"));
        if (both.ok && both.scripts.size() == 1) {
            check(both.scripts.first().graph.eol.isEmpty(),
                  QStringLiteral("the graph records that there was no answer"));
            const QString code = generateEnforce(both.scripts.first().graph, cat, builtins,
                                                 project).code;
            check(!code.contains(QLatin1Char('\r')),
                  QStringLiteral("it is written with bare newlines, which is one of the two "
                                 "the source held"));
            bool said = false;
            for (const QString &n : both.notes)
                if (n.contains(QStringLiteral("both kinds of line ending"))) said = true;
            check(said, QStringLiteral("and the user is told, rather than finding out from "
                                       "the diff"));
        }

        // The one caller the ending on the result is there for is the one that
        // stitches a preamble in front of a generated class, and a file made of
        // nothing but preamble is turned down. It still has to say what it was
        // written with, or that caller has nowhere to read it from.
        const ImportResult noClass = importEnforceText(
            QStringLiteral("enum ESudo\r\n{\r\n\tOne,\r\n\tTwo\r\n};\r\n"), cat, builtins,
            project);
        check(!noClass.ok && noClass.eol == QLatin1String("\r\n"),
              QStringLiteral("a file with no class in it still reports the ending it was "
                             "written with"));
    }

    // ---------------------------------------------------------- real vanilla
    out << Qt::endl << "vanilla source (P:\\scripts)" << Qt::endl;
    {
        const QString scripts = QStringLiteral("P:/scripts");
        if (!QDir(scripts).exists()) {
            out << "       (P:\\scripts is not mounted, so this corpus was skipped)" << Qt::endl;
        } else {
            QStringList files;
            QDirIterator it(scripts, QStringList{QStringLiteral("*.c")}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) files << it.next();
            std::sort(files.begin(), files.end());

            // An even sample rather than the first N, so the answer is not one
            // folder's worth of code.
            const int want = sampleSize;
            QStringList sample;
            const int step = qMax(1, files.size() / want);
            for (int i = 0; i < files.size() && sample.size() < want; i += step)
                sample << files.at(i);

            Tally tally;
            int refused = 0;
            for (const QString &path : sample) {
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) continue;
                // 2765 of these 2810 files are written with CRLF endings. This
                // used to take the carriage returns off before importing, which
                // meant the corpus could not see the importer deleting them:
                // every file it opened came back LF and the comparison, which
                // drops trailing whitespace, called that no change at all.
                const QString src = QString::fromLatin1(f.readAll());
                f.close();

                const ImportResult r = importEnforceText(src, cat, builtins, project);
                if (!r.ok) {
                    refused++;
                    continue;
                }
                for (const ImportedScript &s : r.scripts) {
                    tally.classes++;
                    countMethods(s.graph, &tally);
                    QString original;
                    QString generated;
                    const Diff d = roundTripClass(src, s, cat, builtins, project, &original,
                                                  &generated);
                    if (original.isEmpty()) {
                        tally.reasons[QStringLiteral("the class could not be cut out again")]++;
                        continue;
                    }
                    const QString was = endingName(original);
                    if (was == QLatin1String("none")) tally.endingNone++;
                    else if (was == QLatin1String("mixed")) tally.endingMixed++;
                    else if (endingName(generated) == was) tally.endingKept++;
                    else {
                        tally.endingLost++;
                        if (tally.endingLost <= 3)
                            out << "       ---- " << QDir(scripts).relativeFilePath(path)
                                << " / " << s.className << ": was "
                                << endingName(original) << ", came back "
                                << endingName(generated) << Qt::endl;
                    }
                    if (d.exact) tally.exact++;
                    if (d.same) tally.same++;
                    if (d.sameCode) {
                        // Nothing was lost, so there is no reason to record.
                        tally.sameCode++;
                        continue;
                    }
                    const QString reason = reasonFor(d);
                    tally.reasons[reason]++;
                    if (tally.samples[reason].size() < 3)
                        tally.samples[reason]
                            << QDir(scripts).relativeFilePath(path) + QStringLiteral(" / ")
                                   + s.className + QStringLiteral("\n            was: ")
                                   + d.wasLine + QStringLiteral("\n            now: ")
                                   + d.nowLine;
                }
            }

            out << "       files sampled:         " << sample.size() << " of " << files.size()
                << Qt::endl;
            out << "       files with no class:   " << refused << Qt::endl;
            out << "       classes:               " << tally.classes << Qt::endl;
            out << "       ROUND TRIPS EXACTLY:   " << tally.exact << " of " << tally.classes
                << "  (" << pct(tally.exact, tally.classes) << "%)" << Qt::endl;
            out << "       ROUND TRIPS RESTORED:  " << tally.same << " of " << tally.classes
                << "  (" << pct(tally.same, tally.classes) << "%)" << Qt::endl;
            out << "       SAME CODE:             " << tally.sameCode << " of " << tally.classes
                << "  (" << pct(tally.sameCode, tally.classes) << "%)" << Qt::endl;
            out << "       LINE ENDINGS KEPT:     " << tally.endingKept << " of "
                << tally.endingKept + tally.endingLost << "  ("
                << pct(tally.endingKept, tally.endingKept + tally.endingLost) << "%)"
                << Qt::endl;
            out << "       endings not restored:  " << tally.endingLost << Qt::endl;
            out << "       sources that mix them: " << tally.endingMixed
                << "  (written back with bare newlines, and said so)" << Qt::endl;
            out << "       classes on one line:   " << tally.endingNone
                << "  (no line break to read an ending off)" << Qt::endl;
            out << "       methods:               " << tally.methods << Qt::endl;
            out << "       as nodes:              " << tally.asNodes << "  ("
                << pct(tally.asNodes, tally.methods) << "%)" << Qt::endl;
            out << "       kept as text:          " << tally.asText << "  ("
                << pct(tally.asText, tally.methods) << "%)" << Qt::endl;

            out << Qt::endl << "       what the rest lost" << Qt::endl;
            QVector<QPair<int, QString>> ranked;
            for (auto i = tally.reasons.constBegin(); i != tally.reasons.constEnd(); ++i)
                ranked.append({i.value(), i.key()});
            std::sort(ranked.begin(), ranked.end(),
                      [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                          return a.first > b.first;
                      });
            for (int i = 0; i < ranked.size() && i < 12; ++i)
                out << "       " << ranked.at(i).first << "  " << ranked.at(i).second << Qt::endl;

            if (verbose) {
                for (int i = 0; i < ranked.size(); ++i) {
                    out << Qt::endl << "       " << ranked.at(i).second << Qt::endl;
                    for (const QString &s : tally.samples.value(ranked.at(i).second))
                        out << "       ---- " << s << Qt::endl;
                }
            }

            check(tally.classes > sample.size(),
                  QStringLiteral("%1 vanilla classes read").arg(tally.classes));
            check(refused * 10 < sample.size(),
                  QStringLiteral("%1 of %2 sampled files hold no class at all")
                      .arg(refused)
                      .arg(sample.size()));
            // Floors, with room under the measured numbers so a DayZ update
            // moving the corpus around does not fail the run on its own. They
            // are here to catch a reader that starts dropping things again.
            check(tally.sameCode * 100 >= tally.classes * 65,
                  QStringLiteral("at least 65% of classes come back saying the same thing (%1%)")
                      .arg(pct(tally.sameCode, tally.classes)));
            check(tally.same * 100 >= tally.classes * 50,
                  QStringLiteral("at least 50% come back line for line (%1%)")
                      .arg(pct(tally.same, tally.classes)));
            check(tally.asNodes * 100 >= tally.methods * 25,
                  QStringLiteral("at least 25% of methods stop being a text box (%1%)")
                      .arg(pct(tally.asNodes, tally.methods)));
            // Not a floor with room under it. A file whose ending changed is
            // every line of that file changed, so one is a failure.
            check(tally.endingLost == 0,
                  QStringLiteral("every class whose file ends its lines one way comes back "
                                 "ending them the same way (%1 did not)")
                      .arg(tally.endingLost));
            check(tally.endingKept > 0,
                  QStringLiteral("%1 classes were read with their line endings on")
                      .arg(tally.endingKept));
        }
    }

    out << Qt::endl;
    check(qtMessages == 0,
          QStringLiteral("Qt said nothing during the run (%1 messages)").arg(qtMessages));

    out << Qt::endl
        << (failures == 0 ? QStringLiteral("ALL IMPORT TESTS PASSED")
                          : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;

    out.flush();
    reportStream = nullptr;
    return failures == 0 ? 0 : 1;
}
