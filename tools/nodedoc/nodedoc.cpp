// Writes docs/node-reference.md out of the same tables the app reads, and
// prints the Enforce a .sdzn generates.
//
// The palette is not a hand-written list. Its groups, their ordering and the
// text under each one live in src/nodeindex.cpp; what a node does, what it
// emits and what to watch out for live in src/builtins.cpp; everything else is
// resolved out of resources/catalog.json. A reference typed out by hand would
// be a fourth copy of all of that, and would be wrong the first time somebody
// added a node. So this links the real tables and prints them.
//
//   nodedoc --reference docs/node-reference.md [--catalog resources/catalog.json]
//   nodedoc --generate  project.sdzn [--script Name]
//
// The second mode is why the worked examples in docs/examples are the code the
// tool really produces rather than Enforce written beside a picture: each one
// ships its .sdzn, and this prints what it generates.
//
// Exits non-zero if it wrote a character outside printable ASCII, because the
// output is committed documentation and the house rule is plain ASCII.
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "nodeindex.h"
#include "project.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

namespace {

QTextStream &err()
{
    static QTextStream s(stderr);
    return s;
}

// A markdown table cell. The operator nodes are literally named "|" and "||",
// so this is not a formality.
QString cell(const QString &text)
{
    QString out = text.simplified();
    out.replace(QLatin1Char('|'), QLatin1String("\\|"));
    return out;
}

QString code(const QString &text)
{
    return text.isEmpty() ? QString() : QStringLiteral("`") + text + QStringLiteral("`");
}

QString pinTypeName(const PinType &t)
{
    QString base;
    switch (t.kind) {
    case PinKind::Exec:     return QStringLiteral("exec");
    case PinKind::Bool:     base = QStringLiteral("bool"); break;
    case PinKind::Int:      base = QStringLiteral("int"); break;
    case PinKind::Float:    base = QStringLiteral("float"); break;
    case PinKind::String:   base = QStringLiteral("string"); break;
    case PinKind::Vector:   base = QStringLiteral("vector"); break;
    case PinKind::Typename: base = QStringLiteral("typename"); break;
    case PinKind::Enum:     base = t.cls.isEmpty() ? QStringLiteral("enum") : t.cls; break;
    case PinKind::Object:   base = t.cls.isEmpty() ? QStringLiteral("object") : t.cls; break;
    case PinKind::Any:      base = QStringLiteral("any"); break;
    }
    return t.isArray ? QStringLiteral("array<%1>").arg(base) : base;
}

// "condition (bool), value (float)" for one side of a node.
QString pinLine(const NodeDef &def, PinDir dir)
{
    QStringList parts;
    for (const Pin &p : def.pins) {
        if (p.dir != dir) continue;
        const QString label = p.label.isEmpty()
                                  ? (p.type.kind == PinKind::Exec ? QStringLiteral("exec")
                                                                  : p.id)
                                  : p.label;
        if (p.type.kind == PinKind::Exec)
            parts << label;
        else
            parts << QStringLiteral("%1 (%2)").arg(label, pinTypeName(p.type));
    }
    if (def.list.valid())
        parts << QStringLiteral("%1, %2 to %3 of them")
                     .arg(def.list.label)
                     .arg(def.list.min)
                     .arg(def.list.max);
    return parts.join(QStringLiteral(", "));
}

// A builtin's doc is one summary paragraph, then its effects, then any
// cautions, joined by blank lines. Same split builtins.cpp makes when it builds
// the string, read back the other way round.
struct Help {
    QString summary;
    QStringList effects;
    QStringList cautions;
};

Help splitHelp(const QString &doc)
{
    Help out;
    const QStringList parts = doc.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts);
    static const QLatin1String prefix("Caution: ");
    for (int i = 0; i < parts.size(); ++i) {
        const QString part = parts.at(i).simplified();
        if (part.startsWith(prefix)) out.cautions << part.mid(prefix.size());
        else if (i == 0) out.summary = part;
        else out.effects << part;
    }
    return out;
}

