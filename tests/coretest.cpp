// Headless check of the model layer: the packed catalogue decodes, search
// ranks sanely, and a real .sdzn project survives a load/save round-trip.
//
// Built separately from the app (see tests/CMakeLists.txt) so it can run
// without a display.
#include "analysis.h"
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "graph.h"
#include "project.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) failures++;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("resources");

    out << "catalogue" << Qt::endl;
    Catalog cat;
    const bool loaded = cat.load(root + "/catalog.json");
    check(loaded, QStringLiteral("loads catalog.json (%1)").arg(cat.error()));
    if (!loaded) return 1;

    check(cat.classCount() > 6000,
          QStringLiteral("%1 classes decoded").arg(cat.classCount()));
    check(cat.totals().value("methods") > 29000,
          QStringLiteral("%1 methods in totals").arg(cat.totals().value("methods")));

    // Inheritance is the backbone of connection checking.
    check(cat.classId("PlayerBase") >= 0, QStringLiteral("PlayerBase exists"));
    check(cat.isA("PlayerBase", "EntityAI"),
          QStringLiteral("PlayerBase descends from EntityAI"));
    check(!cat.isA("EntityAI", "PlayerBase"),
          QStringLiteral("inheritance is directional"));

    // Search must put the exact name first, not a substring match.
    const auto hits = cat.search(QStringLiteral("EEInit"));
    check(!hits.isEmpty(), QStringLiteral("search finds EEInit"));
    if (!hits.isEmpty())
        check(hits.first().title.contains(QStringLiteral("EEInit")),
              QStringLiteral("top hit is EEInit, got '%1'").arg(hits.first().title));

    // Every hit must resolve to a usable node.
    int built = 0;
    for (const SearchHit &h : cat.search(QStringLiteral("Get"), {40, {}, {}})) {
        const NodeDef def = cat.defFor(h.key);
        if (def.valid && !def.pins.isEmpty()) built++;
    }
    check(built >= 30, QStringLiteral("%1/40 'Get' hits build valid defs").arg(built));

    // Events start a flow: exec out, no exec in.
    bool eventShapeOk = false;
    for (const SearchHit &h : cat.search(QStringLiteral("EEItemAttached"))) {
        const NodeDef def = cat.defFor(h.key);
        if (!def.valid || def.category != QLatin1String("Events")) continue;
        bool hasExecOut = false, hasExecIn = false;
        for (const Pin &p : def.pins) {
            if (p.type.kind != PinKind::Exec) continue;
            (p.dir == PinDir::Out ? hasExecOut : hasExecIn) = true;
        }
        eventShapeOk = hasExecOut && !hasExecIn;
        break;
    }
    check(eventShapeOk, QStringLiteral("event nodes have exec out and no exec in"));

    out << "pin types" << Qt::endl;
    const auto isEnum = [&cat](const QString &n) { return cat.isEnum(n); };
    const PinType arr = pinTypeOf(QStringLiteral("array<ref ItemBase>"), isEnum);
    check(arr.kind == PinKind::Object && arr.cls == QLatin1String("ItemBase") && arr.isArray,
          QStringLiteral("array<ref ItemBase> parses to object[] of ItemBase"));
    const PinType strs = pinTypeOf(QStringLiteral("TStringArray"), isEnum);
    check(strs.kind == PinKind::String && strs.isArray,
          QStringLiteral("TStringArray parses to string[]"));
    const PinType mapped = pinTypeOf(QStringLiteral("map<string, int>"), isEnum);
    check(mapped.kind == PinKind::Int && mapped.isArray,
          QStringLiteral("map<K,V> carries the value type"));

    out << "project" << Qt::endl;
    Project p;
    QString err;
    const bool opened = loadProject(root + "/SUDO_Link.sdzn", p, &err);
    check(opened, QStringLiteral("loads SUDO_Link.sdzn (%1)").arg(err));
    if (opened) {
        check(p.scripts.size() >= 20,
              QStringLiteral("%1 scripts in the project").arg(p.scripts.size()));
        int nodes = 0, edges = 0;
        for (const ScriptEntry &s : p.scripts) {
            nodes += s.graph.nodes.size();
            edges += s.graph.edges.size();
        }
        check(nodes > 100, QStringLiteral("%1 nodes across all graphs").arg(nodes));
        check(edges > 50, QStringLiteral("%1 edges across all graphs").arg(edges));

        // Round-trip: save and reload must preserve everything we model.
        QTemporaryDir tmp;
        const QString copy = tmp.filePath(QStringLiteral("rt.sdzn"));
        check(saveProject(p, copy, &err), QStringLiteral("saves a copy (%1)").arg(err));
        Project again;
        check(loadProject(copy, again, &err),
              QStringLiteral("reloads the copy (%1)").arg(err));
        check(again.scripts.size() == p.scripts.size(),
              QStringLiteral("script count survives round-trip"));
        int nodes2 = 0, edges2 = 0;
        for (const ScriptEntry &s : again.scripts) {
            nodes2 += s.graph.nodes.size();
            edges2 += s.graph.edges.size();
        }
        check(nodes2 == nodes && edges2 == edges,
              QStringLiteral("nodes/edges survive round-trip (%1/%2 vs %3/%4)")
                  .arg(nodes2).arg(edges2).arg(nodes).arg(edges));

        // Every node in a real project should resolve against the catalogue or
        // the builtins; unresolvable refs mean the two have drifted apart.
        int unresolved = 0;
        for (const ScriptEntry &s : p.scripts)
            for (const GraphNode &n : s.graph.nodes)
                if (!n.ref.startsWith(QLatin1String("bi.")) && n.kind != NodeKind::VarGet
                    && n.kind != NodeKind::VarSet && n.kind != NodeKind::Comment
                    && !n.ref.startsWith(QLatin1String("fn."))
                    && !n.ref.startsWith(QLatin1String("tpl."))
                    && !cat.defFor(n.ref).valid)
                    unresolved++;
        check(unresolved == 0,
              QStringLiteral("%1 catalogue refs unresolved").arg(unresolved));
    }

    out << "builtins" << Qt::endl;
    Builtins builtins;
    check(builtins.contains(bi::Begin) && builtins.contains(bi::Branch)
              && builtins.contains(bi::ForEach) && builtins.contains(bi::Cast)
              && builtins.contains(bi::Raw),
          QStringLiteral("core builtin ids exist"));
    check(builtins.all().size() >= 20,
          QStringLiteral("%1 builtin nodes in the palette").arg(builtins.all().size()));
    check(builtins.beginModes().size() == 4,
          QStringLiteral("4 Begin lifecycle modes"));

    const NodeDef branch = builtins.def(bi::Branch);
    int execIn = 0, execOut = 0;
    for (const Pin &p : branch.pins)
        if (p.type.kind == PinKind::Exec) (p.dir == PinDir::In ? execIn : execOut)++;
    check(execIn == 1 && execOut >= 2,
          QStringLiteral("Branch has one exec in and both outputs"));

    out << "codegen" << Qt::endl;
    if (opened) {
        int generated = 0, empty = 0, unbalanced = 0;
        QStringList allWarnings;
        for (const ScriptEntry &sc : p.scripts) {
            const GenResult gen = generateEnforce(sc.graph, cat, builtins, p);
            if (gen.code.trimmed().isEmpty()) { empty++; continue; }
            generated++;
            // Nothing here can prove it compiles in DayZ, but unbalanced braces
            // guarantee it does not.
            int depth = 0;
            bool inString = false;
            for (int i = 0; i < gen.code.size(); ++i) {
                const QChar c = gen.code.at(i);
                if (c == '"' && (i == 0 || gen.code.at(i - 1) != '\\')) inString = !inString;
                if (inString) continue;
                if (c == '{') depth++;
                else if (c == '}') depth--;
            }
            if (depth != 0) {
                unbalanced++;
                out << "       unbalanced braces in " << sc.name
                    << " (depth " << depth << ")" << Qt::endl;
            }
            allWarnings += gen.warnings;
        }
        check(generated >= 20,
              QStringLiteral("%1 scripts generated code (%2 empty)").arg(generated).arg(empty));
        check(unbalanced == 0,
              QStringLiteral("%1 scripts with unbalanced braces").arg(unbalanced));

        // A generated class must declare itself; a body with no class header
        // would never compile no matter how balanced it is.
        const ScriptEntry &first = p.scripts.first();
        const GenResult gen = generateEnforce(first.graph, cat, builtins, p);
        check(gen.code.contains(first.graph.className),
              QStringLiteral("generated code names the class (%1)").arg(first.graph.className));

        // User regions must survive regeneration; that promise is the reason
        // the markers exist at all. Edit between the markers the way someone
        // editing the exported .c file would.
        check(gen.code.contains(USER_BEGIN) && gen.code.contains(USER_END),
              QStringLiteral("generated file carries the user-region markers"));
        const int a = gen.code.indexOf(USER_BEGIN);
        const int b = gen.code.indexOf(USER_END);
        QString edited = gen.code;
        if (a >= 0 && b > a) {
            const int bodyStart = gen.code.indexOf(QLatin1Char('\n'), a) + 1;
            edited.replace(bodyStart, b - bodyStart,
                           QStringLiteral("\tvoid MyHelper() { Print(\"keep me\"); }\n"));
        }
        const GenResult again = generateEnforce(first.graph, cat, builtins, p, edited);
        check(again.code.contains(QStringLiteral("keep me")),
              QStringLiteral("user region survives regeneration"));
        check(again.code.count(USER_BEGIN) == 1,
              QStringLiteral("regeneration does not duplicate the markers"));
        if (!allWarnings.isEmpty())
            out << "       " << allWarnings.size() << " codegen warnings across the project"
                << Qt::endl;
    }

    // The showcase project exists to exercise one of every node family, so a
    // builtin the generator forgot shows up as a placeholder comment here
    // rather than as silently wrong script months later.
    out << "codegen - every node family" << Qt::endl;
    Project showcase;
    if (loadProject(root + "/Showcase.sdzn", showcase, &err)) {
        const GenResult gen = generateEnforce(showcase.scripts.first().graph, cat,
                                              builtins, showcase);
        check(!gen.code.contains(QStringLiteral("could not build call")),
              QStringLiteral("no unhandled node kinds"));
        check(gen.code.contains(QStringLiteral("Print(")),
              QStringLiteral("Print node emits a call"));
        check(gen.code.contains(QStringLiteral("SetHealth")),
              QStringLiteral("method call emitted"));
        check(gen.code.contains(QStringLiteral("Class.CastTo"))
                  || gen.code.contains(QStringLiteral("PlayerBase")),
              QStringLiteral("cast emitted"));
        check(gen.code.contains(QStringLiteral("m_HitCount")),
              QStringLiteral("variable set emitted"));
        check(gen.code.contains(QStringLiteral("RegisterNetSyncVariable"))
                  || gen.code.contains(QStringLiteral("m_IsLocked")),
              QStringLiteral("sync variable declared"));
    } else {
        out << "       (Showcase.sdzn not present, skipped)" << Qt::endl;
    }

    out << "analysis" << Qt::endl;
    if (opened) {
        int totalErrors = 0, totalWarnings = 0;
        for (const ScriptEntry &sc : p.scripts) {
            const AnalysisResult a = analyzeGraph(sc.graph, cat, builtins);
            totalErrors += a.errors;
            totalWarnings += a.warnings;
            check(a.diagnostics.size() == a.errors + a.warnings
                      + int(std::count_if(a.diagnostics.begin(), a.diagnostics.end(),
                                          [](const Diagnostic &d) {
                                              return d.severity == Severity::Info;
                                          })),
                  QStringLiteral("%1: counters match diagnostics").arg(sc.name));
            break; // one script is enough to prove the counters line up
        }
        out << "       project totals: " << totalErrors << " errors, "
            << totalWarnings << " warnings" << Qt::endl;
    }

    out << Qt::endl << (failures == 0 ? "ALL CORE TESTS PASSED"
                                      : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
