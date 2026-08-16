// Enforce code to nodes, and back again.
//
// The bar is the round trip: lower a block of Enforce into nodes, generate
// Enforce from those nodes, and the two must say the same thing. Anything that
// comes back different is a lowering bug, not a formatting difference, so the
// comparison only forgives whitespace, redundant parentheses and the names of
// locals the graph turned into wires.
//
// It also runs over every Raw node in the user's own project and prints how
// many of them stop being text, which is the number the feature exists for.
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "enforce/ast.h"
#include "enforce/lexer.h"
#include "enforce/lower.h"
#include "graph.h"
#include "project.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <cstdio>

static int failures = 0;
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
// The one that bites here is the font database: it needs a QGuiApplication, and
// a headless test has only a QCoreApplication, so any layout path that measures
// text kills the run in the middle of counting.
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

    reportLine(QStringLiteral("       report cut short by the fatal above, after %1 "
                              "failed checks")
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

// The lowering turns `x++` into `x = x + 1` and `x += y` into `x = x + y`,
// which is the same statement said the way a graph can hold it. Both sides get
// the long form so the comparison is about meaning rather than spelling.
QStringList expandCompound(const QStringList &in)
{
    QStringList out;
    for (int i = 0; i < in.size(); ++i) {
        const QString &t = in.at(i);
        const bool step = t == QLatin1String("++") || t == QLatin1String("--");
        const bool compound = t.size() == 2 && t.endsWith(QLatin1Char('='))
                              && QLatin1String("+-*/%").contains(t.at(0));
        if (!out.isEmpty() && step) {
            const QString name = out.last();
            out << QStringLiteral("=") << name
                << (t == QLatin1String("++") ? QStringLiteral("+") : QStringLiteral("-"))
                << QStringLiteral("1");
            continue;
        }
        if (!out.isEmpty() && compound) {
            const QString name = out.last();
            out << QStringLiteral("=") << name << t.left(1);
            continue;
        }
        out << t;
    }
    return out;
}

// Tokens that carry meaning. Whitespace, comments and parentheses are dropped:
// the generator parenthesises every operator, the author did not, and neither
// choice changes the program.
QStringList meaningfulTokens(const QString &code)
{
    QStringList out;
    for (const Token &t : EnforceLexer::tokenizeAll(code)) {
        if (t.kind == TokenKind::Whitespace || t.kind == TokenKind::Comment) continue;
        if (t.text == QLatin1String("(") || t.text == QLatin1String(")")) continue;
        out << t.text;
    }
    return expandCompound(out);
}

bool isIdentifier(const QString &t)
{
    if (t.isEmpty()) return false;
    if (!t.at(0).isLetter() && t.at(0) != QLatin1Char('_')) return false;
    for (const QChar c : t)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) return false;
    return true;
}

// Names the generator invents for values that used to be locals. A local wired
// straight from its producer has no name of its own any more, so the name in
// the regenerated code is one of these.
bool isGeneratedTemp(const QString &t)
{
    static const QRegularExpression shape(
        QStringLiteral("^(v|out|io|i|idx|item|cast|obj|ent)\\d+$"));
    return shape.match(t).hasMatch();
}

// The generator gives every value produced by a call a local of its own, so
// `m_X = Call();` comes back as two lines. Folding a local that is used exactly
// once back into its use undoes that, and leaves anything with a real second
// reader (a loop counter, a cast result) alone.
QString inlineTemps(const QString &code)
{
    static const QRegularExpression decl(
        QStringLiteral(R"(^[ \t]*(?:ref[ \t]+)?[A-Za-z_][A-Za-z0-9_<>,: \t]*?)"
                       R"([ \t]((?:v|out|io|i|idx|item|cast|obj|ent)\d+)[ \t]*=)"
                       R"([ \t]*(.+);[ \t]*$)"));
    QString text = code;
    for (int pass = 0; pass < 8; ++pass) {
        QStringList lines = text.split(QLatin1Char('\n'));
        bool changed = false;
        for (int i = 0; i < lines.size() && !changed; ++i) {
            const auto m = decl.match(lines.at(i));
            if (!m.hasMatch()) continue;
            const QString name = m.captured(1);
            const QString value = m.captured(2);
            const QRegularExpression use(QStringLiteral("\\b") + name
                                         + QStringLiteral("\\b"));
            int uses = 0;
            for (int j = 0; j < lines.size(); ++j) {
                if (j == i) continue;
                auto it = use.globalMatch(lines.at(j));
                while (it.hasNext()) {
                    it.next();
                    uses++;
                }
            }
            if (uses != 1) continue;
            for (int j = 0; j < lines.size(); ++j) {
                if (j == i) continue;
                lines[j].replace(use, value);
            }
            lines.removeAt(i);
            text = lines.join(QLatin1Char('\n'));
            changed = true;
        }
        if (!changed) break;
    }
    return text;
}