void writeGroups(QTextStream &md, const Catalog &cat, const Builtins &builtins)
{
    // No self class: the index withholds a row a graph could not legally call,
    // and an empty class cannot be shown to inherit anything, so this is the
    // widest view of the palette there is. Anything a real graph sees is a
    // subset of what is printed here.
    const QVector<IndexGroup> groups = nodeIndex(cat, builtins, QString());

    md << "## The groups\n\n";
    md << "The palette is arranged by what you are trying to do, not by which\n";
    md << "engine class a method happens to sit on. " << groups.size() - 1
       << " groups are written by\n";
    md << "hand and ordered by how often the thing they cover appears in shipped\n";
    md << "mod code, measured in [notes/expansion.md](notes/expansion.md). The\n";
    md << "last one is generated from whatever the other " << groups.size() - 1
       << " do not name, so a\n";
    md << "node added to the tool cannot go missing from the palette.\n\n";

    md << "| Group | Nodes |\n";
    md << "| --- | --- |\n";
    for (const IndexGroup &g : groups) {
        md << "| " << cell(g.title) << " | " << g.rows.size()
           << (g.rows.size() == 1 ? " node |\n" : " nodes |\n");
    }
    md << "\n";

    for (const IndexGroup &g : groups) {
        md << "### " << g.title << "\n\n";
        md << g.doc << "\n\n";
        md << "| Node | Shape | What it does |\n";
        md << "| --- | --- | --- |\n";
        for (const IndexRow &r : g.rows) {
            md << "| " << cell(r.title) << " | " << cell(r.detail) << " | "
               << cell(r.doc) << " |\n";
        }
        md << "\n";

        // Deduped: a group can hold the same declaration twice under two
        // owners (ConfigIsExisting on Object and on CGame), and printing the
        // same warning about both of them twice reads as a mistake.
        QStringList cautions;
        for (const IndexRow &r : g.rows) {
            for (const QString &c : nodeCautions(cat, builtins, r.key)) {
                const QString line = QStringLiteral("**%1.** %2").arg(r.title, c);
                if (!cautions.contains(line)) cautions << line;
            }
        }
        if (cautions.isEmpty()) continue;
        md << "Watch out for:\n\n";
        for (const QString &c : cautions) md << "- " << c << "\n";
        md << "\n";
    }
}

void writeBeginModes(QTextStream &md, const Builtins &builtins)
{
    md << "## The four ways a script starts\n\n";
    md << "Enforce has no single begin-play. Begin carries the choice, set in\n";
    md << "Details on the node, and becomes the method named here with `super`\n";
    md << "called for you.\n\n";
    md << "| Mode | Becomes | When it fires |\n";
    md << "| --- | --- | --- |\n";
    for (const QString &key : builtins.beginModeOrder()) {
        const LifecycleSig sig = builtins.beginMode(key);
        const QString method = sig.ctor ? QStringLiteral("the constructor")
                                        : QStringLiteral("`%1()`").arg(sig.method);
        md << "| " << cell(sig.label) << " | " << method << " | " << cell(sig.note)
           << " |\n";
    }
    md << "\n";
}

void writeOperators(QTextStream &md, const Builtins &builtins)
{
    md << "## Operators\n\n";
    md << "One node per operator rather than one Operator node with a symbol to\n";
    md << "set, so placing a subtraction is one action.\n\n";
    md << "| Node | Emits | Yields |\n";
    md << "| --- | --- | --- |\n";
    for (const QString &symbol : Builtins::binaryOperators()) {
        const NodeDef def = builtins.def(QStringLiteral("bi.op.") + symbol);
        if (!def.valid) continue;
        const QString yields = Builtins::operatorYieldsBool(symbol)
                                   ? QStringLiteral("bool")
                                   : QStringLiteral("the type of its inputs");
        md << "| " << cell(def.title) << " | " << cell(code(QStringLiteral("a %1 b").arg(symbol)))
           << " | " << yields << " |\n";
    }
    md << "\n";

    md << "Literal nodes take one of these types: ";
    QStringList types;
    for (const QString &t : Builtins::literalTypes()) types << code(t);
    md << types.join(QStringLiteral(", ")) << ".\n\n";

    md << "Timing nodes run on one of three queues, set in Details:\n\n";
    for (const QString &key : Builtins::callCategories())
        md << "- " << Builtins::callCategoryLabel(key) << "\n";
    md << "\n";
}

void writeEveryBuiltin(QTextStream &md, const Builtins &builtins)
{
    md << "## Every builtin node\n\n";
    md << "The nodes that are not vanilla API: flow control, operators,\n";
    md << "literals, arrays, timing, variable access and the way out to raw\n";
    md << "Enforce. Everything else in the palette comes from the catalogue and\n";
    md << "is documented by DayZ's own declarations.\n\n";

    QString lastCategory;
    for (const NodeDef &def : builtins.all()) {
        if (def.category != lastCategory) {
            md << "### " << def.category << "\n\n";
            lastCategory = def.category;
        }
        const Help h = splitHelp(def.doc);
        md << "#### " << def.title;
        if (!def.subtitle.isEmpty()) md << ", " << def.subtitle;
        md << "\n\n";
        if (!h.summary.isEmpty()) md << h.summary << "\n\n";

        const QString ins = pinLine(def, PinDir::In);
        const QString outs = pinLine(def, PinDir::Out);
        if (!ins.isEmpty()) md << "In: " << ins << "  \n";
        if (!outs.isEmpty()) md << "Out: " << outs << "\n";
        if (!ins.isEmpty() || !outs.isEmpty()) md << "\n";
        if (def.pure)
            md << "Pure: no exec pins, it evaluates where it is used.\n\n";

        for (const QString &e : h.effects) md << "- " << e << "\n";
        if (!h.effects.isEmpty()) md << "\n";
        for (const QString &c : h.cautions) md << "Caution: " << c << "\n\n";
        md << "Node id `" << def.key << "`.\n\n";
    }
}

