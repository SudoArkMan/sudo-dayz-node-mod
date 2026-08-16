// Headless check of the model layer: the packed catalogue decodes, search
// ranks sanely, and a real .sdzn project survives a load/save round-trip.
//
// Built separately from the app (see tests/CMakeLists.txt) so it can run
// without a display.
#include "analysis.h"
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "document.h"
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

    // Two words. The box used to match the whole trimmed string with one
    // contains(), and nothing in the catalogue holds a space, so every query
    // anyone types the way they say it came back empty.
    {
        const auto topOf = [&cat](const QString &query) {
            const QVector<SearchHit> h = cat.search(query);
            return h.isEmpty() ? QString() : h.first().title;
        };
        check(cat.search(QStringLiteral("set health")).size() > 0,
              QStringLiteral("'set health' finds something"));
        check(topOf(QStringLiteral("set health")) == QStringLiteral("SetHealth"),
              QStringLiteral("'set health' puts SetHealth on top, got '%1'")
                  .arg(topOf(QStringLiteral("set health"))));
        // Spacing and case are how a person types, not how a name is spelled.
        check(topOf(QStringLiteral("  Set   Health  ")) == QStringLiteral("SetHealth"),
              QStringLiteral("extra spaces and capitals do not change the answer"));
        // Words spread across the name and the class that declares it.
        bool timerRun = false;
        for (const SearchHit &h : cat.search(QStringLiteral("timer run")))
            if (h.subtitle == QLatin1String("Timer") && h.title == QLatin1String("Run"))
                timerRun = true;
        check(timerRun, QStringLiteral("'timer run' finds Timer::Run"));
        // A word that is nowhere on the row takes the row out, so two words are
        // narrower than one rather than wider.
        check(cat.search(QStringLiteral("set health")).size()
                  < cat.search(QStringLiteral("set")).size(),
              QStringLiteral("adding a word narrows the result"));
        check(cat.search(QStringLiteral("set nosuchwordanywhere")).isEmpty(),
              QStringLiteral("every word has to match, not just one"));
        // A single word must still take the original path, because the importer
        // and the lowering resolve every method name through here.
        check(topOf(QStringLiteral("EEInit")) == QStringLiteral("Event EEInit"),
              QStringLiteral("one word still ranks the way it did"));
    }

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

    // ---------------------------------------------------------------- access
    //
    // The bug this covers: the palette offered TimerBase::SetRunning, the user
    // wired it, and the graph generated `m_Timer.SetRunning(false);` from a
    // MissionServer, which does not compile. Nothing warned.
    out << "access" << Qt::endl;
    {
        // Every entry the catalogue holds is public or protected. Private is
        // not a flag here because a private method is dropped before it lands.
        int methodTotal = cat.totals().value(QStringLiteral("methods"));
        int protectedTotal = 0, reachableFromMission = 0, reachableFromOwner = 0;
        for (int i = 0; i < methodTotal; ++i) {
            const MethodSig m = cat.method(QStringLiteral("m%1").arg(i));
            if (!m.valid || !(m.flags & flag::Protected)) continue;
            protectedTotal++;
            if (cat.accessAllowed(m.owner, m.flags, QStringLiteral("MissionServer")))
                reachableFromMission++;
            if (cat.accessAllowed(m.owner, m.flags, m.owner)) reachableFromOwner++;
        }
        out << "       protected methods:        " << protectedTotal << Qt::endl;
        out << "       reachable from their own class: " << reachableFromOwner
            << Qt::endl;
        out << "       reachable from MissionServer:   " << reachableFromMission
            << Qt::endl;
        check(protectedTotal > 2000,
              QStringLiteral("%1 methods carry the protected flag").arg(protectedTotal));
        check(reachableFromOwner == protectedTotal,
              QStringLiteral("every protected method is reachable from its own class"));
        check(reachableFromMission < protectedTotal / 4,
              QStringLiteral("most of them are out of reach from an unrelated class "
                             "(%1 of %2)").arg(reachableFromMission).arg(protectedTotal));

        // A private method is not offered as a hidden node, it is not there at
        // all. func::SetInstance is `private static void SetInstance(func fn)`
        // in 1_core, and it is the whole entry that is missing rather than a
        // flag on it.
        bool privateFound = false;
        for (const SearchHit &h : cat.search(QStringLiteral("SetInstance")))
            if (h.subtitle == QLatin1String("func")) privateFound = true;
        check(!privateFound, QStringLiteral("a private method is not in the catalogue"));

        // An event row is titled "Event <name>", the same two spellings
        // codegen's ancestryDeclares has to accept. Timer::Run was one of them
        // until event-ness stopped being decided by method name; eventstest
        // covers that directly. What matters here is that Run is public and
        // stays offered.
        const auto titleMatches = [](const QString &title, const QString &name) {
            return title == name || title == QStringLiteral("Event ") + name;
        };
        const auto keyOf = [&cat, &titleMatches](const QString &owner,
                                                 const QString &name) {
            SearchOptions opts;
            opts.limit = 200;
            for (const SearchHit &h : cat.search(name, opts))
                if (h.subtitle == owner && titleMatches(h.title, name)) return h.key;
            return QString();
        };
        const QString setRunning = keyOf(QStringLiteral("TimerBase"),
                                         QStringLiteral("SetRunning"));
        const QString run = keyOf(QStringLiteral("Timer"), QStringLiteral("Run"));
        check(!setRunning.isEmpty(), QStringLiteral("TimerBase::SetRunning is catalogued"));
        check(!run.isEmpty(), QStringLiteral("Timer::Run is catalogued"));

        const MethodSig sr = cat.method(setRunning);
        const MethodSig rn = cat.method(run);
        check(sr.valid && (sr.flags & flag::Protected),
              QStringLiteral("SetRunning is recorded as protected"));
        check(rn.valid && !(rn.flags & flag::Protected),
              QStringLiteral("Run is recorded as public"));

        // The palette's own question, asked the way the palette asks it.
        const auto offers = [&cat, &titleMatches](const QString &self,
                                                  const QString &query,
                                                  const QString &owner,
                                                  const QString &name) {
            SearchOptions opts;
            opts.limit = 200;
            opts.selfClass = self;
            opts.respectAccess = true;
            for (const SearchHit &h : cat.search(query, opts))
                if (h.subtitle == owner && titleMatches(h.title, name)) return true;
            return false;
        };
        check(!offers(QStringLiteral("MissionServer"), QStringLiteral("SetRunning"),
                      QStringLiteral("TimerBase"), QStringLiteral("SetRunning")),
              QStringLiteral("SetRunning is not offered to a MissionServer graph"));
        check(offers(QStringLiteral("Timer"), QStringLiteral("SetRunning"),
                     QStringLiteral("TimerBase"), QStringLiteral("SetRunning")),
              QStringLiteral("SetRunning is still offered to a graph that inherits it"));
        check(offers(QStringLiteral("MissionServer"), QStringLiteral("Run"),
                     QStringLiteral("Timer"), QStringLiteral("Run")),
              QStringLiteral("Run is still offered, and still public"));
        // A public method on the same class must not be caught by the filter.
        check(offers(QStringLiteral("MissionServer"), QStringLiteral("IsRunning"),
                     QStringLiteral("TimerBase"), QStringLiteral("IsRunning")),
              QStringLiteral("a public method on the same class is untouched"));

        // And the graph the user actually built: a Timer being driven from a
        // modded MissionServer.
        Builtins accessBuiltins;
        Graph mission;
        mission.className = QStringLiteral("MissionServer");
        mission.baseClass.clear();
        mission.modded = true;
        GraphNode bad;
        bad.id = QStringLiteral("nBad");
        bad.kind = NodeKind::Call;
        bad.ref = setRunning;
        GraphNode good;
        good.id = QStringLiteral("nGood");
        good.kind = NodeKind::Call;
        good.ref = run;
        mission.nodes << bad << good;

        const AnalysisResult missionResult = analyzeGraph(mission, cat, accessBuiltins);
        int flagged = 0;
        QString message;
        for (const Diagnostic &d : missionResult.diagnostics) {
            if (d.rule != QLatin1String("DZ118")) continue;
            flagged++;
            if (d.nodeId == QLatin1String("nBad")) message = d.message;
        }
        check(flagged == 1,
              QStringLiteral("one DZ118 on the MissionServer graph, got %1").arg(flagged));
        check(message.contains(QStringLiteral("SetRunning"))
                  && message.contains(QStringLiteral("TimerBase"))
                  && message.contains(QStringLiteral("MissionServer")),
              QStringLiteral("the warning names the node, its class and ours"));
        check(missionResult.warnings > 0,
              QStringLiteral("it lands in the warnings, not the errors"));

        // The same call from a class that does inherit it is correct Enforce
        // and must stay quiet.
        Graph timer = mission;
        timer.className = QStringLiteral("Timer");
        int quiet = 0;
        for (const Diagnostic &d : analyzeGraph(timer, cat, accessBuiltins).diagnostics)
            if (d.rule == QLatin1String("DZ118")) quiet++;
        check(quiet == 0, QStringLiteral("a modded Timer calling it is not warned about"));
    }

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

        // The author's own blank lines, comments and indentation ride in
        // GraphNode::opts. A blank line is the value that breaks a naive
        // encoding: "one blank line above this statement" is stored as a bare
        // newline, and an empty string would be indistinguishable from a key
        // that is not there at all.
        {
            Project fmt;
            ScriptEntry entry;
            entry.name = QStringLiteral("SUDO_Fmt");
            entry.graph.className = QStringLiteral("SUDO_Fmt");
            GraphNode n;
            n.id = QStringLiteral("n1");
            n.kind = NodeKind::Builtin;
            n.ref = bi::Print;
            n.opts.insert(nodefmt::keyBase(), QStringLiteral("    "));
            n.opts.insert(nodefmt::keyUnit(), QStringLiteral("  "));
            n.opts.insert(nodefmt::keyBefore(), QStringLiteral("\n\t\t// why this runs first\n"));
            n.opts.insert(nodefmt::keyTrailing(), QStringLiteral(" // and this one after"));
            n.opts.insert(nodefmt::keyEnd(), QStringLiteral("\n"));
            entry.graph.nodes.append(n);
            fmt.scripts.append(entry);

            const QString path = tmp.filePath(QStringLiteral("fmt.sdzn"));
            check(saveProject(fmt, path, &err),
                  QStringLiteral("saves a project carrying the author's formatting (%1)").arg(err));
            Project back;
            check(loadProject(path, back, &err),
                  QStringLiteral("reloads it (%1)").arg(err));
            const GraphNode *got = back.scripts.isEmpty() ? nullptr
                                                          : back.scripts.first().graph.node(
                                                                QStringLiteral("n1"));
            check(got && got->opts == n.opts,
                  QStringLiteral("every formatting key comes back with the value it went in with"));
            if (got)
                check(got->opts.value(nodefmt::keyEnd()) == QStringLiteral("\n")
                          && nodefmt::lines(got->opts.value(nodefmt::keyEnd())).size() == 1,
                      QStringLiteral("one blank line survives as one blank line"));
            check(nodefmt::lines(QString()).isEmpty()
                      && nodefmt::store({QString()}) == QStringLiteral("\n")
                      && nodefmt::lines(QStringLiteral("\n")) == QStringList{QString()},
                  QStringLiteral("no lines and one blank line are told apart"));
            check(nodefmt::isCommentaryOnly(QStringLiteral("\t// note"))
                      && nodefmt::isCommentaryOnly(QStringLiteral("/* two\n   lines */"))
                      && !nodefmt::isCommentaryOnly(QStringLiteral("Print(1);"))
                      && !nodefmt::isCommentaryOnly(QStringLiteral("/* never closed")),
                  QStringLiteral("only whitespace and comments are accepted as commentary"));
        }
    }

    if (opened) {
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

    // ------------------------------------- layout fields are not a place for code
    //
    // graph.h states the invariant: the graph must not become a second place to
    // hide script. It used to be enforced where a body was read and nowhere
    // else, which was enough only while the importer was the sole writer. A
    // .sdzn is a file anyone can hand you and the mod browser has made opening
    // other people's work an ordinary thing to do, so the same rule is checked
    // where the text leaves the graph for the user's own .c.
    out << "layout fields" << Qt::endl;
    {
        check(nodefmt::isValidValue(nodefmt::keyBase(), QStringLiteral("\t\t"))
                  && !nodefmt::isValidValue(nodefmt::keyBase(), QStringLiteral("x")),
              QStringLiteral("an indent field takes whitespace and nothing else"));
        check(nodefmt::isValidValue(nodefmt::keyBefore(), QStringLiteral("// note\n"))
                  && !nodefmt::isValidValue(nodefmt::keyBefore(),
                                            QStringLiteral("GetGame().RequestExit(0);\n")),
              QStringLiteral("lines above a node are commentary or nothing"));
        // A trailing sits behind code already on the line, so a newline in it
        // would put what follows on a line of its own, past the statement.
        check(nodefmt::isValidValue(nodefmt::keyTrailing(), QStringLiteral(" // done"))
                  && !nodefmt::isValidValue(nodefmt::keyTrailing(),
                                            QStringLiteral(" // done\n\t\tRequestExit(0);")),
              QStringLiteral("a trailing comment stays on one line"));
        check(nodefmt::isValidValue(QStringLiteral("code"), QStringLiteral("Print(1);")),
              QStringLiteral("a key that is not a layout key is left to its owner"));

        // The whole point, end to end: a hostile project file generates a
        // script with none of its code in it, and says why.
        Graph g;
        g.className = QStringLiteral("SUDO_Hostile");
        g.baseClass = QStringLiteral("ItemBase");
        GraphNode begin;
        begin.id = QStringLiteral("evt");
        begin.kind = NodeKind::Builtin;
        begin.ref = bi::Begin;
        begin.opts.insert(QStringLiteral("noSuper"), QStringLiteral("1"));
        begin.opts.insert(nodefmt::keyEnd(), QStringLiteral("DeleteSafe();\n"));
        GraphNode pr;
        pr.id = QStringLiteral("p1");
        pr.kind = NodeKind::Builtin;
        pr.ref = bi::Print;
        pr.inputs.insert(QStringLiteral("value"), QStringLiteral("hello"));
        pr.opts.insert(nodefmt::keyBefore(), QStringLiteral("GetGame().RequestExit(0);\n"));
        pr.opts.insert(nodefmt::keyTrailing(), QStringLiteral(" // fine\n\t\tDestroy();"));
        g.nodes << begin << pr;
        g.edges.append({QStringLiteral("e1"),
                        {begin.id, QStringLiteral("exec")},
                        {pr.id, QStringLiteral("exec")},
                        {}});

        const GenResult hostile = generateEnforce(g, cat, builtins, p);
        check(!hostile.code.contains(QStringLiteral("RequestExit"))
                  && !hostile.code.contains(QStringLiteral("DeleteSafe"))
                  && !hostile.code.contains(QStringLiteral("Destroy()")),
              QStringLiteral("code hidden in a layout field is not written into the script"));
        check(hostile.code.contains(QStringLiteral("Print(")),
              QStringLiteral("the rest of the method is still generated"));
        int said = 0;
        for (const QString &w : hostile.warnings)
            if (w.contains(QStringLiteral("layout"))) said++;
        check(said >= 3,
              QStringLiteral("each field that was refused is reported (%1 warnings)").arg(said));
    }

    // ------------------------------------------------ the file's own line ending
    out << "line endings" << Qt::endl;
    {
        const QString crlf = QStringLiteral("\r\n");
        const QString lf = QStringLiteral("\n");

        check(nodefmt::fileEol(QStringLiteral("a\r\nb\r\n")) == crlf
                  && nodefmt::fileEol(QStringLiteral("a\nb\n")) == lf,
              QStringLiteral("a file that ends its lines one way answers with that ending"));
        // A file with nothing to restore takes the form everything else works
        // in, rather than no answer at all.
        check(nodefmt::fileEol(QString()) == lf && nodefmt::fileEol(QStringLiteral("a")) == lf,
              QStringLiteral("a file with no line break at all is a bare newline"));
        // The one case no single ending reproduces. Answering "the commoner one"
        // would rewrite every line carrying the other, which is the defect this
        // whole rule exists to stop, so it answers with nothing instead.
        check(nodefmt::fileEol(QStringLiteral("a\r\nb\nc\r\n")).isEmpty(),
              QStringLiteral("a file that mixes the two has no single answer"));
        // A carriage return inside a line is a byte of that line, not a break.
        check(nodefmt::fileEol(QStringLiteral("Print(\"a\rb\");\n")) == lf,
              QStringLiteral("a lone carriage return does not make a file CRLF"));

        check(nodefmt::withEol(QStringLiteral("a\nb\n"), crlf) == QStringLiteral("a\r\nb\r\n")
                  && nodefmt::withEol(QStringLiteral("a\nb\n"), lf) == QStringLiteral("a\nb\n")
                  && nodefmt::withEol(QStringLiteral("a\nb\n"), QString())
                         == QStringLiteral("a\nb\n"),
              QStringLiteral("the ending is put back only where there is one to put back"));
        check(nodefmt::withEol(nodefmt::withEol(QStringLiteral("a\nb\n"), crlf), crlf)
                  == QStringLiteral("a\r\nb\r\n"),
              QStringLiteral("applying it twice writes the same bytes as applying it once"));
        // The reason this walks the string rather than replacing the character:
        // a carriage return the author put inside a line must not become the
        // line's ending, and the line's own ending must still be written.
        check(nodefmt::withEol(QStringLiteral("Print(\"a\rb\");\n"), crlf)
                  == QStringLiteral("Print(\"a\rb\");\r\n"),
              QStringLiteral("a carriage return inside a line is left where it is"));
    }

    // A file anyone can hand you is checked when it is opened, not only when it
    // is generated: the user is still looking at the dialog that opened it
    // rather than at a mod that has already been built.
    out << "opening a .sdzn" << Qt::endl;
    {
        QTemporaryDir tmp;
        Project hostile;
        ScriptEntry entry;
        entry.id = QStringLiteral("s1");
        entry.name = QStringLiteral("SUDO_Hostile");
        entry.graph.className = QStringLiteral("SUDO_Hostile");
        GraphNode n;
        n.id = QStringLiteral("n1");
        n.kind = NodeKind::Builtin;
        n.ref = bi::Print;
        n.opts.insert(nodefmt::keyBefore(), QStringLiteral("GetGame().RequestExit(0);\n"));
        n.opts.insert(nodefmt::keyEnd(), QStringLiteral("// a real note\n"));
        n.opts.insert(nodefmt::keyUnit(), QStringLiteral("nope"));
        entry.graph.nodes.append(n);
        hostile.scripts.append(entry);

        const QString path = tmp.filePath(QStringLiteral("hostile.sdzn"));
        QString e2;
        check(saveProject(hostile, path, &e2), QStringLiteral("writes the file (%1)").arg(e2));
        Project back;
        check(loadProject(path, back, &e2), QStringLiteral("opens it (%1)").arg(e2));
        const GraphNode *got = back.scripts.isEmpty()
                                   ? nullptr
                                   : back.scripts.first().graph.node(QStringLiteral("n1"));
        check(got && !got->opts.contains(nodefmt::keyBefore())
                  && !got->opts.contains(nodefmt::keyUnit()),
              QStringLiteral("a layout field holding code is dropped as the file opens"));
        check(got && got->opts.value(nodefmt::keyEnd()) == QStringLiteral("// a real note\n"),
              QStringLiteral("and the fields beside it are left alone"));

        // The version field. Every build before this one wrote no field at all,
        // so a file without one is version 1; saving it makes it a v2 file,
        // because this build has just written every v2 field into it. A file
        // claiming a version this build does not know keeps its own number
        // rather than being relabelled as one this build could have produced.
        QFile written(path);
        check(written.open(QIODevice::ReadOnly), QStringLiteral("reads the saved file back"));
        QJsonObject root = QJsonDocument::fromJson(written.readAll()).object();
        written.close();
        check(root.value(QStringLiteral("formatVersion")).toInt() == kProjectFormatVersion,
              QStringLiteral("saving writes the version this build produces (%1)")
                  .arg(kProjectFormatVersion));

        // What an older build left behind: the same file with no field at all.
        root.remove(QStringLiteral("formatVersion"));
        const QString older = tmp.filePath(QStringLiteral("older.sdzn"));
        QFile out1(older);
        check(out1.open(QIODevice::WriteOnly), QStringLiteral("writes a file with no version"));
        out1.write(QJsonDocument(root).toJson());
        out1.close();
        Project old;
        // Loaded on its own line: the order the arguments of a call are
        // evaluated in is not fixed, so a message built in the same expression
        // reports whatever the field held before the load.
        const bool openedOld = loadProject(older, old, &e2);
        check(openedOld && old.formatVersion == 1,
              QStringLiteral("a file with no version field reads as version 1 (%1)")
                  .arg(old.formatVersion));
        const QString upgraded = tmp.filePath(QStringLiteral("upgraded.sdzn"));
        check(saveProject(old, upgraded, &e2), QStringLiteral("saves it again (%1)").arg(e2));
        Project after;
        check(loadProject(upgraded, after, &e2) && after.formatVersion == kProjectFormatVersion,
              QStringLiteral("and saving it with this build makes it a v%1 file")
                  .arg(kProjectFormatVersion));

        Project fromTheFuture = back;
        fromTheFuture.formatVersion = kProjectFormatVersion + 7;
        const QString later = tmp.filePath(QStringLiteral("later.sdzn"));
        check(saveProject(fromTheFuture, later, &e2),
              QStringLiteral("saves a project that claims a later version (%1)").arg(e2));
        Project reread;
        check(loadProject(later, reread, &e2) && reread.formatVersion == kProjectFormatVersion + 7,
              QStringLiteral("a version this build does not know is kept, not overwritten"));

        // The file's ending has to survive the project file, or a script opened
        // from a CRLF mod, saved into a .sdzn and exported the next day is
        // written back flattened. The three states are told apart on purpose:
        // a file that mixes its endings is not the same fact as one written
        // with bare newlines, and the notes the user saw are long gone by then.
        Project endings;
        for (const char *value : {"\r\n", "\n", ""}) {
            ScriptEntry s;
            s.id = nextId(QStringLiteral("s"));
            s.name = QStringLiteral("SUDO_Ends") + QString::number(endings.scripts.size());
            s.graph.className = s.name;
            s.graph.eol = QString::fromLatin1(value);
            endings.scripts.append(s);
        }
        const QString endingsPath = tmp.filePath(QStringLiteral("endings.sdzn"));
        check(saveProject(endings, endingsPath, &e2),
              QStringLiteral("saves a project holding all three endings (%1)").arg(e2));
        Project endingsBack;
        check(loadProject(endingsPath, endingsBack, &e2)
                  && endingsBack.scripts.size() == 3
                  && endingsBack.scripts.at(0).graph.eol == QLatin1String("\r\n")
                  && endingsBack.scripts.at(1).graph.eol == QLatin1String("\n")
                  && endingsBack.scripts.at(2).graph.eol.isEmpty(),
              QStringLiteral("and each script comes back with the ending its file had"));
    }

    out << "closing a tab" << Qt::endl;
    {
        // What a tab is, and therefore what closing one means. The bar is a view
        // of project().scripts and nothing else, so a close takes the script out
        // of the project rather than hiding a window onto something that carries
        // on existing. That makes it a removal, and the two things it must never
        // do are take a script it was not asked for and drop one for good.
        //
        // The read only mark is written here by hand under the key modlibrary.cpp
        // uses. That file is Qt Widgets and this target stops at Qt Gui, so the
        // marker cannot be called from here; the mark itself is a bool in
        // Graph::extra, which the .sdzn reader and writer carry through.
        Document doc;
        Project &p = doc.project();
        p.scripts.clear();
        const auto add = [&p](const char *id, const char *name, bool browsed) {
            ScriptEntry s;
            s.id = QLatin1String(id);
            s.name = QLatin1String(name);
            s.graph.className = s.name;
            GraphNode n;
            n.id = QStringLiteral("n_") + s.id;
            n.kind = NodeKind::Builtin;
            n.ref = bi::Print;
            s.graph.nodes.append(n);
            if (browsed) s.graph.extra.insert(QStringLiteral("readOnly"), true);
            p.scripts.append(s);
        };
        add("s1", "SUDO_Mine", false);
        add("s2", "CarScript", true);
        add("s3", "SUDO_Other", false);
        p.activeId = QStringLiteral("s2");

        int projectSignals = 0;
        QObject::connect(&doc, &Document::projectChanged,
                         [&projectSignals]() { projectSignals++; });

        check(!doc.closeScript(QStringLiteral("nope")),
              QStringLiteral("an id the project does not hold closes nothing"));
        check(p.scripts.size() == 3 && projectSignals == 0,
              QStringLiteral("and says nothing happened"));

        // The case the Mod Browser creates: a class read out of somebody else's
        // mod, sitting between two of the user's own.
        check(doc.closeScript(QStringLiteral("s2")),
              QStringLiteral("a browsed script closes"));
        check(p.scripts.size() == 2 && !p.script(QStringLiteral("s2")),
              QStringLiteral("and is out of the project"));
        check(projectSignals == 1, QStringLiteral("one projectChanged for one close"));

        const ScriptEntry *mine = p.script(QStringLiteral("s1"));
        const ScriptEntry *other = p.script(QStringLiteral("s3"));
        check(mine && other, QStringLiteral("the project's own scripts are still there"));
        check(mine && mine->name == QLatin1String("SUDO_Mine")
                  && mine->graph.nodes.size() == 1
                  && mine->graph.nodes.first().id == QLatin1String("n_s1"),
              QStringLiteral("and their graphs are untouched"));
        check(other && other->graph.nodes.size() == 1
                  && other->graph.nodes.first().id == QLatin1String("n_s3"),
              QStringLiteral("both of them, not just the one beside the gap"));

        // Closing the tab in front has to leave the editor pointing somewhere.
        // Project::active falls back to the first script when activeId names
        // nothing, so a dangling id looks like it works and stamps every undo
        // snapshot with a script no lookup can find.
        check(p.script(p.activeId) != nullptr,
              QStringLiteral("closing the tab in front repoints the active script"));
        check(doc.activeScriptId() == QLatin1String("s3"),
              QStringLiteral("at the tab that took its place, got '%1'")
                  .arg(doc.activeScriptId()));
        check(doc.activeGraph() != nullptr,
              QStringLiteral("so there is still a graph to draw"));

        // Nothing is dropped in silence: the close is on the modified flag, so
        // the project is behind the file and the existing quit prompt fires, and
        // the entry itself is still reachable.
        check(doc.isModified(),
              QStringLiteral("a close leaves the project modified, so quitting asks"));
        check(doc.canReopenScript() && doc.lastClosedName() == QLatin1String("CarScript"),
              QStringLiteral("the closed script is on the reopen stack by name"));
        check(doc.reopenClosedScript(), QStringLiteral("reopening it succeeds"));
        check(p.scripts.size() == 3, QStringLiteral("the project has it back"));
        check(p.scripts.at(1).id == QLatin1String("s2"),
              QStringLiteral("at the tab position it was closed from"));
        check(p.scripts.at(1).graph.nodes.size() == 1
                  && p.scripts.at(1).graph.nodes.first().id == QLatin1String("n_s2"),
              QStringLiteral("with its graph whole"));
        check(p.scripts.at(1).graph.extra.value(QStringLiteral("readOnly")).toBool(),
              QStringLiteral("and still marked read only, so it cannot be exported"));
        check(doc.activeScriptId() == QLatin1String("s2"),
              QStringLiteral("and in front, which is where a reopen is looking"));
        check(!doc.canReopenScript(), QStringLiteral("the stack is empty again"));

        // Undo belongs to a graph, so closing a tab in the background may not
        // take the history off the graph in front. The script being closed is
        // the one whose snapshots have to go: undo skips an entry it cannot
        // apply, but a reopen makes them applicable again and the first Ctrl+Z
        // after that would rewind past the close.
        doc.setActiveScript(QStringLiteral("s3"));
        doc.beginEdit(QStringLiteral("an edit on the script in front"));
        p.script(QStringLiteral("s3"))->graph.nodes.first().x = 42;
        doc.commitEdit();
        check(doc.canUndo(), QStringLiteral("the script in front has an undo step"));
        check(doc.closeScript(QStringLiteral("s2")),
              QStringLiteral("closing a tab in the background works"));
        check(doc.canUndo(),
              QStringLiteral("and leaves the undo history of the script in front"));
        doc.undo();
        check(p.script(QStringLiteral("s3"))
                  && qFuzzyIsNull(p.script(QStringLiteral("s3"))->graph.nodes.first().x),
              QStringLiteral("which still undoes the edit it was taken for"));
        check(doc.reopenClosedScript(),
              QStringLiteral("and the background tab comes back"));

        // A script the project is the only copy of. Closing it may not be the
        // end of the graph, whatever the window decides to ask first.
        GraphVariable authored;
        authored.id = QStringLiteral("v1");
        authored.name = QStringLiteral("m_Unsaved");
        authored.type = QStringLiteral("int");
        p.script(QStringLiteral("s1"))->graph.variables.append(authored);
        check(doc.closeScript(QStringLiteral("s1")),
              QStringLiteral("a script of the user's own closes too"));
        check(doc.reopenClosedScript()
                  && p.script(QStringLiteral("s1"))
                  && p.script(QStringLiteral("s1"))->graph.variables.size() == 1
                  && p.script(QStringLiteral("s1"))->graph.variables.first().name
                         == QLatin1String("m_Unsaved"),
              QStringLiteral("and comes back carrying the work that was in it"));
        check(p.scripts.first().id == QLatin1String("s1"),
              QStringLiteral("in the position it held, not appended to the end"));

        // An id the project has grown back in the meantime. Two entries under
        // one id is worse than a renumbered one: every lookup here takes the
        // first match, so the tab bar, the undo stack and the exporter would
        // each be free to pick a different script.
        check(doc.closeScript(QStringLiteral("s3")), QStringLiteral("closes s3"));
        add("s3", "SUDO_Impostor", false);
        check(doc.reopenClosedScript(), QStringLiteral("reopens it over the collision"));
        check(p.scripts.size() == 4, QStringLiteral("both scripts are in the project"));
        int named = 0;
        for (const ScriptEntry &s : p.scripts)
            if (s.id == QLatin1String("s3")) named++;
        check(named == 1, QStringLiteral("and exactly one of them answers to s3"));

        // The floor. Every dock and the canvas read the active graph, so the
        // project keeps one script rather than each of them growing an empty
        // case nobody would ever see except by closing every tab.
        QStringList left;
        for (const ScriptEntry &s : p.scripts) left << s.id;
        for (const QString &id : left) doc.closeScript(id);
        check(p.scripts.size() == 1,
              QStringLiteral("the last script will not close, got %1 left")
                  .arg(p.scripts.size()));
        check(doc.activeGraph() != nullptr,
              QStringLiteral("so the canvas always has something to draw"));

        // A project opened over the top of this one takes its own scripts with
        // it. Reopening across that boundary would graft a class out of the last
        // project into this one.
        doc.resetToNew();
        check(!doc.canReopenScript(),
              QStringLiteral("starting a new project empties the reopen stack"));
    }

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

    // The thing the user sat down to build: a five second timer. It used to
    // take a ref member, a New Object node whose class could not be set, a Set,
    // a Get, a Run call that no pin could reach, a callback named by a string
    // nothing checked, and a separate function node.
    out << "codegen - timers and deferred calls" << Qt::endl;
    {
        const auto timerGraph = [](const QString &name, bool repeat) {
            Graph g;
            g.className = QStringLiteral("SUDO_Timed");
            g.baseClass = QStringLiteral("ItemBase");
            GraphNode begin;
            begin.id = QStringLiteral("evt");
            begin.kind = NodeKind::Builtin;
            begin.ref = bi::Begin;
            GraphNode timer;
            timer.id = QStringLiteral("t1");
            timer.kind = NodeKind::Builtin;
            timer.ref = bi::SetTimer;
            if (!name.isEmpty()) timer.opts.insert(QStringLiteral("name"), name);
            timer.inputs.insert(QStringLiteral("seconds"), QStringLiteral("5.0"));
            timer.inputs.insert(QStringLiteral("repeat"),
                                repeat ? QStringLiteral("true") : QStringLiteral("false"));
            GraphNode pr;
            pr.id = QStringLiteral("p1");
            pr.kind = NodeKind::Builtin;
            pr.ref = bi::Print;
            pr.inputs.insert(QStringLiteral("value"), QStringLiteral("\"tick\""));
            g.nodes << begin << timer << pr;
            g.edges.append({QStringLiteral("e1"), {begin.id, QStringLiteral("exec")},
                            {timer.id, QStringLiteral("exec")}, {}});
            // The callback pin, not the exec pin: the work runs later.
            g.edges.append({QStringLiteral("e2"), {timer.id, QStringLiteral("elapsed")},
                            {pr.id, QStringLiteral("exec")}, {}});
            return g;
        };

        const Graph g = timerGraph(QStringLiteral("Reload"), false);
        const GenResult gen = generateEnforce(g, cat, builtins, p);
        check(gen.code.contains(QStringLiteral("\tref Timer m_Reload;")),
              QStringLiteral("the member is declared, and declared ref"));
        check(gen.code.contains(
                  QStringLiteral("m_Reload = new Timer(CALL_CATEGORY_SYSTEM);")),
              QStringLiteral("the timer is constructed on the system queue"));
        check(gen.code.contains(QStringLiteral(
                  "m_Reload.Run(5.0, this, \"ReloadElapsed\", null, false);")),
              QStringLiteral("the call names the callback"));
        check(gen.code.contains(QStringLiteral("\tvoid ReloadElapsed()")),
              QStringLiteral("the callback method is written"));
        check(gen.code.contains(QStringLiteral("Print(")),
              QStringLiteral("the elapsed chain lands inside it"));
        // The string and the method are one decision, so no graph can produce a
        // Run() whose callback does not exist. That was the magic string.
        const int quoted = gen.code.indexOf(QStringLiteral("\"ReloadElapsed\""));
        const int declared = gen.code.indexOf(QStringLiteral("void ReloadElapsed()"));
        check(quoted >= 0 && declared >= 0,
              QStringLiteral("the name in the call and the method declared are the same"));
        check(gen.warnings.isEmpty(),
              QStringLiteral("a complete timer generates no warnings (%1)")
                  .arg(gen.warnings.join(QStringLiteral("; "))));

        // Regenerating the same graph has to produce the same file, or a node
        // that writes a member and a method is not finished.
        check(generateEnforce(g, cat, builtins, p).code == gen.code,
              QStringLiteral("the graph regenerates its own file byte for byte"));

        // The queue is a real choice and it has to reach the file.
        Graph gameplay = g;
        gameplay.nodes[1].opts.insert(QStringLiteral("category"),
                                      QStringLiteral("gameplay"));
        check(generateEnforce(gameplay, cat, builtins, p).code.contains(
                  QStringLiteral("new Timer(CALL_CATEGORY_GAMEPLAY);")),
              QStringLiteral("the gameplay queue reaches the constructor"));

        // Repeating reaches the last argument of Run.
        check(generateEnforce(timerGraph(QStringLiteral("Reload"), true), cat, builtins, p)
                  .code.contains(QStringLiteral("\"ReloadElapsed\", null, true);")),
              QStringLiteral("a repeating timer says so in the call"));

        // An untouched node still compiles: unique member, unique method.
        const GenResult unnamed =
            generateEnforce(timerGraph(QString(), false), cat, builtins, p);
        check(unnamed.code.contains(QStringLiteral("ref Timer m_TimerT1;"))
                  && unnamed.code.contains(QStringLiteral("void TimerT1Elapsed()")),
              QStringLiteral("an unnamed timer is named after its node"));
        // The name is an identifier whatever is typed into it.
        Graph messy = timerGraph(QStringLiteral("reload delay!"), false);
        check(generateEnforce(messy, cat, builtins, p).code.contains(
                  QStringLiteral("ref Timer m_ReloadDelay;")),
              QStringLiteral("a typed name is reduced to an identifier"));

        // Two timers sharing a name would declare one member twice and one
        // method twice. The file has to say so rather than not compile.
        Graph clash = g;
        GraphNode second = clash.nodes.at(1);
        second.id = QStringLiteral("t2");
        clash.nodes << second;
        const GenResult clashed = generateEnforce(clash, cat, builtins, p);
        check(clashed.code.count(QStringLiteral("void ReloadElapsed()")) == 1,
              QStringLiteral("a duplicated name declares the method once"));
        check(!clashed.warnings.isEmpty(),
              QStringLiteral("and the duplicate is reported"));

        // A name the class already uses would declare the same method or the
        // same member twice, which is a compile error a long way from the node.
        Graph fnClash = g;
        GraphFunction fn;
        fn.id = QStringLiteral("f1");
        fn.name = QStringLiteral("ReloadElapsed");
        fn.returns = QStringLiteral("void");
        fn.hasRawBody = true;
        fnClash.functions.append(fn);
        const GenResult fnOut = generateEnforce(fnClash, cat, builtins, p);
        check(fnOut.code.count(QStringLiteral("void ReloadElapsed()")) == 1,
              QStringLiteral("a callback clashing with a function is declared once"));
        check(!fnOut.warnings.isEmpty(),
              QStringLiteral("and the clash is reported"));

        Graph varClash = g;
        GraphVariable mine;
        mine.id = QStringLiteral("v1");
        mine.name = QStringLiteral("m_Reload");
        mine.type = QStringLiteral("int");
        varClash.variables.append(mine);
        const GenResult varOut = generateEnforce(varClash, cat, builtins, p);
        check(!varOut.code.contains(QStringLiteral("ref Timer m_Reload;")),
              QStringLiteral("a member clashing with a variable is not declared twice"));
        check(!varOut.warnings.isEmpty(),
              QStringLiteral("and that clash is reported too"));

        // Stop Timer, and the mismatch that used to be silent.
        Graph stopping = g;
        GraphNode stop;
        stop.id = QStringLiteral("s1");
        stop.kind = NodeKind::Builtin;
        stop.ref = bi::StopTimer;
        stop.opts.insert(QStringLiteral("name"), QStringLiteral("Reload"));
        stopping.nodes << stop;
        stopping.edges.append({QStringLiteral("e3"), {QStringLiteral("t1"), QStringLiteral("exec")},
                               {stop.id, QStringLiteral("exec")}, {}});
        const GenResult stopped = generateEnforce(stopping, cat, builtins, p);
        check(stopped.code.contains(QStringLiteral("if (m_Reload) m_Reload.Stop();")),
              QStringLiteral("Stop Timer guards the member it stops"));
        check(stopped.warnings.isEmpty(),
              QStringLiteral("a Stop that matches a Set is not reported (%1)")
                  .arg(stopped.warnings.join(QStringLiteral("; "))));

        Graph orphan = stopping;
        orphan.nodes.last().opts.insert(QStringLiteral("name"), QStringLiteral("Nothing"));
        bool said = false;
        for (const QString &w : generateEnforce(orphan, cat, builtins, p).warnings)
            if (w.contains(QStringLiteral("stops nothing"))) said = true;
        check(said, QStringLiteral("a Stop naming no timer is reported"));

        // Call Later: milliseconds as a whole number, a function reference
        // rather than a name, and the method written for it.
        Graph later;
        later.className = QStringLiteral("SUDO_Deferred");
        later.baseClass = QStringLiteral("ItemBase");
        GraphNode lb;
        lb.id = QStringLiteral("evt");
        lb.kind = NodeKind::Builtin;
        lb.ref = bi::Begin;
        GraphNode cl;
        cl.id = QStringLiteral("c1");
        cl.kind = NodeKind::Builtin;
        cl.ref = bi::CallLater;
        cl.opts.insert(QStringLiteral("name"), QStringLiteral("RefreshHud"));
        cl.inputs.insert(QStringLiteral("ms"), QStringLiteral("250"));
        cl.inputs.insert(QStringLiteral("repeat"), QStringLiteral("false"));
        GraphNode lp;
        lp.id = QStringLiteral("p1");
        lp.kind = NodeKind::Builtin;
        lp.ref = bi::Print;
        lp.inputs.insert(QStringLiteral("value"), QStringLiteral("\"late\""));
        later.nodes << lb << cl << lp;
        later.edges.append({QStringLiteral("e1"), {lb.id, QStringLiteral("exec")},
                            {cl.id, QStringLiteral("exec")}, {}});
        later.edges.append({QStringLiteral("e2"), {cl.id, QStringLiteral("then")},
                            {lp.id, QStringLiteral("exec")}, {}});
        const GenResult def = generateEnforce(later, cat, builtins, p);
        check(def.code.contains(QStringLiteral(
                  "GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM)"
                  ".CallLater(RefreshHud, 250, false);")),
              QStringLiteral("Call Later passes the method, unquoted"));
        check(!def.code.contains(QStringLiteral("\"RefreshHud\"")),
              QStringLiteral("and never as a string, which is a different call"));
        check(def.code.contains(QStringLiteral("\tvoid RefreshHud()")),
              QStringLiteral("the deferred method is written"));
        check(!def.code.contains(QStringLiteral("ref Timer")),
              QStringLiteral("a deferred call declares no member"));

        GraphNode cancel;
        cancel.id = QStringLiteral("x1");
        cancel.kind = NodeKind::Builtin;
        cancel.ref = bi::CancelCallLater;
        cancel.opts.insert(QStringLiteral("name"), QStringLiteral("RefreshHud"));
        Graph cancelling = later;
        cancelling.nodes << cancel;
        cancelling.edges.append({QStringLiteral("e3"), {cl.id, QStringLiteral("exec")},
                                 {cancel.id, QStringLiteral("exec")}, {}});
        check(generateEnforce(cancelling, cat, builtins, p).code.contains(
                  QStringLiteral("GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM)"
                                 ".Remove(RefreshHud);")),
              QStringLiteral("Cancel Call Later removes the same reference"));

        // Nothing on the callback pin schedules an empty method, which is the
        // one way this node can be wired and still do nothing.
        Graph bare = timerGraph(QStringLiteral("Reload"), false);
        bare.edges.removeLast();
        bool empty = false;
        for (const QString &w : generateEnforce(bare, cat, builtins, p).warnings)
            if (w.contains(QStringLiteral("empty method"))) empty = true;
        check(empty, QStringLiteral("a timer with nothing to run is reported"));
    }

    // A file is more than one class. The generator answers for a class, so the
    // preamble and the line between two classes are assembleScriptFile's own
    // and it is the only thing that can put the file's ending on them. Before
    // this existed, a CRLF file holding two classes came back holding both
    // endings, and so did any CRLF file with an enum above its class. 759 and
    // 451 real files respectively.
    out << "one file out of several classes" << Qt::endl;
    {
        const QString a = QStringLiteral("class A\n{\n\tint x;\n};\n");
        const QString b = QStringLiteral("class B\n{\n\tint y;\n};\n");
        const QString enumAbove = QStringLiteral("enum EMode\n{\n\tOff,\n\tOn\n}\n");

        const QString lf = assembleScriptFile({a, b}, QString(), QStringLiteral("\n"));
        check(!lf.contains(QLatin1Char('\r')), QStringLiteral("an LF file stays LF"));
        check(lf.contains(QStringLiteral("};\n\nclass B")),
              QStringLiteral("one blank line between two classes"));

        const QString crlf =
            assembleScriptFile({nodefmt::withEol(a, QStringLiteral("\r\n")),
                                nodefmt::withEol(b, QStringLiteral("\r\n"))},
                               QString(), QStringLiteral("\r\n"));
        check(crlf.count(QLatin1Char('\n')) == crlf.count(QStringLiteral("\r\n")),
              QStringLiteral("a CRLF file holding two classes has no bare newline"));
        check(crlf == nodefmt::withEol(lf, QStringLiteral("\r\n")),
              QStringLiteral("the two spellings differ only in their endings"));

        const QString withPre =
            assembleScriptFile({nodefmt::withEol(a, QStringLiteral("\r\n"))}, enumAbove,
                               QStringLiteral("\r\n"));
        check(withPre.count(QLatin1Char('\n')) == withPre.count(QStringLiteral("\r\n")),
              QStringLiteral("a preamble above the class carries the ending too"));
        check(withPre.startsWith(QStringLiteral("enum EMode\r\n")),
              QStringLiteral("the preamble is still what the author wrote"));
        check(withPre.contains(QStringLiteral("}\r\n\r\nclass A")),
              QStringLiteral("one blank line between the preamble and the class"));

        // Whatever endings the preamble arrives with, the answer is the file's.
        check(assembleScriptFile({a}, enumAbove + QStringLiteral("\n\n\n"),
                                 QStringLiteral("\n"))
                  == assembleScriptFile({a}, enumAbove, QStringLiteral("\n")),
              QStringLiteral("trailing blank lines on a preamble do not stack up"));

        // A source that mixed them has no answer that reproduces it, so nothing
        // is invented: it comes back on bare newlines.
        check(!assembleScriptFile({a, b}, enumAbove, QString())
                   .contains(QLatin1Char('\r')),
              QStringLiteral("a file that mixed endings is left on bare newlines"));
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

    // The timing rules. Each one is a mistake the engine reports as silence:
    // a callback nothing reaches, a Stop aimed at a name nobody used, a queue
    // entry left repeating after its object is gone.
    {
        const auto rules = [&](const Graph &g) {
            QStringList ids;
            for (const Diagnostic &d : analyzeGraph(g, cat, builtins).diagnostics)
                ids << d.rule;
            return ids;
        };
        Graph g;
        g.className = QStringLiteral("SUDO_Timing");
        g.baseClass = QStringLiteral("ItemBase");
        GraphNode begin;
        begin.id = QStringLiteral("evt");
        begin.kind = NodeKind::Builtin;
        begin.ref = bi::Begin;
        GraphNode later;
        later.id = QStringLiteral("c1");
        later.kind = NodeKind::Builtin;
        later.ref = bi::CallLater;
        later.opts.insert(QStringLiteral("name"), QStringLiteral("Poll"));
        later.inputs.insert(QStringLiteral("ms"), QStringLiteral("250"));
        later.inputs.insert(QStringLiteral("repeat"), QStringLiteral("true"));
        GraphNode pr;
        pr.id = QStringLiteral("p1");
        pr.kind = NodeKind::Builtin;
        pr.ref = bi::Print;
        pr.inputs.insert(QStringLiteral("value"), QStringLiteral("\"poll\""));
        g.nodes << begin << later << pr;
        g.edges.append({QStringLiteral("e1"), {begin.id, QStringLiteral("exec")},
                        {later.id, QStringLiteral("exec")}, {}});
        g.edges.append({QStringLiteral("e2"), {later.id, QStringLiteral("then")},
                        {pr.id, QStringLiteral("exec")}, {}});

        check(rules(g).contains(QStringLiteral("DZ319")),
              QStringLiteral("a repeating deferred call with no cancel is reported"));

        // A Timer does not need the same treatment, and must not be told it
        // does: releasing the ref member runs the destructor, which takes it
        // off the queue.
        Graph timerLoop = g;
        timerLoop.nodes[1].ref = bi::SetTimer;
        timerLoop.nodes[1].inputs.insert(QStringLiteral("seconds"), QStringLiteral("5.0"));
        timerLoop.edges[1].from.pin = QStringLiteral("elapsed");
        check(!rules(timerLoop).contains(QStringLiteral("DZ319")),
              QStringLiteral("a repeating timer is not told to cancel itself"));

        // Adding the cancel clears it.
        Graph cancelled = g;
        GraphNode cancel;
        cancel.id = QStringLiteral("x1");
        cancel.kind = NodeKind::Builtin;
        cancel.ref = bi::CancelCallLater;
        cancel.opts.insert(QStringLiteral("name"), QStringLiteral("Poll"));
        cancelled.nodes << cancel;
        cancelled.edges.append({QStringLiteral("e3"), {later.id, QStringLiteral("exec")},
                                {cancel.id, QStringLiteral("exec")}, {}});
        check(!rules(cancelled).contains(QStringLiteral("DZ319")),
              QStringLiteral("cancelling it clears the finding"));
        check(!rules(cancelled).contains(QStringLiteral("DZ318")),
              QStringLiteral("a matching name is not reported as a mismatch"));

        // A cancel that names nothing, and one that names the wrong queue.
        Graph misnamed = cancelled;
        misnamed.nodes.last().opts.insert(QStringLiteral("name"), QStringLiteral("Other"));
        check(rules(misnamed).contains(QStringLiteral("DZ318")),
              QStringLiteral("a cancel naming nothing is reported"));

        Graph wrongQueue = cancelled;
        wrongQueue.nodes.last().opts.insert(QStringLiteral("category"),
                                            QStringLiteral("gameplay"));
        check(rules(wrongQueue).contains(QStringLiteral("DZ318")),
              QStringLiteral("cancelling on the wrong queue is reported"));

        // Nothing on the callback pin.
        Graph bare = g;
        bare.edges.removeLast();
        check(rules(bare).contains(QStringLiteral("DZ317")),
              QStringLiteral("an unwired callback pin is reported"));
        check(!rules(bare).contains(QStringLiteral("DZ205")),
              QStringLiteral("and not also called a branch that decides nothing"));

        // Two nodes claiming one name. The generator writes the method once, so
        // the second node's chain is dropped from the file with nothing on the
        // canvas to say which one lost. The name is the member, the string and
        // the method all at once, which is what makes it unshareable.
        check(!rules(cancelled).contains(QStringLiteral("DZ320")),
              QStringLiteral("one name used once is not reported"));
        Graph twice = cancelled;
        GraphNode second = later;
        second.id = QStringLiteral("c2");
        twice.nodes << second;
        twice.edges.append({QStringLiteral("e4"), {cancel.id, QStringLiteral("exec")},
                            {second.id, QStringLiteral("exec")}, {}});
        twice.edges.append({QStringLiteral("e5"), {second.id, QStringLiteral("then")},
                            {pr.id, QStringLiteral("exec")}, {}});
        check(rules(twice).contains(QStringLiteral("DZ320")),
              QStringLiteral("two Call Later nodes sharing a name are reported"));
        twice.nodes.last().opts.insert(QStringLiteral("name"), QStringLiteral("Other"));
        check(!rules(twice).contains(QStringLiteral("DZ320")),
              QStringLiteral("renaming one of them clears it"));

        // And the same collision against something the class already spells,
        // where the generator refuses to write the timer at all.
        Graph clash;
        clash.className = QStringLiteral("SUDO_Timing");
        clash.baseClass = QStringLiteral("ItemBase");
        GraphNode timer = later;
        timer.id = QStringLiteral("t1");
        timer.ref = bi::SetTimer;
        timer.opts.insert(QStringLiteral("name"), QStringLiteral("Reload"));
        clash.nodes << begin << timer << pr;
        clash.edges.append({QStringLiteral("e1"), {begin.id, QStringLiteral("exec")},
                            {timer.id, QStringLiteral("exec")}, {}});
        clash.edges.append({QStringLiteral("e2"), {timer.id, QStringLiteral("elapsed")},
                            {pr.id, QStringLiteral("exec")}, {}});
        check(!rules(clash).contains(QStringLiteral("DZ320")),
              QStringLiteral("a timer whose name nothing else uses is not reported"));

        Graph clashFn = clash;
        GraphFunction fn;
        fn.id = QStringLiteral("f1");
        fn.name = QStringLiteral("ReloadElapsed");
        fn.returns = QStringLiteral("void");
        clashFn.functions << fn;
        check(rules(clashFn).contains(QStringLiteral("DZ320")),
              QStringLiteral("a timer landing on a declared function is reported"));

        Graph clashVar = clash;
        GraphVariable var;
        var.name = QStringLiteral("m_Reload");
        var.type = QStringLiteral("int");
        clashVar.variables << var;
        check(rules(clashVar).contains(QStringLiteral("DZ320")),
              QStringLiteral("a timer landing on a declared variable is reported"));
    }

    // ------------------------------------------------------------------ arrays
    //
    // An array used to be a dead end: no editor, no Make Array node, and a
    // catalogue `array.Insert` whose target pin is an `array` OBJECT, which
    // canConnect refuses to join to an `array<ref X>` pin. All three are
    // checked here, plus the one thing that decides whether the generated
    // script compiles: Enforce takes a brace list after a type in a declaration
    // and nowhere else.
    out << "arrays" << Qt::endl;
    {
        const PinList list = builtins.def(bi::MakeArray).list;
        check(list.valid(), QStringLiteral("Make Array declares a pin list"));

        // An array pin still gets no box to type into, and that stays true: a
        // list of values is not something one field can hold, and a pin's
        // literal is emitted code. What changed is that there is now a node
        // whose element pins each get one.
        const PinType arrayOfStrings{PinKind::String, {}, true};
        const PinType oneString{PinKind::String, {}, false};
        check(inlineEditorFor(arrayOfStrings) == InlineEditor::None
                  && inlineEditorFor(oneString) == InlineEditor::Text,
              QStringLiteral("an array has no inline editor and its element does"));

        // The count lives in opts, so it goes out to the .sdzn and comes back.
        Graph counted;
        GraphNode mk;
        mk.id = QStringLiteral("mk");
        mk.kind = NodeKind::Builtin;
        mk.ref = bi::MakeArray;
        mk.opts.insert(QStringLiteral("count"), QStringLiteral("3"));
        mk.inputs.insert(QStringLiteral("el2"), QStringLiteral("third"));
        counted.nodes << mk;
        counted.edges.append({QStringLiteral("ex"), {QStringLiteral("other"),
                                                     QStringLiteral("ret")},
                              {QStringLiteral("mk"), QStringLiteral("el2")}, {}});
        const Graph reloaded = graphFromJson(graphToJson(counted));
        check(reloaded.nodes.size() == 1
                  && bi::listCount(reloaded.nodes.first(), list) == 3,
              QStringLiteral("the element count survives a save and a load"));

        GraphNode untouched;
        untouched.ref = bi::MakeArray;
        check(bi::listCount(untouched, list) == 2,
              QStringLiteral("a node placed from the palette already has elements"));
        GraphNode absurd = untouched;
        absurd.opts.insert(QStringLiteral("count"), QStringLiteral("100000"));
        check(bi::listCount(absurd, list) == list.max,
              QStringLiteral("a hand-edited count is clamped to %1").arg(list.max));

        const NodeDef threeUp = builtins.defForNode(mk, cat);
        int elementPins = 0;
        for (const Pin &p : threeUp.pins)
            if (p.dir == PinDir::In && p.type.kind != PinKind::Exec
                && p.id.startsWith(list.pinPrefix))
                elementPins++;
        check(elementPins == 3, QStringLiteral("three element pins are drawn, got %1")
                                    .arg(elementPins));

        // Shrinking takes the wire and the value of the pin that goes away.
        // Leaving either behind means growing the node back resurrects a value
        // the author deleted, and leaves a wire pointing at no pin at all.
        removePin(counted, QStringLiteral("mk"), QStringLiteral("el2"));
        check(counted.edges.isEmpty()
                  && !counted.nodes.first().inputs.contains(QStringLiteral("el2")),
              QStringLiteral("dropping an element takes its wire and its value"));

        // -------------------------------------------------- the two forms
        const auto generate = [&](const Graph &g) {
            Project proj;
            return generateEnforce(g, cat, builtins, proj).code;
        };
        const auto lineWith = [](const QString &code, const QString &needle) {
            for (const QString &l : code.split(QLatin1Char('\n')))
                if (l.contains(needle)) return l.trimmed();
            return QString();
        };

        GraphNode begin;
        begin.id = QStringLiteral("b");
        begin.kind = NodeKind::Builtin;
        begin.ref = bi::Begin;
        begin.opts.insert(QStringLiteral("noSuper"), QStringLiteral("1"));

        // A declaration, so the vanilla brace form is legal and is what a DayZ
        // author writes: batterycharger.c:388 is exactly this shape.
        Graph decl;
        decl.className = QStringLiteral("SUDO_Arrays");
        decl.baseClass = QStringLiteral("ItemBase");
        GraphNode make;
        make.id = QStringLiteral("mk");
        make.kind = NodeKind::Builtin;
        make.ref = bi::MakeArray;
        make.opts.insert(QStringLiteral("count"), QStringLiteral("2"));
        make.opts.insert(QStringLiteral("type"), QStringLiteral("string"));
        make.inputs.insert(QStringLiteral("el0"), QStringLiteral("shortname1"));
        make.inputs.insert(QStringLiteral("el1"), QStringLiteral("shortname2"));
        decl.nodes << begin << make;
        decl.edges.append({QStringLiteral("e1"), {QStringLiteral("b"), QStringLiteral("exec")},
                           {QStringLiteral("mk"), QStringLiteral("exec")}, {}});
        const QString declared = lineWith(generate(decl), QStringLiteral("array<string>"));
        check(declared == QStringLiteral("array<string> arr0 = {\"shortname1\", \"shortname2\"};"),
              QStringLiteral("a declaration takes the brace form, got [%1]").arg(declared));

        // The same node feeding a member that already exists. There is no
        // declaration to hang an initialiser off, so it has to allocate and
        // insert, which is what vanilla and Expansion both write mid-flow.
        Graph assign;
        assign.className = QStringLiteral("SUDO_Arrays");
        assign.baseClass = QStringLiteral("ItemBase");
        GraphVariable junk;
        junk.id = QStringLiteral("v0");
        junk.name = QStringLiteral("m_JunkTypes");
        junk.type = QStringLiteral("ref array<string>");
        assign.variables << junk;
        GraphNode bare = make;
        // Nothing says what it holds except the member it goes into.
        bare.opts.remove(QStringLiteral("type"));
        GraphNode set;
        set.id = QStringLiteral("st");
        set.kind = NodeKind::VarSet;
        set.ref = QStringLiteral("var.set.v0");
        assign.nodes << begin << bare << set;
        assign.edges.append({QStringLiteral("e1"), {QStringLiteral("b"), QStringLiteral("exec")},
                             {QStringLiteral("mk"), QStringLiteral("exec")}, {}});
        assign.edges.append({QStringLiteral("e2"), {QStringLiteral("mk"), QStringLiteral("exec")},
                             {QStringLiteral("st"), QStringLiteral("exec")}, {}});
        assign.edges.append({QStringLiteral("e3"), {QStringLiteral("mk"), QStringLiteral("arr")},
                             {QStringLiteral("st"), QStringLiteral("v")}, {}});
        const QString assigned = generate(assign);
        check(lineWith(assigned, QStringLiteral("new array"))
                  == QStringLiteral("m_JunkTypes = new array<string>();"),
              QStringLiteral("an assignment allocates, got [%1]")
                  .arg(lineWith(assigned, QStringLiteral("new array"))));
        check(assigned.contains(QStringLiteral("m_JunkTypes.Insert(\"shortname1\");"))
                  && assigned.contains(QStringLiteral("m_JunkTypes.Insert(\"shortname2\");")),
              QStringLiteral("and inserts each element behind it"));
        check(!assigned.contains(QLatin1String("= {")),
              QStringLiteral("with no brace list anywhere, which would not compile"));
        // The Inserts belong AFTER the assignment. Emitted where the Make Array
        // sits they would run against a member that is still null.
        check(assigned.indexOf(QStringLiteral("new array<string>()"))
                  < assigned.indexOf(QStringLiteral("m_JunkTypes.Insert")),
              QStringLiteral("the inserts land behind the assignment"));

        // ------------------------------------------------- type inference
        // The pin the array is wired into, in the spelling that declaration
        // used. array<ref X> and array<X> are different instantiations, so the
        // modifier has to survive.
        SearchOptions opts;
        opts.ofClass = QStringLiteral("ItemBase");
        QString takesFloats;
        for (const SearchHit &h : cat.search(QStringLiteral("TransferVariablesFloat"), opts)) {
            const MethodSig sig = cat.method(h.key);
            if (sig.valid && sig.name == QLatin1String("TransferVariablesFloat"))
                takesFloats = h.key;
        }
        check(!takesFloats.isEmpty(),
              QStringLiteral("the catalogue still declares EntityAI::TransferVariablesFloat"));
        if (!takesFloats.isEmpty()) {
            Graph intoCall;
            intoCall.className = QStringLiteral("SUDO_Arrays");
            intoCall.baseClass = QStringLiteral("ItemBase");
            GraphNode call;
            call.id = QStringLiteral("cl");
            call.kind = NodeKind::Call;
            call.ref = takesFloats;
            GraphNode untyped = make;
            untyped.opts.remove(QStringLiteral("type"));
            untyped.inputs.clear();
            intoCall.nodes << begin << untyped << call;
            intoCall.edges.append({QStringLiteral("e1"),
                                   {QStringLiteral("b"), QStringLiteral("exec")},
                                   {QStringLiteral("mk"), QStringLiteral("exec")}, {}});
            intoCall.edges.append({QStringLiteral("e2"),
                                   {QStringLiteral("mk"), QStringLiteral("exec")},
                                   {QStringLiteral("cl"), QStringLiteral("exec")}, {}});
            intoCall.edges.append({QStringLiteral("e3"),
                                   {QStringLiteral("mk"), QStringLiteral("arr")},
                                   {QStringLiteral("cl"), QStringLiteral("p0")}, {}});
            const QString code = generate(intoCall);
            check(code.contains(QStringLiteral("array<float> arr0 = {")),
                  QStringLiteral("the type comes off the pin the array is wired into"));
        }

        // Nothing wired and nothing set: what is typed on the elements is the
        // last thing left to read, and only for spellings with one meaning.
        Graph sniffed = decl;
        sniffed.nodes[1].opts.remove(QStringLiteral("type"));
        sniffed.nodes[1].inputs.insert(QStringLiteral("el0"), QStringLiteral("1"));
        sniffed.nodes[1].inputs.insert(QStringLiteral("el1"), QStringLiteral("2"));
        check(generate(sniffed).contains(QStringLiteral("array<int> arr0 = {1, 2};")),
              QStringLiteral("whole numbers typed on the elements read as an int array"));

        // What the author set beats everything under it.
        Graph forced = assign;
        for (GraphNode &n : forced.nodes)
            if (n.ref == bi::MakeArray) n.opts.insert(QStringLiteral("type"),
                                                      QStringLiteral("ref EntityAI"));
        check(generate(forced).contains(QStringLiteral("new array<ref EntityAI>()")),
              QStringLiteral("an element type set in Details wins over the member's own"));

        // `array<ref X>` and `array<X>` are different instantiations, so a
        // modifier read off a declaration has to survive into the generated
        // type. Dropping it is a compile error at whatever the array feeds.
        Graph owning = assign;
        owning.variables[0].type = QStringLiteral("ref array<ref EntityAI>");
        check(generate(owning).contains(QStringLiteral("new array<ref EntityAI>()")),
              QStringLiteral("the ref inside a declared array type is kept"));

        // ------------------------------------------------ the rest of them
        Graph ops;
        ops.className = QStringLiteral("SUDO_Arrays");
        ops.baseClass = QStringLiteral("ItemBase");
        GraphVariable bag;
        bag.id = QStringLiteral("v1");
        bag.name = QStringLiteral("m_Bag");
        bag.type = QStringLiteral("ref array<string>");
        ops.variables << bag;
        GraphNode get;
        get.id = QStringLiteral("gv");
        get.kind = NodeKind::VarGet;
        get.ref = QStringLiteral("var.get.v1");
        GraphNode insert;
        insert.id = QStringLiteral("ins");
        insert.kind = NodeKind::Builtin;
        insert.ref = bi::ArrayInsert;
        insert.inputs.insert(QStringLiteral("v"), QStringLiteral("\"rag\""));
        GraphNode loop;
        loop.id = QStringLiteral("lp");
        loop.kind = NodeKind::Builtin;
        loop.ref = bi::ArrayForIndex;
        GraphNode shout;
        shout.id = QStringLiteral("pr");
        shout.kind = NodeKind::Builtin;
        shout.ref = bi::Print;
        ops.nodes << begin << get << insert << loop << shout;
        ops.edges.append({QStringLiteral("e1"), {QStringLiteral("b"), QStringLiteral("exec")},
                          {QStringLiteral("ins"), QStringLiteral("exec")}, {}});
        ops.edges.append({QStringLiteral("e2"), {QStringLiteral("gv"), QStringLiteral("ret")},
                          {QStringLiteral("ins"), QStringLiteral("arr")}, {}});
        ops.edges.append({QStringLiteral("e3"), {QStringLiteral("ins"), QStringLiteral("exec")},
                          {QStringLiteral("lp"), QStringLiteral("exec")}, {}});
        ops.edges.append({QStringLiteral("e4"), {QStringLiteral("gv"), QStringLiteral("ret")},
                          {QStringLiteral("lp"), QStringLiteral("arr")}, {}});
        ops.edges.append({QStringLiteral("e5"), {QStringLiteral("lp"), QStringLiteral("body")},
                          {QStringLiteral("pr"), QStringLiteral("exec")}, {}});
        ops.edges.append({QStringLiteral("e6"), {QStringLiteral("lp"), QStringLiteral("item")},
                          {QStringLiteral("pr"), QStringLiteral("value")}, {}});
        const QString opsCode = generate(ops);
        check(opsCode.contains(QStringLiteral("m_Bag.Insert(\"rag\");")),
              QStringLiteral("Array Insert runs against the array pin, not a target pin"));
        check(opsCode.contains(QStringLiteral("for (int i0 = 0; i0 < m_Bag.Count(); i0++)")),
              QStringLiteral("For Each Index counts up to Count()"));
        check(opsCode.contains(QStringLiteral("Print(m_Bag.Get(i0));")),
              QStringLiteral("and its item pin reads the element back"));

        // ------------------------------------------------- what is reported
        const auto rulesOf = [&](const Graph &g) {
            QStringList ids;
            for (const Diagnostic &d : analyzeGraph(g, cat, builtins).diagnostics)
                ids << d.rule;
            return ids;
        };
        check(!rulesOf(ops).contains(QStringLiteral("DZ321")),
              QStringLiteral("an array node with its array wired is not reported"));
        Graph loose = ops;
        for (int i = loose.edges.size() - 1; i >= 0; --i)
            if (loose.edges.at(i).to.pin == QLatin1String("arr")) loose.edges.removeAt(i);
        check(rulesOf(loose).contains(QStringLiteral("DZ321")),
              QStringLiteral("one with nothing on it generates a call on null and is reported"));

        // Insert inside a loop over the same array is the crash the rule for
        // catalogue calls already covered. The builtin has to be seen too.
        Graph inLoop;
        inLoop.className = QStringLiteral("SUDO_Arrays");
        inLoop.baseClass = QStringLiteral("ItemBase");
        inLoop.variables << bag;
        inLoop.nodes << begin << get << loop << insert;
        inLoop.edges.append({QStringLiteral("l1"), {QStringLiteral("b"), QStringLiteral("exec")},
                             {QStringLiteral("lp"), QStringLiteral("exec")}, {}});
        inLoop.edges.append({QStringLiteral("l2"), {QStringLiteral("gv"), QStringLiteral("ret")},
                             {QStringLiteral("lp"), QStringLiteral("arr")}, {}});
        inLoop.edges.append({QStringLiteral("l3"), {QStringLiteral("lp"), QStringLiteral("body")},
                             {QStringLiteral("ins"), QStringLiteral("exec")}, {}});
        inLoop.edges.append({QStringLiteral("l4"), {QStringLiteral("gv"), QStringLiteral("ret")},
                             {QStringLiteral("ins"), QStringLiteral("arr")}, {}});
        check(rulesOf(inLoop).contains(QStringLiteral("DZ307")),
              QStringLiteral("Array Insert inside a loop over the same array is reported"));

        Graph dropped;
        dropped.className = QStringLiteral("SUDO_Arrays");
        dropped.baseClass = QStringLiteral("ItemBase");
        dropped.nodes << begin << make;
        dropped.edges.append({QStringLiteral("e1"), {QStringLiteral("b"), QStringLiteral("exec")},
                              {QStringLiteral("mk"), QStringLiteral("exec")}, {}});
        check(rulesOf(dropped).contains(QStringLiteral("DZ322")),
              QStringLiteral("an array nobody reads is reported"));

        // An element pin left empty falls back to its type's default, and the
        // default for a class is null: a hole in a `ref` array that the first
        // loop over it dereferences. Nothing new reports that, and nothing
        // should, because DZ106 already does and calls it an error. What was
        // missing was a check that it stays that way, since an element pin is
        // the first input in this build whose type comes from an option on the
        // node rather than from its definition, and a pin that resolved to
        // something editable would drop out of DZ106 without failing anywhere.
        GraphVariable boxes;
        boxes.id = QStringLiteral("v9");
        boxes.name = QStringLiteral("m_Boxes");
        boxes.type = QStringLiteral("ref array<ref ItemBase>");
        GraphNode objMake = make;
        objMake.opts.insert(QStringLiteral("type"), QStringLiteral("ref ItemBase"));
        objMake.inputs.clear();
        GraphNode setBoxes;
        setBoxes.id = QStringLiteral("sb");
        setBoxes.kind = NodeKind::VarSet;
        setBoxes.ref = QStringLiteral("var.set.v9");
        Graph nulls;
        nulls.className = QStringLiteral("SUDO_Arrays");
        nulls.baseClass = QStringLiteral("ItemBase");
        nulls.variables << boxes;
        nulls.nodes << begin << objMake << setBoxes;
        nulls.edges.append({QStringLiteral("e1"), {QStringLiteral("b"), QStringLiteral("exec")},
                            {QStringLiteral("mk"), QStringLiteral("exec")}, {}});
        nulls.edges.append({QStringLiteral("e2"), {QStringLiteral("mk"), QStringLiteral("exec")},
                            {QStringLiteral("sb"), QStringLiteral("exec")}, {}});
        nulls.edges.append({QStringLiteral("e3"), {QStringLiteral("mk"), QStringLiteral("arr")},
                            {QStringLiteral("sb"), QStringLiteral("v")}, {}});
        check(generate(nulls).contains(QStringLiteral("m_Boxes.Insert(null);")),
              QStringLiteral("an empty element pin on an object array writes a null"));
        check(rulesOf(nulls).contains(QStringLiteral("DZ106")),
              QStringLiteral("and it is an error before it ever runs"));

        // Not set on the node either, so the element pins are Any. That is the
        // state a node is in between being placed and being told what it holds,
        // and Any has no field to type into, so it has to report as well.
        Graph inferred = nulls;
        for (GraphNode &n : inferred.nodes)
            if (n.ref == bi::MakeArray) n.opts.remove(QStringLiteral("type"));
        check(rulesOf(inferred).contains(QStringLiteral("DZ106")),
              QStringLiteral("an element pin with no type yet is reported too"));

        // Taking the pins off with the minus is the fix, and it has to clear it.
        Graph emptied = nulls;
        for (GraphNode &n : emptied.nodes)
            if (n.ref == bi::MakeArray) n.opts.insert(QStringLiteral("count"),
                                                      QStringLiteral("0"));
        check(!rulesOf(emptied).contains(QStringLiteral("DZ106")),
              QStringLiteral("a Make Array with no elements has nothing to report"));

        // "" and 0 are values a mod may well mean, and both have a field on the
        // node to type them into, so an array of strings is not this rule's.
        Graph strings = nulls;
        for (GraphNode &n : strings.nodes)
            if (n.ref == bi::MakeArray) n.opts.insert(QStringLiteral("type"),
                                                      QStringLiteral("string"));
        check(!rulesOf(strings).contains(QStringLiteral("DZ106")),
              QStringLiteral("an empty element on a string array is not reported"));

        // And a filled one is finished.
        Graph filled = nulls;
        filled.nodes << get;
        filled.edges.append({QStringLiteral("e4"), {QStringLiteral("gv"), QStringLiteral("ret")},
                             {QStringLiteral("mk"), QStringLiteral("el0")}, {}});
        filled.edges.append({QStringLiteral("e5"), {QStringLiteral("gv"), QStringLiteral("ret")},
                             {QStringLiteral("mk"), QStringLiteral("el1")}, {}});
        check(!rulesOf(filled).contains(QStringLiteral("DZ106")),
              QStringLiteral("every element wired means nothing to report"));

        // ------------------------------------------- what could not connect
        // The shape from the user's own screenshot: an array<ref X> coming out
        // of a catalogue call. The array group has to take it.
        Pin containers;
        containers.id = QStringLiteral("ret");
        containers.dir = PinDir::Out;
        containers.type = pinTypeOf(QStringLiteral("array<ref ExpansionLootContainer>"),
                                    [&cat](const QString &n) { return cat.isEnum(n); });
        check(containers.type.isArray && containers.type.kind == PinKind::Object,
              QStringLiteral("array<ref X> parses as an array of objects"));

        const NodeDef countDef = builtins.def(bi::ArrayCount);
        const Pin *arrPin = countDef.pin(QStringLiteral("arr"), PinDir::In);
        check(arrPin && canConnect(containers, *arrPin),
              QStringLiteral("Array Count takes it"));

        // And the catalogue's own array.Insert does not, which is the whole
        // reason the group exists rather than being a set of curated rows.
        QString catalogueInsert;
        SearchOptions arrayOpts;
        arrayOpts.ofClass = QStringLiteral("array");
        for (const SearchHit &h : cat.search(QStringLiteral("Insert"), arrayOpts)) {
            const MethodSig sig = cat.method(h.key);
            if (sig.valid && sig.owner == QLatin1String("array")
                && sig.name == QLatin1String("Insert"))
                catalogueInsert = h.key;
        }
        if (!catalogueInsert.isEmpty()) {
            const NodeDef fromCat = cat.defFor(catalogueInsert);
            const Pin *target = fromCat.pin(QStringLiteral("target"), PinDir::In);
            check(target && !canConnect(containers, *target),
                  QStringLiteral("the catalogue's own array.Insert still cannot"));
        }
    }

    // ------------------------------------------------ what a node row prints
    //
    // A pin row on the canvas now names the type it carries, and it takes that
    // name from the signature rather than from the pin, because a pin kept only
    // a kind and a class and gives `array<ItemBase>` back for something the
    // file spells `ref array<ref ItemBase>`. NodeItem is compiled into the
    // application alone and no test target can reach it, so what is pinned here
    // is the catalogue side it reads: if any of this stops holding, the word on
    // the row goes quietly wrong rather than failing anywhere.
    out << Qt::endl << "what a pin row can say about its type" << Qt::endl;
    {
        // Walked rather than searched. Ranking decides what a human sees and
        // has no business deciding what a check looks at: a name six classes
        // declare can rank the wrong one first and turn a real regression into
        // a test that quietly stops examining anything.
        const int methodCount = cat.totals().value(QStringLiteral("methods"));
        const auto sigOf = [&cat, methodCount](const QString &owner, const QString &name) {
            for (int i = 0; i < methodCount; ++i) {
                const QString key = QStringLiteral("m%1").arg(i);
                const MethodSig sig = cat.method(key);
                if (sig.valid && sig.owner == owner && sig.name == name)
                    return QPair<QString, MethodSig>(key, sig);
            }
            return QPair<QString, MethodSig>(QString(), MethodSig());
        };

        // A generic return arrives whole. This is the shape from the user's own
        // screenshot: the node used to print `return` for it.
        const auto cargo = sigOf(QStringLiteral("EntityAI"),
                                 QStringLiteral("GetAttachmentsWithCargo"));
        check(cargo.second.valid
                  && cargo.second.ret == QLatin1String("array<EntityAI>"),
              QStringLiteral("a generic return keeps its spelling, got '%1'")
                  .arg(cargo.second.ret));

        // And a typedef stays a typedef. Reconstructing it from the pin would
        // print `array<string>`, which is the same type and not the word the
        // reader will search P:\\scripts for.
        const auto hidden = sigOf(QStringLiteral("EntityAI"),
                                  QStringLiteral("GetHiddenSelections"));
        check(hidden.second.valid
                  && hidden.second.ret == QLatin1String("TStringArray"),
              QStringLiteral("a typedef return is not expanded, got '%1'")
                  .arg(hidden.second.ret));

        // The row finds its type by the digits in the pin id, so the numbering
        // has to be an index into the parameters and not a count of the pins in
        // front of it. Catalog::paramPins numbers an out parameter's output pin
        // `o<N>` after the same N, which is what makes one lookup serve both.
        if (!cargo.first.isEmpty()) {
            const auto colour = sigOf(QStringLiteral("EntityAI"), QStringLiteral("GetColor"));
            const NodeDef def = cat.defFor(colour.first);
            bool numbered = colour.second.valid && !colour.second.params.isEmpty();
            for (const Pin &p : def.pins) {
                const QChar family = p.id.isEmpty() ? QChar() : p.id.at(0);
                if (family != QLatin1Char('p') && family != QLatin1Char('o')) continue;
                bool ok = false;
                const int index = p.id.mid(1).toInt(&ok);
                if (!ok || index < 0 || index >= colour.second.params.size()) numbered = false;
            }
            check(numbered,
                  QStringLiteral("every p<N> and o<N> pin indexes a real parameter"));
        }

        // `return` is the one label the row drops in favour of the type, and it
        // has to be exactly that word on a call that returns something.
        const auto health = sigOf(QStringLiteral("Object"), QStringLiteral("GetHealth"));
        if (!health.first.isEmpty()) {
            const NodeDef def = cat.defFor(health.first);
            const Pin *ret = def.pin(QStringLiteral("ret"), PinDir::Out);
            check(ret && ret->label == QLatin1String("return"),
                  QStringLiteral("a returning call labels its output pin `return`"));
        }

        // The receiver is the one pin the row deliberately says nothing about,
        // because its class IS the subtitle. That is only true while the two
        // come from the same place.
        const auto setHealth = sigOf(QStringLiteral("Object"), QStringLiteral("SetHealth"));
        if (!setHealth.first.isEmpty()) {
            const NodeDef def = cat.defFor(setHealth.first);
            const Pin *target = def.pin(QStringLiteral("target"), PinDir::In);
            check(target && target->type.cls == def.subtitle
                      && !def.subtitle.isEmpty(),
                  QStringLiteral("a target pin's class is the node's subtitle"));
        }

        // The sentence on the exec row comes from here, and only from here: a
        // key with no comment behind it has to answer empty rather than
        // answering something derived, or three nodes in four would carry a
        // line the declaration never said.
        int documented = 0;
        int silent = 0;
        for (int i = 0; i < 4000; ++i) {
            const QString key = QStringLiteral("m%1").arg(i);
            if (!cat.method(key).valid) continue;
            if (cat.doc(key).isEmpty()) silent++;
            else documented++;
        }
        check(documented > 200 && silent > documented,
              QStringLiteral("doc() answers for some and stays quiet for most (%1 of %2)")
                  .arg(documented).arg(documented + silent));
        check(cat.doc(QStringLiteral("en0")).isEmpty()
                  && cat.doc(QStringLiteral("co0")).isEmpty()
                  && cat.doc(bi::MakeArray).isEmpty(),
              QStringLiteral("enums, constants and builtins carry no vanilla comment"));
    }

    out << Qt::endl << (failures == 0 ? "ALL CORE TESTS PASSED"
                                      : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
