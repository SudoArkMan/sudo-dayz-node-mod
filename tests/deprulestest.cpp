// Verification harness for the three dependency rules, DZ314/315/316.
//
// Written so the body of dependencyRules() below drops into tests/depstest.cpp
// unchanged once that target also compiles ../src/analysis.cpp.
#include "analysis.h"
#include "builtins.h"
#include "catalog.h"
#include "graph.h"
#include "moddeps.h"
#include "project.h"

#include <QCoreApplication>
#include <QSet>
#include <QTextStream>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) failures++;
}

static QSet<QString> rulesOf(const AnalysisResult &a)
{
    QSet<QString> r;
    for (const Diagnostic &d : a.diagnostics) r.insert(d.rule);
    return r;
}

static void dump(const AnalysisResult &a)
{
    QTextStream out(stdout);
    for (const Diagnostic &d : a.diagnostics) {
        if (d.rule != QLatin1String("DZ314") && d.rule != QLatin1String("DZ315")
            && d.rule != QLatin1String("DZ316"))
            continue;
        out << "         [" << d.rule << "] "
            << (d.severity == Severity::Error ? "error  " : "warning") << " on node "
            << d.nodeId << Qt::endl;
        out << "           " << d.message << Qt::endl;
        out << "           hint: " << d.hint << Qt::endl;
    }
}

// A graph holding one COT call. The node is an Event, which makes it an entry,
// so it is reachable without wiring anything to it.
static Graph cotGraph()
{
    Graph g;
    g.className = QStringLiteral("MyItem");
    g.baseClass = QStringLiteral("ItemBase");
    GraphNode n;
    n.id = QStringLiteral("n1");
    n.kind = NodeKind::Event;
    n.ref = QStringLiteral("dep.JM_COT_Scripts.JMModuleManager.GetModule");
    g.nodes.append(n);
    return g;
}