// A switch becomes a chain of Branch nodes, which is the same program written
// a different way. The file is expected to change for one of these, so it is
// counted apart from a change nothing asked for.
bool rewrittenOnPurpose(const QString &code)
{
    for (const Token &t : EnforceLexer::tokenizeAll(code))
        if (t.text == QLatin1String("switch")) return true;
    return false;
}

enum class Match { Exact, Renamed, Different };

// Exact means token for token. Renamed means the only differences are locals
// the graph replaced with a wire, matched one to one in both directions so two
// different names can never collapse into one.
Match compare(const QString &a, const QString &b)
{
    const QStringList ta = meaningfulTokens(inlineTemps(a));
    const QStringList tb = meaningfulTokens(inlineTemps(b));
    if (ta == tb) return Match::Exact;
    if (ta.size() != tb.size()) return Match::Different;

    QHash<QString, QString> forward;
    QHash<QString, QString> back;
    for (int i = 0; i < ta.size(); ++i) {
        if (ta.at(i) == tb.at(i)) continue;
        if (!isIdentifier(ta.at(i)) || !isGeneratedTemp(tb.at(i))) return Match::Different;
        if (forward.contains(ta.at(i)) && forward.value(ta.at(i)) != tb.at(i))
            return Match::Different;
        if (back.contains(tb.at(i)) && back.value(tb.at(i)) != ta.at(i))
            return Match::Different;
        forward.insert(ta.at(i), tb.at(i));
        back.insert(tb.at(i), ta.at(i));
    }
    return Match::Renamed;
}

// A graph holding nothing but one event whose chain is the lowered code, so the
// generator has something to walk.
Graph graphFor(const LowerResult &r, const Graph &base)
{
    Graph g = base;
    GraphNode begin;
    begin.id = QStringLiteral("evt");
    begin.kind = NodeKind::Builtin;
    begin.ref = bi::Begin;
    begin.opts.insert(QStringLiteral("noSuper"), QStringLiteral("1"));
    g.nodes.append(begin);
    for (const GraphNode &n : r.nodes) g.nodes.append(n);
    for (const GraphEdge &e : r.edges) g.edges.append(e);
    for (const GraphVariable &v : r.variables) g.variables.append(v);
    if (!r.entryNode.isEmpty())
        g.edges.append({QStringLiteral("e_entry"),
                        {begin.id, QStringLiteral("exec")},
                        {r.entryNode, QStringLiteral("exec")},
                        {}});
    return g;
}

// Just the statements of the generated EEInit, with the class header, the
// member declarations and the user region stripped off.
QString bodyOf(const QString &generated)
{
    const QStringList lines = generated.split(QLatin1Char('\n'));
    QStringList body;
    bool inMethod = false;
    int depth = 0;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (!inMethod) {
            if (t.startsWith(QLatin1String("override void EEInit"))
                || t.startsWith(QLatin1String("void EEInit"))) {
                inMethod = true;
                depth = 0;
            }
            continue;
        }
        if (t == QLatin1String("{")) {
            depth++;
            if (depth == 1) continue;
        }
        if (t == QLatin1String("}")) {
            depth--;
            if (depth == 0) break;
        }
        body << line;
    }
    return body.join(QLatin1Char('\n'));
}

// The generated body arrives indented to where it sits in the class. Taking
// that common indent off is what makes it comparable with the source line it
// came from, without forgiving any indentation inside the block.
QString dedent(const QString &code)
{
    QStringList lines = code.split(QLatin1Char('\n'));
    int common = -1;
    for (const QString &l : lines) {
        if (l.trimmed().isEmpty()) continue;
        int tabs = 0;
        while (tabs < l.size() && l.at(tabs) == QLatin1Char('\t')) tabs++;
        common = common < 0 ? tabs : qMin(common, tabs);
    }
    if (common <= 0) return code;
    for (QString &l : lines) l = l.mid(qMin(common, l.size()));
    return lines.join(QLatin1Char('\n')).trimmed();
}