int writeReference(const QString &outPath, const Catalog &cat, const Builtins &builtins)
{
    QString text;
    QTextStream md(&text);

    const QHash<QString, int> totals = cat.totals();
    const auto total = [&totals](const char *name) {
        return totals.value(QString::fromLatin1(name));
    };

    md << "# Node reference\n\n";
    md << "This file is generated. Do not edit it: run\n";
    md << "`nodedoc --reference docs/node-reference.md` after changing the node\n";
    md << "tables, and commit what comes out. Building the generator is in\n";
    md << "[architecture.md](architecture.md).\n\n";

    md << "Every vanilla class, method, enum, global function and constant is a\n";
    md << "node. The catalogue in this build holds " << total("classes")
       << " classes and " << total("methods") << " methods,\n";
    md << "of which " << total("events") << " are events and " << total("pure")
       << " are pure, plus " << total("enums") << " enums, " << total("globals")
       << "\n";
    md << "global functions and " << total("consts") << " constants. It is built from `"
       << cat.source() << "`.\n\n";
    md << "Searching for one of those by name is what the palette's search box\n";
    md << "is for. This page is the other half: which nodes to reach for when\n";
    md << "you do not already know the name of the thing you want.\n\n";

    writeGroups(md, cat, builtins);
    writeBeginModes(md, builtins);
    writeOperators(md, builtins);
    writeEveryBuiltin(md, builtins);
    md.flush();

    // The output is committed documentation, and the house rule for anything
    // this project writes is plain ASCII. Reporting the character and where it
    // landed beats a silent em-dash in a generated file.
    int offenders = 0;
    int line = 1;
    for (const QChar c : text) {
        if (c == QLatin1Char('\n')) { ++line; continue; }
        if (c.unicode() >= 0x20 && c.unicode() < 0x7f) continue;
        if (c == QLatin1Char('\t')) continue;
        if (++offenders <= 10)
            err() << "non-ascii U+" << Qt::hex << c.unicode() << Qt::dec
                  << " on line " << line << "\n";
    }
    if (offenders > 0) {
        err() << offenders << " non-ascii characters, nothing written\n";
        err().flush();
        return 2;
    }

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        err() << "cannot write " << outPath << "\n";
        err().flush();
        return 1;
    }
    out.write(text.toUtf8());
    out.close();
    QTextStream(stdout) << "wrote " << outPath << ", " << text.size() << " characters\n";
    return 0;
}

int generate(const QString &projectPath, const QString &only, const Catalog &cat,
             const Builtins &builtins)
{
    Project project;
    QString error;
    if (!loadProject(projectPath, project, &error)) {
        err() << "cannot read " << projectPath << ": " << error << "\n";
        err().flush();
        return 1;
    }

    QTextStream out(stdout);
    for (const ScriptEntry &script : project.scripts) {
        if (!only.isEmpty() && script.name != only) continue;
        const GenResult gen = generateEnforce(script.graph, cat, builtins, project);
        out << "// " << script.folder << "/" << script.name << ".c\n";
        out << gen.code;
        if (!gen.code.endsWith(QLatin1Char('\n'))) out << "\n";
        for (const QString &w : gen.warnings) err() << "warning: " << w << "\n";
        out << "\n";
    }
    out.flush();
    err().flush();
    return 0;
}

QString valueAfter(const QStringList &args, const QString &flag)
{
    const int at = args.indexOf(flag);
    return at >= 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    const QString reference = valueAfter(args, QStringLiteral("--reference"));
    const QString project = valueAfter(args, QStringLiteral("--generate"));
    if (reference.isEmpty() && project.isEmpty()) {
        err() << "nodedoc --reference <out.md> [--catalog <catalog.json>]\n";
        err() << "nodedoc --generate <project.sdzn> [--script <name>] "
                 "[--catalog <catalog.json>]\n";
        err().flush();
        return 1;
    }

    QString catalogPath = valueAfter(args, QStringLiteral("--catalog"));
    if (catalogPath.isEmpty()) catalogPath = QStringLiteral("resources/catalog.json");
    Catalog cat;
    if (!cat.load(catalogPath)) {
        err() << "cannot load " << catalogPath << ": " << cat.error() << "\n";
        err().flush();
        return 1;
    }
    Builtins builtins;

    if (!reference.isEmpty()) return writeReference(reference, cat, builtins);
    return generate(project, valueAfter(args, QStringLiteral("--script")), cat, builtins);
}