static void addRaw(Graph &g, const QString &code)
{
    GraphNode raw;
    raw.id = QStringLiteral("n2");
    raw.ref = bi::Raw;
    raw.opts.insert(QStringLiteral("code"), code);
    g.nodes.append(raw);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("resources");

    Catalog cat;
    if (!cat.load(root + "/catalog.json")) {
        out << "cannot load catalogue: " << cat.error() << Qt::endl;
        return 1;
    }
    Builtins builtins;

    const ModDependency cot = knownDependency(QStringLiteral("JM_COT_Scripts"));
    check(cot.isValid() && cot.loadedDefine == QLatin1String("JM_COT_LOADED")
              && cot.requires.contains(QStringLiteral("JM_CF_Scripts")),
          QStringLiteral("the COT preset carries the facts the rules read"));

    // The mod under test requires DZ_Data and nothing else, which is what a
    // freshly scaffolded config.cpp says.
    DependencyContext base;
    base.deps = {cot};
    base.configRead = true;
    base.patchClass = QStringLiteral("MyMod_Scripts");
    base.configPath = QStringLiteral("P:/MyMod/config.cpp");
    base.declaredAddons = {QStringLiteral("\"DZ_Data\"")};  // quoted, as config.cpp writes it

    out << Qt::endl << "1. COT called, COT not required" << Qt::endl;
    {
        const AnalysisResult a = analyzeGraph(cotGraph(), cat, builtins, QString(), base);
        const QSet<QString> r = rulesOf(a);
        check(r.contains(QStringLiteral("DZ314")),
              QStringLiteral("DZ314 fires: JM_COT_Scripts is not in requiredAddons"));
        check(r.contains(QStringLiteral("DZ316")),
              QStringLiteral("DZ316 fires: COT's own JM_CF_Scripts is missing"));
        check(!r.contains(QStringLiteral("DZ315")),
              QStringLiteral("DZ315 silent: this dependency is not optional"));
        dump(a);
    }

    out << Qt::endl << "2. one finding per dependency, not one per node" << Qt::endl;
    {
        Graph g = cotGraph();
        for (int i = 0; i < 6; i++) {
            GraphNode n;
            n.id = QStringLiteral("extra%1").arg(i);
            n.kind = NodeKind::Event;
            n.ref = QStringLiteral("dep.JM_COT_Scripts.JMPlayerInstance.SetPermissions");
            g.nodes.append(n);
        }
        const AnalysisResult a = analyzeGraph(g, cat, builtins, QString(), base);
        int d314 = 0;
        for (const Diagnostic &d : a.diagnostics)
            if (d.rule == QLatin1String("DZ314")) d314++;
        check(d314 == 1, QStringLiteral("seven COT nodes give one DZ314, got %1").arg(d314));
    }

    out << Qt::endl << "3. optional and unguarded" << Qt::endl;
    {
        DependencyContext c = base;
        c.deps.first().optional = true;
        c.declaredAddons = {QStringLiteral("DZ_Data"), QStringLiteral("JM_COT_Scripts"),
                            QStringLiteral("JM_CF_Scripts")};
        const AnalysisResult a = analyzeGraph(cotGraph(), cat, builtins, QString(), c);
        const QSet<QString> r = rulesOf(a);
        check(r.contains(QStringLiteral("DZ315")),
              QStringLiteral("DZ315 fires: nothing sits behind #ifdef JM_COT_LOADED"));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("the config rules are silent, everything is declared"));
        dump(a);
    }

    out << Qt::endl << "4. optional and guarded" << Qt::endl;
    {
        const QStringList forms = {
            QStringLiteral("#ifdef JM_COT_LOADED\n\tPrint(\"cot\");\n#endif\n"),
            QStringLiteral("#if defined(JM_COT_LOADED)\n\tPrint(\"cot\");\n#endif\n"),
            QStringLiteral("  #  ifdef   JM_COT_LOADED\n#endif\n"),
            QStringLiteral("#ifndef JM_COT_LOADED\n\treturn;\n#endif\n"),
        };
        for (const QString &form : forms) {
            DependencyContext c = base;
            c.deps.first().optional = true;
            c.declaredAddons = {QStringLiteral("DZ_Data"), QStringLiteral("JM_COT_Scripts"),
                                QStringLiteral("JM_CF_Scripts")};
            Graph g = cotGraph();
            addRaw(g, form);
            const AnalysisResult a = analyzeGraph(g, cat, builtins, QString(), c);
            check(!rulesOf(a).contains(QStringLiteral("DZ315")),
                  QStringLiteral("DZ315 silent for `%1`")
                      .arg(form.split(QLatin1Char('\n')).first().trimmed()));
        }
        // A mention that is not a guard must not count as one.
        DependencyContext c = base;
        c.deps.first().optional = true;
        c.declaredAddons = {QStringLiteral("DZ_Data"), QStringLiteral("JM_COT_Scripts"),
                            QStringLiteral("JM_CF_Scripts")};
        Graph g = cotGraph();
        addRaw(g, QStringLiteral("// JM_COT_LOADED is the define COT sets\n"));
        check(rulesOf(analyzeGraph(g, cat, builtins, QString(), c))
                  .contains(QStringLiteral("DZ315")),
              QStringLiteral("DZ315 still fires when the name is only in a comment"));
    }

    out << Qt::endl << "5. everything declared" << Qt::endl;
    {
        DependencyContext c = base;
        // Case must not matter: config.cpp is not consistent about it.
        c.declaredAddons = {QStringLiteral("DZ_Data"), QStringLiteral("jm_cot_scripts"),
                            QStringLiteral("JM_CF_SCRIPTS")};
        const QSet<QString> r = rulesOf(analyzeGraph(cotGraph(), cat, builtins, QString(), c));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("both config rules silent, and the compare ignores case"));
    }

    out << Qt::endl << "6. config.cpp never read" << Qt::endl;
    {
        DependencyContext c = base;
        c.configRead = false;
        c.declaredAddons.clear();
        const QSet<QString> r = rulesOf(analyzeGraph(cotGraph(), cat, builtins, QString(), c));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("the config rules stay quiet rather than guessing"));
    }

    out << Qt::endl << "7. cases that are not a dependency finding" << Qt::endl;
    {
        // A vanilla node.
        Graph g;
        g.className = QStringLiteral("MyItem");
        g.baseClass = QStringLiteral("ItemBase");
        GraphNode n;
        n.id = QStringLiteral("n1");
        n.kind = NodeKind::Event;
        n.ref = QStringLiteral("m1204");
        g.nodes.append(n);
        QSet<QString> r = rulesOf(analyzeGraph(g, cat, builtins, QString(), base));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ315"))
                  && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("a vanilla node carries no dependency finding"));

        // A key naming a mod the project does not list.
        Graph other = cotGraph();
        other.nodes.first().ref = QStringLiteral("dep.Expansion_Scripts.ExpansionMarker.Get");
        r = rulesOf(analyzeGraph(other, cat, builtins, QString(), base));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("an unlisted dependency is DZ101's finding, not ours"));

        // A COT node nothing reaches generates no code.
        Graph dead = cotGraph();
        dead.nodes.first().kind = NodeKind::Call;
        r = rulesOf(analyzeGraph(dead, cat, builtins, QString(), base));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("dead code is not a missing dependency"));

        // No dependencies configured at all.
        r = rulesOf(analyzeGraph(cotGraph(), cat, builtins));
        check(!r.contains(QStringLiteral("DZ314")) && !r.contains(QStringLiteral("DZ315"))
                  && !r.contains(QStringLiteral("DZ316")),
              QStringLiteral("an empty context reports nothing"));
    }

    out << Qt::endl << "8. resources/SUDO_Link.sdzn stays at zero" << Qt::endl;
    {
        Project p;
        QString err;
        if (!loadProject(root + "/SUDO_Link.sdzn", p, &err)) {
            check(false, QStringLiteral("loads SUDO_Link.sdzn (%1)").arg(err));
        } else {
            DependencyContext c = base;
            // Both presets live, and both rules that can blame the config armed,
            // so this is silence by the rules rather than by an empty context.
            c.deps = knownDependencies();
            c.deps[1].optional = true;
            c.declaredAddons.clear();
            // The same project read twice: once with the dependency rules armed
            // and once with nothing configured. Any difference between the two
            // is something this change added.
            const auto tally = [&](const DependencyContext &dc) {
                QMap<QString, int> byRule;
                for (const ScriptEntry &sc : p.scripts)
                    for (const Diagnostic &d : analyzeGraph(sc.graph, cat, builtins, sc.id, dc)
                                                   .diagnostics)
                        byRule[d.rule]++;
                return byRule;
            };
            const QMap<QString, int> armed = tally(c);
            const QMap<QString, int> off = tally(DependencyContext());

            int dep = 0;
            for (const QString &r : {QStringLiteral("DZ314"), QStringLiteral("DZ315"),
                                     QStringLiteral("DZ316")})
                dep += armed.value(r);
            check(dep == 0,
                  QStringLiteral("%1 scripts, 0 dependency findings").arg(p.scripts.size()));
            check(armed == off,
                  QStringLiteral("armed and unarmed give the same findings"));

            QStringList shape;
            for (auto it = armed.constBegin(); it != armed.constEnd(); ++it)
                shape << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
            out << "       findings by rule: " << shape.join(QLatin1Char(' ')) << Qt::endl;
        }
    }

    out << Qt::endl << (failures == 0 ? QStringLiteral("DEPENDENCY RULES OK")
                                      : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