struct Outcome {
    Match match = Match::Different;
    int lowered = 0;
    int raw = 0;
    int nodes = 0;
    QString generated;
};

Outcome roundTrip(const QString &code, const Catalog &cat, const Builtins &builtins,
                  const Project &project, const Graph &base, const QString &selfClass,
                  const QHash<QString, QString> &params)
{
    Outcome o;
    LowerOptions opts;
    opts.selfClass = selfClass;
    opts.knownLocals = params;
    const LowerResult r = lowerEnforceCode(code, cat, builtins, base, project, opts);
    o.lowered = r.statementsLowered;
    o.raw = r.statementsRaw;
    o.nodes = r.nodes.size();
    if (r.nodes.isEmpty()) return o;

    const Graph g = graphFor(r, base);
    const GenResult gen = generateEnforce(g, cat, builtins, project);
    o.generated = bodyOf(gen.code);
    o.match = compare(code, o.generated);
    return o;
}

} // namespace

int main(int argc, char *argv[])
{
    // Nothing may sit in a buffer waiting for a clean exit. The numbers this run
    // exists to print are the last thing written, so a process that dies without
    // unwinding takes exactly the part worth reading.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // Installed before the application object, because construction can warn.
    qInstallMessageHandler(relayQtMessage);

    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    reportStream = &out;

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("resources");

    Catalog cat;
    if (!cat.load(root + "/catalog.json")) {
        out << "cannot load catalog.json: " << cat.error() << Qt::endl;
        return 1;
    }
    Builtins builtins;

    Project project;
    QString err;
    const bool haveProject = loadProject(root + "/SUDO_Link.sdzn", project, &err);

    // The class the corpus is written against: without its members, every
    // assignment below would be an assignment to a name nothing declares.
    Graph base;
    base.className = QStringLiteral("SUDO_Probe");
    base.baseClass = QStringLiteral("ItemBase");
    const auto member = [&base](const char *name, const char *type) {
        GraphVariable v;
        v.id = QStringLiteral("var_%1").arg(QString::fromUtf8(name));
        v.name = QString::fromUtf8(name);
        v.type = QString::fromUtf8(type);
        base.variables.append(v);
    };
    member("m_Count", "int");
    member("m_Name", "string");
    member("m_Ready", "bool");
    member("m_Timer", "Timer");
    member("m_Items", "array<ref ItemBase>");
    member("m_Owner", "EntityAI");
    // `m_MinDamageIngredient[0] = -1;` out of Sudo Server Pack is the shape
    // behind the Set Element node, so the fixture carries something to index.
    member("m_Damage", "array<float>");

    // ------------------------------------------------------------- corpus
    // Every line here is a shape the importer used to leave as a text box.
    // Exact: token for token. Equivalent: the same but for names the graph
    // replaced with wires. Rewritten: the graph says the same thing a different
    // way, which is the point of a compound assignment or an inlined local.
    enum class Want { Exact, Equivalent, Rewritten };
    struct Case {
        const char *name;
        const char *code;
        Want want;
    };
    const QVector<Case> cases = {
        {"member assignment", "m_Count = 3;", Want::Exact},
        {"string assignment", "m_Name = \"link\";", Want::Exact},
        {"bool assignment", "m_Ready = false;", Want::Exact},
        {"assignment from a call", "m_Count = GetQuantity();", Want::Exact},
        {"member call", "SetQuantity(5);", Want::Exact},
        {"call on a member", "m_Timer.Stop();", Want::Exact},
        {"call through the game singleton", "Print(GetGame().IsServer());", Want::Exact},
        {"operator", "m_Count = m_Count + 1;", Want::Exact},
        {"nested operator", "m_Count = m_Count * 2 + 1;", Want::Exact},
        {"comparison", "m_Ready = m_Count > 0;", Want::Exact},
        {"not", "m_Ready = !m_Ready;", Want::Exact},
        {"branch on a member", "if (m_Ready)\n{\n\tSetQuantity(1);\n}", Want::Exact},
        {"branch on not", "if (!m_Ready)\n{\n\tSetQuantity(1);\n}", Want::Exact},
        {"branch with else",
         "if (m_Ready)\n{\n\tSetQuantity(1);\n}\nelse\n{\n\tSetQuantity(2);\n}", Want::Exact},
        {"return", "return;", Want::Exact},
        {"return a value", "return m_Count;", Want::Exact},
        {"early out", "if (!m_Ready)\n{\n\treturn;\n}\nSetQuantity(1);", Want::Exact},
        {"server guard", "if (!GetGame().IsServer())\n\treturn;", Want::Exact},
        {"statement after a branch",
         "if (m_Ready)\n{\n\tSetQuantity(1);\n}\nSetQuantity(2);", Want::Exact},
        {"counted loop",
         "for (int i = 0; i < 4; i++)\n{\n\tSetQuantity(i);\n}", Want::Equivalent},
        {"while", "while (m_Ready)\n{\n\tSetQuantity(1);\n}", Want::Exact},
        {"select", "m_Count = m_Ready ? 1 : 2;", Want::Exact},
        {"compound assignment", "m_Count += 2;", Want::Rewritten},
        {"increment", "m_Count++;", Want::Rewritten},
        {"local wired straight through",
         "int total = m_Count + 1;\nSetQuantity(total);", Want::Rewritten},
        {"local written twice",
         "int total = 0;\ntotal = m_Count;\nSetQuantity(total);", Want::Rewritten},
        {"print", "Print(\"hello\");", Want::Exact},
        {"print a member", "Print(m_Count);", Want::Exact},
        // The element type comes back as `auto`, which vanilla writes too, so
        // the loop means the same thing under a different spelling.
        {"foreach over a member array",
         "foreach (ItemBase item : m_Items)\n{\n\titem.SetQuantity(1);\n}", Want::Rewritten},
        {"foreach with an index",
         "foreach (int i, ItemBase item : m_Items)\n{\n\tPrint(i);\n}", Want::Rewritten},
        {"the CastTo pattern",
         "ItemBase item;\nif (Class.CastTo(item, m_Owner))\n{\n\titem.SetQuantity(1);\n}",
         Want::Equivalent},
        {"cast with an else",
         "ItemBase item;\nif (Class.CastTo(item, m_Owner))\n{\n\titem.SetQuantity(1);\n}"
         "\nelse\n{\n\tPrint(\"no\");\n}",
         Want::Equivalent},
        {"a call on a cast result",
         "ItemBase item;\nif (Class.CastTo(item, m_Owner))\n{\n\tm_Count = item.GetQuantity();"
         "\n}",
         Want::Equivalent},
        {"nested branch inside a loop",
         "foreach (ItemBase item : m_Items)\n{\n\tif (m_Ready)\n\t{\n\t\titem.SetQuantity(1);"
         "\n\t}\n}",
         Want::Rewritten},
        {"switch on a member",
         "switch (m_Count)\n{\n\tcase 1:\n\t\tSetQuantity(1);\n\t\tbreak;\n\tcase 2:"
         "\n\t\tSetQuantity(2);\n\t\tbreak;\n\tdefault:\n\t\tSetQuantity(0);\n}",
         Want::Rewritten},
    };

    out << "corpus" << Qt::endl;
    int exact = 0;
    int renamed = 0;
    int different = 0;
    int allRaw = 0;
    for (const Case &c : cases) {
        const Outcome o = roundTrip(QString::fromUtf8(c.code), cat, builtins, project,
                                    base, base.baseClass, {});
        if (o.lowered == 0) allRaw++;
        if (o.match == Match::Exact) exact++;
        else if (o.match == Match::Renamed) renamed++;
        else different++;

        const bool pass = c.want == Want::Exact      ? o.match == Match::Exact
                          : c.want == Want::Equivalent ? o.match != Match::Different
                                                       : true;
        // Whatever the shape of the result, nothing in the corpus may fall back
        // to a Raw node: these are the statements the tool has to model.
        check(pass && o.raw == 0, QString::fromUtf8(c.name));
        if (!pass || o.raw != 0) {
            out << "         in:  " << QString::fromUtf8(c.code).replace('\n', "\\n")
                << Qt::endl;
            out << "         out: " << o.generated.trimmed().replace('\n', "\\n")
                << Qt::endl;
        }
    }
    out << "       " << exact << " exact, " << renamed << " equivalent, " << different
        << " different, " << allRaw << " not lowered at all" << Qt::endl;

    // Variables must not be invented for a local that is only written once:
    // that is the whole difference between a graph and a pile of boxes.
    // --------------------------------------- statements that used to keep text
    // Each of these is a line out of the installed mods, ranked by how many
    // statements its shape was holding back. The check is the exact text, not
    // the token comparison above: these exist because a method is only taken as
    // nodes when it regenerates character for character, so "close enough" is
    // the same as not converting at all.
    out << Qt::endl << "shapes that used to keep their text" << Qt::endl;
    {
        struct Exact {
            const char *name;
            const char *code;
            const char *node;  // a node ref the result has to contain
            int nodes;         // how many of it
        };
        const QVector<Exact> exacts = {
            // Sudo Server Pack 1: assignment to one slot of an array.
            {"one slot of an array", "m_Damage[0] = -1;", "bi.setElement", 1},
            // 3D Printer: a member the base class owns, which the graph cannot
            // declare and so had no Set node for.
            {"a member the base class owns", "m_bIsPrinting = true;", "bi.setMember", 1},
            // CashRobbery: a field on an object of a type nothing here declares.
            {"a field on an unknown object", "weapons.Min = 1;", "bi.setMember", 1},
            // BS Patrol Tank: one line the graph cannot read used to take the
            // whole `if` around it back to text.
            {"an unreadable line stays on its own",
             "if (m_Ready)\n{\n\tdelete m_Timer;\n\tSetQuantity(1);\n}", "bi.raw", 1},
        };
        for (const Exact &c : exacts) {
            const QString code = QString::fromUtf8(c.code);
            const Outcome o = roundTrip(code, cat, builtins, project, base,
                                        base.baseClass, {});
            LowerOptions opts;
            opts.selfClass = base.baseClass;
            const LowerResult r = lowerEnforceCode(code, cat, builtins, base, project, opts);
            int found = 0;
            for (const GraphNode &n : r.nodes)
                if (n.ref == QLatin1String(c.node)) found++;
            const QString got = dedent(o.generated);
            check(found == c.nodes && got == code, QString::fromUtf8(c.name));
            if (found != c.nodes || got != code) {
                out << "         want " << c.nodes << " " << c.node << ", got " << found
                    << Qt::endl;
                out << "         in:  " << QString(code).replace('\n', "\\n") << Qt::endl;
                out << "         out: " << QString(got).replace('\n', "\\n") << Qt::endl;
            }
        }
    }

    // The generator used to bracket every operator, so `m_Count + 1` came back
    // as `(m_Count + 1)` and the method around it was turned down for being
    // spelled differently. It now brackets by precedence, which is the one
    // change here that could quietly alter what a graph means, so these are
    // checked as exact text: the comparison further up drops brackets and would
    // pass a tree that had been rewired.
    out << Qt::endl << "brackets only where they change the meaning" << Qt::endl;
    {
        const QVector<QPair<const char *, const char *>> spelled = {
            {"a plain operator", "m_Count = m_Count + 1;"},
            {"tighter on the right", "m_Count = m_Count + 1 * 2;"},
            {"looser on the left", "m_Count = (m_Count + 1) * 2;"},
            {"the same on the right", "m_Count = m_Count - (m_Count - 1);"},
            {"the same on the left", "m_Count = m_Count - m_Count - 1;"},
            {"divide keeps its right side", "m_Count = m_Count / (m_Count / 2);"},
            {"comparison inside and", "m_Ready = m_Count > 0 && m_Ready;"},
            {"or inside and", "m_Ready = (m_Ready || m_Count > 0) && m_Ready;"},
            {"not on a name", "m_Ready = !m_Ready;"},
            {"not on a comparison", "m_Ready = !(m_Count > 0);"},
            {"a ternary", "m_Count = m_Ready ? 1 : 2;"},
            {"a ternary inside an operator", "m_Count = (m_Ready ? 1 : 2) + 1;"},
        };
        for (const auto &c : spelled) {
            const QString code = QString::fromUtf8(c.second);
            const Outcome o = roundTrip(code, cat, builtins, project, base,
                                        base.baseClass, {});
            const QString got = dedent(o.generated);
            check(got == code, QString::fromUtf8(c.first));
            if (got != code)
                out << "         want: " << code << Qt::endl
                    << "         got:  " << got << Qt::endl;
        }
    }

    // A name declared on several classes with one signature between them is not
    // a choice at all: the generator writes `target.Count()` whichever one is
    // meant, so refusing it only cost the method around it.
    {
        LowerOptions opts;
        opts.selfClass = base.baseClass;
        opts.knownLocals.insert(QStringLiteral("holder"),
                                QStringLiteral("SUDO_NotInTheCatalogue"));
        const LowerResult r = lowerEnforceCode(QStringLiteral("m_Count = holder.Count();"),
                                               cat, builtins, base, project, opts);
        int raw = 0;
        int calls = 0;
        for (const GraphNode &n : r.nodes) {
            if (n.ref == bi::Raw) raw++;
            if (n.kind == NodeKind::Call) calls++;
        }
        const Graph g = graphFor(r, base);
        const QString got = dedent(bodyOf(generateEnforce(g, cat, builtins, project).code));
        check(raw == 0 && calls == 1 && got == QLatin1String("m_Count = holder.Count();"),
              QStringLiteral("a name several classes declare the same way"));
        if (got != QLatin1String("m_Count = holder.Count();"))
            out << "         out: " << QString(got).replace('\n', "\\n") << Qt::endl;
    }

    out << Qt::endl << "single-assignment locals stay wires" << Qt::endl;
    {
        LowerOptions opts;
        opts.selfClass = base.baseClass;
        const LowerResult r = lowerEnforceCode(
            QStringLiteral("int total = m_Count + 1;\nSetQuantity(total);"), cat, builtins,
            base, project, opts);
        check(r.variables.isEmpty(),
              QStringLiteral("no variable invented (%1 created)").arg(r.variables.size()));
        int rawNodes = 0;
        for (const GraphNode &n : r.nodes)
            if (n.ref == bi::Raw) rawNodes++;
        check(rawNodes == 0, QStringLiteral("%1 raw nodes left").arg(rawNodes));
    }
    {
        LowerOptions opts;
        opts.selfClass = base.baseClass;
        const LowerResult r = lowerEnforceCode(
            QStringLiteral("int total = 0;\ntotal = m_Count;\nSetQuantity(total);"), cat,
            builtins, base, project, opts);
        check(r.variables.size() == 1,
              QStringLiteral("a twice-written local becomes one variable (%1)")
                  .arg(r.variables.size()));
    }

    // The shape the whole feature was asked for.
    out << Qt::endl << "the shape this exists for" << Qt::endl;
    {
        Graph g;
        g.className = QStringLiteral("SUDO_Api");
        g.baseClass = QStringLiteral("Managed");
        GraphVariable v;
        v.id = QStringLiteral("v1");
        v.name = QStringLiteral("m_RestApi");
        v.type = QStringLiteral("RestApi");
        g.variables.append(v);

        LowerOptions opts;
        opts.selfClass = g.baseClass;
        const LowerResult r = lowerEnforceCode(
            QStringLiteral("m_RestApi = CreateRestApi();\n"
                           "if (!m_RestApi)\n{\n\treturn;\n}\n"
                           "m_RestApi.EnableDebug(false);"),
            cat, builtins, g, project, opts);

        int sets = 0, gets = 0, branches = 0, nots = 0, calls = 0, raws = 0;
        for (const GraphNode &n : r.nodes) {
            if (n.kind == NodeKind::VarSet) sets++;
            else if (n.kind == NodeKind::VarGet) gets++;
            else if (n.ref == bi::Branch) branches++;
            else if (n.ref == QLatin1String("bi.not")) nots++;
            else if (n.ref == bi::Raw) raws++;
            else if (n.kind == NodeKind::Call) calls++;
        }
        check(sets == 1, QStringLiteral("one Set m_RestApi (%1)").arg(sets));
        check(gets >= 2, QStringLiteral("Get m_RestApi feeds the reads (%1)").arg(gets));
        check(branches == 1, QStringLiteral("one Branch (%1)").arg(branches));
        check(nots == 1, QStringLiteral("one Not (%1)").arg(nots));
        check(calls >= 2,
              QStringLiteral("CreateRestApi and EnableDebug are call nodes (%1)").arg(calls));
        check(raws == 0, QStringLiteral("%1 raw nodes left").arg(raws));

        const Graph built = graphFor(r, g);
        const GenResult gen = generateEnforce(built, cat, builtins, project);
        check(gen.code.contains(QStringLiteral("CreateRestApi()"))
                  && gen.code.contains(QStringLiteral("m_RestApi =")),
              QStringLiteral("regenerates the assignment"));
        check(gen.code.contains(QStringLiteral("EnableDebug(false)")),
              QStringLiteral("regenerates the call"));
    }

    // Exploding a raw node has to leave the chain it was part of intact.
    out << Qt::endl << "explode in place" << Qt::endl;
    {
        Graph g;
        g.className = QStringLiteral("SUDO_Probe");
        g.baseClass = QStringLiteral("ItemBase");
        GraphNode begin;
        begin.id = QStringLiteral("n_begin");
        begin.kind = NodeKind::Builtin;
        begin.ref = bi::Begin;
        GraphNode raw;
        raw.id = QStringLiteral("n_raw");
        raw.kind = NodeKind::Builtin;
        raw.ref = bi::Raw;
        raw.opts.insert(QStringLiteral("code"),
                        QStringLiteral("SetQuantity(1);\nSetQuantity(2);"));
        GraphNode after;
        after.id = QStringLiteral("n_after");
        after.kind = NodeKind::Builtin;
        after.ref = bi::Print;
        after.inputs.insert(QStringLiteral("value"), QStringLiteral("done"));
        g.nodes << begin << raw << after;
        g.edges.append({QStringLiteral("e1"),
                        {begin.id, QStringLiteral("exec")},
                        {raw.id, QStringLiteral("exec")},
                        {}});
        g.edges.append({QStringLiteral("e2"),
                        {raw.id, QStringLiteral("exec")},
                        {after.id, QStringLiteral("exec")},
                        {}});

        QStringList notes;
        const bool done = explodeRawNode(g, raw.id, cat, builtins, project, &notes);
        check(done, QStringLiteral("the raw node converted"));
        check(!g.node(raw.id), QStringLiteral("the raw node is gone"));
        const GenResult gen = generateEnforce(g, cat, builtins, project);
        check(gen.code.contains(QStringLiteral("SetQuantity(1);")),
              QStringLiteral("the first statement survived"));
        check(gen.code.contains(QStringLiteral("SetQuantity(2);")),
              QStringLiteral("the second statement survived"));
        check(gen.code.contains(QStringLiteral("Print(")),
              QStringLiteral("what came after is still chained on"));
    }

    // A raw node that lowers to nothing but another raw node is not an
    // improvement, and the action has to say so rather than churn the graph.
    {
        Graph g;
        g.className = QStringLiteral("SUDO_Probe");
        g.baseClass = QStringLiteral("ItemBase");
        GraphNode raw;
        raw.id = QStringLiteral("n_raw");
        raw.kind = NodeKind::Builtin;
        raw.ref = bi::Raw;
        raw.opts.insert(QStringLiteral("code"),
                        QStringLiteral("delete m_Something;"));
        g.nodes << raw;
        const int before = g.nodes.size();
        const bool done = explodeRawNode(g, raw.id, cat, builtins, project);
        check(!done, QStringLiteral("nothing to gain, so nothing changes"));
        check(g.nodes.size() == before && g.node(raw.id),
              QStringLiteral("the graph is untouched"));
    }

    // ------------------------------------------------- the real project
    // The action the user runs, on every raw node the project has: convert one
    // in place and check the whole generated file still says what it said
    // before. That takes in the wires the node was carrying as well as its
    // code, which is the strongest form of the round trip.
    out << Qt::endl << "every raw node in SUDO_Link.sdzn" << Qt::endl;
    if (!haveProject) {
        out << "       (SUDO_Link.sdzn not present: " << err << ")" << Qt::endl;
    } else {
        const bool verbose = argc > 2 && QString::fromLocal8Bit(argv[2]) == QLatin1String("-v");
        int rawNodes = 0;
        int converted = 0;
        int fully = 0;
        int same = 0;
        int equivalent = 0;
        int rewritten = 0;
        int changed = 0;
        // What is still text, and why, so the next thing to teach the lowerer
        // is the one that comes up most rather than the one noticed first.
        QHash<QString, int> reasons;
        QStringList refused;
        QStringList drifted;

        for (const ScriptEntry &s : project.scripts) {
            const GenResult before = generateEnforce(s.graph, cat, builtins, project);
            QStringList ids;
            for (const GraphNode &n : s.graph.nodes)
                if (n.ref == bi::Raw
                    && !n.opts.value(QStringLiteral("code")).trimmed().isEmpty())
                    ids << n.id;
            rawNodes += ids.size();

            for (const QString &id : ids) {
                Graph g = s.graph;
                const QString code = g.node(id)->opts.value(QStringLiteral("code"));
                int rawBefore = 0;
                for (const GraphNode &n : g.nodes)
                    if (n.ref == bi::Raw) rawBefore++;

                QStringList notes;
                if (!explodeRawNode(g, id, cat, builtins, project, &notes)) {
                    for (const QString &note : notes) reasons[note]++;
                    if (notes.isEmpty()) reasons[QStringLiteral("(no reason given)")]++;
                    if (refused.size() < 15)
                        refused << code.split(QLatin1Char('\n')).first().trimmed();
                    continue;
                }
                converted++;
                int rawAfter = 0;
                for (const GraphNode &n : g.nodes)
                    if (n.ref == bi::Raw) rawAfter++;
                if (rawAfter < rawBefore) fully++;
                for (const QString &note : notes) reasons[note]++;

                const GenResult after = generateEnforce(g, cat, builtins, project);
                switch (compare(before.code, after.code)) {
                case Match::Exact: same++; break;
                case Match::Renamed: equivalent++; break;
                case Match::Different: {
                    if (rewrittenOnPurpose(code)) {
                        rewritten++;
                        break;
                    }
                    changed++;
                    if (drifted.size() < 8) {
                        const QStringList ta = meaningfulTokens(inlineTemps(before.code));
                        const QStringList tb = meaningfulTokens(inlineTemps(after.code));
                        int at = 0;
                        while (at < ta.size() && at < tb.size() && ta.at(at) == tb.at(at))
                            at++;
                        drifted << s.name + QStringLiteral(" / ") + id
                                       + QStringLiteral("\n            was: ")
                                       + ta.mid(qMax(0, at - 6), 22).join(QLatin1Char(' '))
                                       + QStringLiteral("\n            now: ")
                                       + tb.mid(qMax(0, at - 6), 22).join(QLatin1Char(' '));
                    }
                    break;
                }
                }
            }
        }

        const auto pct = [](int n, int of) {
            return of > 0 ? QString::number(100.0 * n / of, 'f', 1) : QStringLiteral("0.0");
        };
        out << "       raw nodes:                 " << rawNodes << Qt::endl;
        out << "       convert to nodes accepted: " << converted << "  ("
            << pct(converted, rawNodes) << "%)" << Qt::endl;
        out << "       nothing raw left behind:   " << fully << "  ("
            << pct(fully, rawNodes) << "%)" << Qt::endl;
        out << "       file identical after:      " << same << "  ("
            << pct(same, converted) << "% of converted)" << Qt::endl;
        out << "       file equivalent after:     " << (same + equivalent) << "  ("
            << pct(same + equivalent, converted) << "% of converted)" << Qt::endl;
        out << "       rewritten on purpose:      " << rewritten << Qt::endl;
        out << "       file changed after:        " << changed << Qt::endl;

        check(rawNodes > 300, QStringLiteral("%1 raw nodes found").arg(rawNodes));
        check(converted * 2 > rawNodes,
              QStringLiteral("more than half of them stop being text"));
        check(changed == 0,
              QStringLiteral("%1 conversions changed the generated file").arg(changed));

        if (verbose) {
            out << Qt::endl << "what is still text" << Qt::endl;
            QVector<QPair<int, QString>> ranked;
            for (auto it = reasons.constBegin(); it != reasons.constEnd(); ++it)
                ranked.append({it.value(), it.key()});
            std::sort(ranked.begin(), ranked.end(),
                      [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                          return a.first > b.first;
                      });
            for (int i = 0; i < ranked.size() && i < 25; ++i)
                out << "       " << ranked.at(i).first << "  " << ranked.at(i).second
                    << Qt::endl;
            out << Qt::endl << "first lines of blocks that kept their code" << Qt::endl;
            for (const QString &sample : refused) out << "       " << sample << Qt::endl;
            out << Qt::endl << "blocks whose conversion changed the file" << Qt::endl;
            for (const QString &sample : drifted)
                out << "       ---- " << sample << Qt::endl;
        }
    }

    // A Qt message in a run that has no GUI is the shape of a bug that used to
    // end this process with RaiseFailFastException and no explanation: the
    // layout pass measuring text through a font database that needs a
    // QGuiApplication. Named here, it is a failing check rather than a hex code.
    out << Qt::endl;
    check(qtMessages == 0,
          QStringLiteral("Qt said nothing during the run (%1 messages)").arg(qtMessages));

    out << Qt::endl
        << (failures == 0 ? QStringLiteral("ALL LOWERING TESTS PASSED")
                          : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;

    // The stream goes out of scope on the next line; the handler must not reach
    // for it after that.
    out.flush();
    reportStream = nullptr;
    return failures == 0 ? 0 : 1;
}
