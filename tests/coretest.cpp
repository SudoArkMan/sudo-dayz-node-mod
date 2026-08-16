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
    }

    out << Qt::endl << (failures == 0 ? "ALL CORE TESTS PASSED"
                                      : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
