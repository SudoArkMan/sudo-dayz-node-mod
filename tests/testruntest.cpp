// Running the mod: what can be checked without running it.
//
// The line this suite will not cross is starting a game or packing a PBO. Both
// take minutes, both write outside the test's own folder, and neither is what
// goes wrong. What goes wrong is a path that resolved to the wrong place and a
// command line that was one argument short, so those are what is asserted:
//
//   - the prerequisite list read against this real machine, printed in full
//   - a junction made under a temporary name, verified, then removed, with the
//     folder it pointed at proved intact afterwards
//   - the project.cfg rewrite, asserted line by line so that only Mods moved
//   - the build, server, client and offline command lines, printed rather than
//     executed, with the pair proved unchanged by the offline mode existing
//   - a mod with no mission at all, which offline has to refuse rather than
//     start into a session that looks like the mod failed to load
//   - the load order, on two mods built here whose config.cpp really does name
//     one another, added in the wrong order on purpose
//   - a mod picked for the run and then uninstalled, which every launch has to
//     refuse rather than hand the engine a path to nothing
//   - the picks saved into a .sdzn and read back out of it
//
// Everything happens inside a QTemporaryDir except the one junction that goes
// on the work drive, which uses a name nothing else will have.
#include "document.h"
#include "moddeps.h"
#include "modtemplate.h"
#include "panels/testpanel.h"
#include "project.h"
#include "testrun.h"
#include "theme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) failures++;
}

static void note(const QString &text)
{
    QTextStream out(stdout);
    out << "       " << text << Qt::endl;
}

static void heading(const QString &text)
{
    QTextStream out(stdout);
    out << Qt::endl << text << Qt::endl;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

static QString stateWord(PrereqState s)
{
    if (s == PrereqState::Ok) return QStringLiteral("ok");
    if (s == PrereqState::Warning) return QStringLiteral("check");
    return QStringLiteral("MISSING");
}

static PrereqCheck findCheck(const QVector<PrereqCheck> &checks, const QString &id)
{
    for (const PrereqCheck &c : checks)
        if (c.id == id) return c;
    return {};
}

// Where a mod sits in the chain, matched with the punctuation out so that
// "@Community-Online-Tools" off disk and "@CommunityOnlineTools" guessed from
// the name are the same answer. -1 when it is not in there.
static int chainIndex(const QVector<ModRef> &chain, const QString &needle)
{
    for (int i = 0; i < chain.size(); ++i) {
        QString packed = chain.at(i).name;
        packed.remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
        if (packed.contains(needle, Qt::CaseInsensitive)) return i;
    }
    return -1;
}

// Where a mod sits in the chain, found by an addon it ships rather than by its
// folder name. The folder name is not an identity: Community Framework is
// installed as @CF on one machine and @CommunityFramework on the next, and the
// addon under CfgPatches is the same on both. -1 when it is not in there.
static int chainIndexByAddon(const QVector<ModRef> &chain, const QString &addon)
{
    for (int i = 0; i < chain.size(); ++i)
        for (const QString &ships : chain.at(i).addons)
            if (ships.compare(addon, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

static QStringList chainNames(const QVector<ModRef> &chain)
{
    QStringList names;
    for (const ModRef &m : chain) names << m.name;
    return names;
}

static bool anyArgumentStartsWith(const RunCommand &cmd, const QString &prefix)
{
    for (const QString &a : cmd.arguments)
        if (a.startsWith(prefix)) return true;
    return false;
}

// A mod folder built here, real enough to be read: a Scripts folder with a
// config.cpp naming one addon and the addons it requires. That is the only
// thing on disk the load order can be read from, so it is the thing the order
// gets tested against.
static QString buildMod(const QString &parent, const QString &folderName,
                        const QString &addon, const QStringList &needs)
{
    const QString folder = QDir(parent).filePath(folderName);
    if (!QDir().mkpath(folder + QStringLiteral("/Scripts"))) return QString();
    QFile file(folder + QStringLiteral("/Scripts/config.cpp"));
    if (!file.open(QIODevice::WriteOnly)) return QString();
    QStringList quoted;
    for (const QString &need : needs) quoted << QStringLiteral("\"%1\"").arg(need);
    QTextStream(&file) << QStringLiteral(
                              "class CfgPatches\n"
                              "{\n"
                              "\tclass %1\n"
                              "\t{\n"
                              "\t\tunits[] = {};\n"
                              "\t\tweapons[] = {};\n"
                              "\t\trequiredAddons[] = {%2};\n"
                              "\t};\n"
                              "};\n")
                              .arg(addon, quoted.join(QStringLiteral(",")));
    file.close();
    return QDir::cleanPath(folder);
}

static ExtraMod pick(const QString &name, const QString &folder,
                     const QString &label, bool serverOnly = false)
{
    ExtraMod mod;
    mod.name = name;
    mod.folder = folder;
    mod.label = label;
    mod.serverOnly = serverOnly;
    return mod;
}

// Lines that differ between two versions of a file, by index. Line endings are
// part of the comparison: a rewrite that turns CRLF into LF is a rewrite of
// every line, and this has to catch that.
static QList<int> changedLines(const QByteArray &before, const QByteArray &after)
{
    const QByteArrayList a = before.split('\n');
    const QByteArrayList b = after.split('\n');
    QList<int> out;
    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const QByteArray left = i < a.size() ? a.at(i) : QByteArray("<absent>");
        const QByteArray right = i < b.size() ? b.at(i) : QByteArray("<absent>");
        if (left != right) out << i;
    }
    return out;
}

int main(int argc, char **argv)
{
    // Offscreen unless the caller has already chosen. The suite builds a
    // QApplication only so --shot can grab the real dock; nothing else here
    // touches a widget, and forcing the platform keeps it runnable with no
    // display.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SUDO"));
    // Deliberately not the app's own name: TestRun remembers the DayZ Tools
    // folder in QSettings, and a test has no business reading or writing the
    // key the user's real install sits in.
    QCoreApplication::setApplicationName(QStringLiteral("SUDO DayZ Node Mod tests"));

    QTextStream out(stdout);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        out << "cannot make a temporary folder" << Qt::endl;
        return 1;
    }

    // ------------------------------------------------------- a real mod folder

    heading(QStringLiteral("Scaffolding a mod to test against"));
    ModTemplateOptions options;
    options.prefix = QStringLiteral("SUDO_RunTest");
    options.displayName = QStringLiteral("Run test");
    options.author = QStringLiteral("tests");
    // Two maps, because offline runs one mission and the interesting case is
    // the one where the mod ships more than one and somebody has to pick.
    options.includeMissions = true;
    options.maps = { QStringLiteral("ChernarusPlus"), QStringLiteral("Enoch") };
    // The scaffolder junctions P:\<prefix> by itself now. This suite is about
    // what the test dock does, so it opts out and links the work drive itself
    // under a name of its own: otherwise every run leaves a junction on the
    // user's work drive pointing at a temporary folder that is already gone,
    // and the checklist row for an unlinked drive can never be exercised.
    options.linkWorkDrive = false;
    const ModTemplateResult made = scaffoldMod(tmp.path(), options);
    check(made.ok, QStringLiteral("mod scaffolded (%1)")
                       .arg(made.ok ? made.modRoot : made.error));
    if (!made.ok) {
        out << Qt::endl << "1 FAILURES" << Qt::endl;
        return 1;
    }

    // CF and COT come from the presets. Dabs is built by hand because there is
    // no preset for it, and it is the one worth having here: the template's own
    // Mods line already names it, and its folder really is on this machine
    // under a spelling with a space in it.
    ModDependency dabs;
    dabs.id = QStringLiteral("DF_Scripts");
    dabs.displayName = QStringLiteral("Dabs Framework");
    dabs.shortName = QStringLiteral("DF");

    Project project;
    project.name = options.prefix;
    project.modRoot = made.modRoot;
    project.modPrefix = options.prefix;
    // COT is declared before the framework it is built on, deliberately. COT's
    // own requiredAddons name JM_CF_Scripts, so a chain that keeps the declared
    // order would load COT before the thing it needs, and the engine's answer to
    // that is a mod that silently does nothing.
    project.dependencies = { dabs,
                             knownDependency(QStringLiteral("JM_COT_Scripts")),
                             knownDependency(QStringLiteral("JM_CF_Scripts")) };
    for (const ModDependency &d : project.dependencies)
        check(d.isValid(), QStringLiteral("dependency %1 is usable")
                               .arg(d.id.isEmpty() ? QStringLiteral("(empty)") : d.id));

    TestRun run;
    run.refresh(project);
    const TestRunPaths &paths = run.paths();

    // ----------------------------------------------------- prerequisites, real

    heading(QStringLiteral("Prerequisites on this machine"));
    const QVector<PrereqCheck> checks = run.check();
    for (const PrereqCheck &c : checks)
        out << QStringLiteral("  %1 %2: %3")
                   .arg(stateWord(c.state), -8)
                   .arg(c.label, -16)
                   .arg(c.detail)
            << Qt::endl;

    check(!checks.isEmpty(), QStringLiteral("the checklist is not empty"));
    check(findCheck(checks, QStringLiteral("modroot")).state == PrereqState::Ok,
          QStringLiteral("the scaffolded mod folder is found"));
    check(findCheck(checks, QStringLiteral("projectcfg")).state == PrereqState::Ok,
          QStringLiteral("the template's project.cfg is found"));
    check(findCheck(checks, QStringLiteral("servercfg")).state == PrereqState::Ok,
          QStringLiteral("the template's server.cfg is found"));
    // Nothing has been linked or built yet, so these two have to be reported
    // as not satisfied. A checklist that says everything is fine before any
    // work has been done is worse than no checklist.
    check(findCheck(checks, QStringLiteral("junction")).state != PrereqState::Ok,
          QStringLiteral("the work drive link is reported as not made yet"));
    check(findCheck(checks, QStringLiteral("pbo")).state != PrereqState::Ok,
          QStringLiteral("the PBO is reported as not built yet"));
    for (const PrereqCheck &c : checks)
        if (c.state != PrereqState::Ok)
            check(!c.fix.isEmpty(),
                  QStringLiteral("%1 says what to do about it").arg(c.label));

    note(QStringLiteral("work drive: %1").arg(QDir::toNativeSeparators(paths.workDrive)));
    note(QStringLiteral("DayZ Tools: %1 (%2)")
             .arg(paths.dayzTools.isEmpty() ? QStringLiteral("not found")
                                            : QDir::toNativeSeparators(paths.dayzTools),
                  paths.toolsFrom));
    note(QStringLiteral("DayZ game:  %1 (%2)")
             .arg(paths.gamePath.isEmpty() ? QStringLiteral("not found")
                                           : QDir::toNativeSeparators(paths.gamePath),
                  paths.gameFrom));
    note(QStringLiteral("DayZServer: %1")
             .arg(paths.serverPath.isEmpty() ? QStringLiteral("not found")
                                             : QDir::toNativeSeparators(paths.serverPath)));

    // The PBO source has to be the folder holding config.cpp, or the engine
    // reads no CfgPatches out of what gets packed.
    check(!paths.sourceDir.isEmpty()
              && QFileInfo::exists(paths.sourceDir + QStringLiteral("/config.cpp")),
          QStringLiteral("the pack source is the folder holding config.cpp"));
    check(paths.pboPrefix == options.prefix + QStringLiteral("\\Scripts"),
          QStringLiteral("the PBO prefix matches the paths in config.cpp (%1)")
              .arg(paths.pboPrefix));

    // ------------------------------------------------------------- the chain

    heading(QStringLiteral("Mod chain"));
    for (const ModRef &m : paths.modChain)
        out << QStringLiteral("  %1  [%2]  %3")
                   .arg(m.name, -28)
                   .arg(m.from,
                        m.path.isEmpty() ? QStringLiteral("(not installed here)")
                                         : QDir::toNativeSeparators(m.path))
            << Qt::endl;
    for (const QString &line : paths.chainNotes) note(line);

    check(paths.modChain.size() == project.dependencies.size() + 1,
          QStringLiteral("every dependency, plus the mod itself"));
    check(!paths.modChain.isEmpty()
              && paths.modChain.last().name == QStringLiteral("@") + options.prefix,
          QStringLiteral("the mod loads last, after what it is written against"));

    // The one that costs an evening when it is wrong. COT was declared first and
    // has to come out second, because its requiredAddons name CF's addon. Found
    // by addon, because on a machine where CF is installed the entry is called
    // whatever that folder is called, which is @CF here and @CommunityFramework
    // on the next machine.
    const int cfAt = chainIndexByAddon(paths.modChain, QStringLiteral("JM_CF_Scripts"));
    const int cotAt = chainIndexByAddon(paths.modChain, QStringLiteral("JM_COT_Scripts"));
    check(cfAt >= 0 && cotAt >= 0 && cfAt < cotAt,
          QStringLiteral("Community Framework loads before the tools built on it, "
                         "whatever order they were declared in (%1 then %2)")
              .arg(cfAt).arg(cotAt));
    for (const ModRef &m : paths.modChain)
        check(!m.factsFrom.isEmpty(),
              QStringLiteral("%1 says where its addon names came from (%2)")
                  .arg(m.name, m.factsFrom));

    // The template ships Mods=@Dabs Framework. Whether that entry keeps its
    // space or not, it must not end up in the chain twice under two spellings.
    QStringList packed;
    for (const ModRef &m : paths.modChain) {
        QString flat = m.name;
        packed << flat.remove(QLatin1Char(' ')).toLower();
    }
    QStringList unique = packed;
    unique.removeDuplicates();
    check(unique.size() == packed.size(),
          QStringLiteral("no mod appears twice under two spellings"));

    // ------------------------------------------------ finding the folder at all
    //
    // A -mod= entry the engine cannot resolve is not an error. It is a mod that
    // silently did not load, and the session then looks exactly like the mod
    // under test being broken. So the three ways a folder is found are checked
    // against folders built here, spelt the three ways the Workshop spells them.

    heading(QStringLiteral("Finding a dependency's folder"));
    const QString library = QDir(tmp.path()).filePath(QStringLiteral("installed"));
    QDir().mkpath(library);
    // The name asked for, exactly.
    QDir().mkpath(QDir(library).filePath(QStringLiteral("@SUDO_Exact")));
    // The same mod with the punctuation the Workshop page happened to use.
    QDir().mkpath(QDir(library).filePath(QStringLiteral("@SUDO-Punctuated-Name")));
    // A folder whose name says nothing, with a mod.cpp that says everything.
    // This is Community Framework's real shape: it ships as @CF.
    const QString hidden = QDir(library).filePath(QStringLiteral("@XX"));
    QDir().mkpath(hidden);
    {
        QFile modCpp(QDir(hidden).filePath(QStringLiteral("mod.cpp")));
        if (modCpp.open(QIODevice::WriteOnly))
            QTextStream(&modCpp) << QStringLiteral("name = \"SUDO Hidden Name\";\n");
    }

    const auto folderFor = [&library](const QString &display, const QString &id) {
        ModDependency dep;
        dep.id = id;
        dep.displayName = display;
        // gamePath is the second place searched, and the library is built to
        // stand in for its !Workshop folder's contents.
        return modFolderFor(dep, QStringLiteral("P:/no-such-drive"), library);
    };

    const ModRef exact = folderFor(QStringLiteral("SUDO_Exact"),
                                   QStringLiteral("SUDO_Exact_Scripts"));
    note(exact.from);
    check(exact.name == QStringLiteral("@SUDO_Exact") && !exact.guessed,
          QStringLiteral("the exact spelling is found (%1)").arg(exact.name));

    const ModRef punctuated = folderFor(QStringLiteral("SUDO Punctuated Name"),
                                        QStringLiteral("SUDO_Punctuated_Scripts"));
    note(punctuated.from);
    check(punctuated.name == QStringLiteral("@SUDO-Punctuated-Name")
              && !punctuated.guessed,
          QStringLiteral("so is the same name with hyphens in it, and the chain "
                         "carries the folder's own spelling (%1)")
              .arg(punctuated.name));

    const ModRef byModCpp = folderFor(QStringLiteral("SUDO Hidden Name"),
                                      QStringLiteral("SUDO_Hidden_Scripts"));
    note(byModCpp.from);
    check(byModCpp.name == QStringLiteral("@XX") && !byModCpp.guessed,
          QStringLiteral("and a folder whose name shares nothing with the mod is "
                         "found through its own mod.cpp (%1)").arg(byModCpp.name));
    check(QFileInfo(byModCpp.path).isDir(),
          QStringLiteral("the path it comes back with is a folder that is there"));

    const ModRef absent = folderFor(QStringLiteral("SUDO Not Installed"),
                                    QStringLiteral("SUDO_Absent_Scripts"));
    check(absent.guessed && absent.path.isEmpty(),
          QStringLiteral("a mod nothing here has is still a guess, and says so "
                         "(%1)").arg(absent.from));

    // ------------------------------------------------------ project.cfg rewrite

    heading(QStringLiteral("Writing the mod chain into project.cfg"));
    const QByteArray before = readFile(paths.projectCfg);
    check(before.contains("Mods="), QStringLiteral("the template file has a Mods line"));
    check(before.contains("ServerMods="),
          QStringLiteral("and a ServerMods line to leave alone"));

    const RunStep chainStep = run.writeModChain();
    check(chainStep.ok, QStringLiteral("the rewrite reports success (%1)")
                            .arg(chainStep.detail));
    note(chainStep.output);

    const QByteArray after = readFile(paths.projectCfg);
    const QList<int> moved = changedLines(before, after);
    check(moved.size() == 1,
          QStringLiteral("exactly one line changed (%1 changed)").arg(moved.size()));
    if (moved.size() == 1) {
        const QByteArray line = after.split('\n').at(moved.first());
        check(line.startsWith("Mods="),
              QStringLiteral("and it is the Mods line (%1)")
                  .arg(QString::fromUtf8(line.trimmed())));
    }
    check(before.split('\n').size() == after.split('\n').size(),
          QStringLiteral("the line count is unchanged"));
    check(before.endsWith('\n') == after.endsWith('\n'),
          QStringLiteral("the final newline, or its absence, is preserved"));
    check(after.contains("ServerMods="),
          QStringLiteral("ServerMods is left where it was"));
    check(after.contains(QStringLiteral("@%1").arg(options.prefix).toUtf8()),
          QStringLiteral("the mod itself is on the line"));

    // Running it again must be a no-op, or every build would report a change
    // that did not happen.
    run.refresh(project);
    const RunStep again = run.writeModChain();
    check(again.ok && readFile(paths.projectCfg) == after,
          QStringLiteral("a second write changes nothing (%1)").arg(again.detail));

    // The pure rewrite, on shapes the template does not happen to ship.
    heading(QStringLiteral("The rewrite in isolation"));
    check(withModChain(QByteArray("Mods=@Old\r\nServerMods=\r\n"), { "@A", "@B" })
              == QByteArray("Mods=@A;@B\r\nServerMods=\r\n"),
          QStringLiteral("CRLF stays CRLF"));
    check(withModChain(QByteArray("ServerMods=\nPrefixes="), { "@A" })
              == QByteArray("ServerMods=\nPrefixes=\nMods=@A\n"),
          QStringLiteral("a file with no Mods line gets one appended"));
    check(withModChain(QByteArray("Mods=@Old"), {}) == QByteArray("Mods="),
          QStringLiteral("an empty chain empties the line rather than removing it"));
    check(withModChain(QByteArray("ServerMods=@X\n"), { "@A" })
              == QByteArray("ServerMods=@X\nMods=@A\n"),
          QStringLiteral("ServerMods is not mistaken for Mods"));

    // A guess must defer to what the file already says, which is the only
    // reason "@Dabs Framework" survives on a machine without Dabs installed.
    QVector<ModRef> chain;
    ModRef guess;
    guess.name = QStringLiteral("@DabsFramework");
    guess.guessed = true;
    ModRef found;
    found.name = QStringLiteral("@CommunityFramework");
    found.path = QStringLiteral("P:/Mods/@CommunityFramework");
    chain << guess << found;
    applyExistingSpelling(chain, QByteArray("Mods=@Dabs Framework;@Community Framework\n"));
    check(chain.at(0).name == QStringLiteral("@Dabs Framework"),
          QStringLiteral("a guess takes the spelling already in the file"));
    check(chain.at(1).name == QStringLiteral("@CommunityFramework"),
          QStringLiteral("a name read off a real folder is not overruled"));

    // The ServerMods line, which nothing wrote until there was a server-only
    // chain to put on it.
    check(withServerModChain(QByteArray("Mods=@A\nServerMods=\n"), {})
              == QByteArray("Mods=@A\nServerMods=\n"),
          QStringLiteral("an empty server chain leaves the file exactly as it was"));
    check(withServerModChain(QByteArray("Mods=@A\n"), {})
              == QByteArray("Mods=@A\n"),
          QStringLiteral("and a file without the line does not gain an empty one"));
    check(withServerModChain(QByteArray("Mods=@A\r\nServerMods=\r\n"), { "@Log" })
              == QByteArray("Mods=@A\r\nServerMods=@Log\r\n"),
          QStringLiteral("a server chain lands on ServerMods and leaves Mods alone"));
    check(withServerModChain(QByteArray("Mods=@A\nServerMods=@Log\n"), {})
              == QByteArray("Mods=@A\nServerMods=\n"),
          QStringLiteral("taking the last one off empties the line rather than "
                         "leaving a stale chain for Workbench"));

    // ------------------------------------------------------ the load order alone

    heading(QStringLiteral("The load order, on entries built here"));
    const auto ref = [](const QString &name, const QStringList &ships,
                        const QStringList &needs,
                        ModOrigin origin = ModOrigin::Extra) {
        ModRef m;
        m.name = name;
        m.addons = ships;
        m.requires = needs;
        m.origin = origin;
        return m;
    };

    QVector<ModRef> order;
    order << ref(QStringLiteral("@Self"), {}, {}, ModOrigin::Self)
          << ref(QStringLiteral("@COT"), { QStringLiteral("JM_COT_Scripts") },
                 { QStringLiteral("JM_CF_Scripts") })
          << ref(QStringLiteral("@Blind"), {}, {})
          << ref(QStringLiteral("@CF"), { QStringLiteral("JM_CF_Scripts") }, {});
    const QStringList orderNotes = orderModChain(order);
    for (const QString &line : orderNotes) note(line);
    check(chainNames(order)
              == QStringList{ QStringLiteral("@Blind"), QStringLiteral("@CF"),
                              QStringLiteral("@COT"), QStringLiteral("@Self") },
          QStringLiteral("the edge decides, the rest keep their places (%1)")
              .arg(chainNames(order).join(QStringLiteral(", "))));
    check(order.last().name == QStringLiteral("@Self"),
          QStringLiteral("the mod under test is pinned last even when it came in "
                         "first"));
    bool saidBlind = false;
    for (const QString &line : orderNotes)
        saidBlind = saidBlind || line.contains(QStringLiteral("@Blind"));
    check(saidBlind,
          QStringLiteral("an entry with nothing to order it by is named rather "
                         "than quietly placed"));

    // Two mods that each require the other cannot both load second, so the only
    // honest thing to do is leave them alone and say so.
    QVector<ModRef> loop;
    loop << ref(QStringLiteral("@A"), { QStringLiteral("A_Scripts") },
                { QStringLiteral("B_Scripts") })
         << ref(QStringLiteral("@B"), { QStringLiteral("B_Scripts") },
                { QStringLiteral("A_Scripts") });
    const QStringList loopNotes = orderModChain(loop);
    check(chainNames(loop)
              == QStringList{ QStringLiteral("@A"), QStringLiteral("@B") },
          QStringLiteral("a cycle keeps the order it arrived in"));
    check(loopNotes.join(QStringLiteral(" "))
              .contains(QStringLiteral("require each other")),
          QStringLiteral("and is reported rather than passed off as sorted"));

    // ---------------------------------------------------------------- junctions

    heading(QStringLiteral("Junctions"));
    const QString target = paths.modFolder;
    const QString localLink = QDir(tmp.path()).filePath(QStringLiteral("link-probe"));
    const RunStep linked = makeJunction(localLink, target);
    check(linked.ok, QStringLiteral("junction made (%1)").arg(linked.detail));
    if (!linked.command.display().isEmpty()) note(linked.command.display());
    check(QFileInfo(localLink).isJunction(), QStringLiteral("it is a junction"));
    check(QFileInfo::exists(localLink + QStringLiteral("/Workbench/project.cfg")),
          QStringLiteral("the target's files are reachable through it"));

    // Asking twice is what the button does when the work drive is already set
    // up, and it has to be a success rather than an error about a name in use.
    const RunStep twice = makeJunction(localLink, target);
    check(twice.ok && twice.command.program.isEmpty(),
          QStringLiteral("a second call is satisfied and runs nothing"));

    // Pointing at something else is a refusal, not a silent replacement.
    const QString otherTarget = QDir(tmp.path()).filePath(QStringLiteral("other"));
    QDir().mkpath(otherTarget);
    const RunStep clash = makeJunction(localLink, otherTarget);
    check(!clash.ok && clash.detail.contains(QStringLiteral("already points at")),
          QStringLiteral("a junction pointing elsewhere is refused, not replaced"));

    const RunStep unlinked = removeJunction(localLink);
    check(unlinked.ok, QStringLiteral("junction removed (%1)").arg(unlinked.detail));
    check(!QFileInfo::exists(localLink), QStringLiteral("the link is gone"));
    // The whole reason removeJunction checks before it deletes.
    check(QFileInfo::exists(target + QStringLiteral("/Workbench/project.cfg")),
          QStringLiteral("and what it pointed at is untouched"));

    const RunStep notALink = removeJunction(otherTarget);
    check(!notALink.ok && QFileInfo(otherTarget).isDir(),
          QStringLiteral("a real folder is refused rather than deleted"));

    // The same thing on the work drive, which is where the app actually does
    // it. Skipped rather than failed when there is no P:, so this suite still
    // passes on a machine that has never mounted one.
    if (QFileInfo(paths.workDrive).isDir()) {
        // The process id is in the name because P: is machine-wide and this
        // suite is not the only thing on the machine. Two runs at once under
        // one fixed name is one of them making the junction and the other
        // failing on a name in use, which reads as a broken junction rather
        // than as two tests fighting.
        const QString driveLink = QDir(paths.workDrive)
                                      .filePath(QStringLiteral("SUDO_RunTest_Probe_%1")
                                                    .arg(QCoreApplication::applicationPid()));
        const RunStep onDrive = makeJunction(driveLink, target);
        check(onDrive.ok, QStringLiteral("junction on the work drive (%1)")
                              .arg(onDrive.detail));
        check(QFileInfo(driveLink).isJunction(),
              QStringLiteral("P: link is a junction"));
        const RunStep off = removeJunction(driveLink);
        check(off.ok && !QFileInfo::exists(driveLink),
              QStringLiteral("P: link removed again"));
    } else {
        note(QStringLiteral("no work drive mounted, skipped the P: junction"));
    }

    // linkWorkDrive picks its folders the way SetupWorkdrive.bat does. Assert
    // what it decided to link without letting it write to P:.
    const QVector<RunStep> plan = run.linkWorkDrive();
    bool linksTheModFolder = false;
    for (const RunStep &step : plan) {
        note(QStringLiteral("%1: %2").arg(step.title, step.detail));
        if (step.title.contains(options.prefix)) linksTheModFolder = true;
    }
    check(linksTheModFolder,
          QStringLiteral("the folder carrying Workbench\\dayz.gproj is the one linked"));
    // Clean up whatever that actually created on the work drive.
    if (!paths.link.isEmpty() && QFileInfo(paths.link).isJunction())
        removeJunction(paths.link);

    // ---------------------------------------------------------------- missions

    heading(QStringLiteral("Missions this mod ships"));
    for (const QString &m : paths.missions)
        out << "  " << QDir::toNativeSeparators(m) << Qt::endl;

    check(paths.missions.size() == 2,
          QStringLiteral("both scaffolded maps are found (%1)")
              .arg(paths.missions.size()));
    check(!paths.mission.isEmpty() && paths.missions.contains(paths.mission),
          QStringLiteral("one of them is selected without being asked"));
    check(paths.missionFrom == QStringLiteral("shipped with this mod"),
          QStringLiteral("and it came from the mod, not from the DayZ install"));

    // Picking the other one has to stick across a re-check. A refresh that
    // quietly moved somebody back to Chernarus would be found out in game.
    const QString wantedMission = paths.missions.last();
    run.setMission(wantedMission);
    check(run.paths().mission == wantedMission,
          QStringLiteral("the picked mission is the one held"));
    run.refresh(project);
    check(run.paths().mission == wantedMission,
          QStringLiteral("and it survives a refresh (%1)")
              .arg(QFileInfo(wantedMission).fileName()));

    // ---------------------------------------------------------- command lines

    heading(QStringLiteral("Command lines, assembled and printed, never run"));
    QString error;

    const RunCommand build = run.buildCommand(false, &error);
    if (build.isValid()) {
        out << "  build   " << build.display() << Qt::endl;
        check(build.arguments.value(0).startsWith(QStringLiteral("P:")),
              QStringLiteral("the source is given on the work drive, not as C:"));
        check(build.arguments.value(1).contains(
                  QStringLiteral("Mods\\@%1\\Addons").arg(options.prefix)),
              QStringLiteral("the output goes to the only folder the engine loads from"));
        check(build.arguments.contains(
                  QStringLiteral("-prefix=%1").arg(paths.pboPrefix)),
              QStringLiteral("the prefix is passed"));
        check(!build.arguments.contains(QStringLiteral("-clear")),
              QStringLiteral("a plain build does not wipe the folder"));
        const RunCommand cleaned = run.buildCommand(true, &error);
        check(cleaned.arguments.contains(QStringLiteral("-clear")),
              QStringLiteral("a clean build does"));
    } else {
        check(!paths.addonBuilder.isEmpty(),
              QStringLiteral("build command assembled (%1)").arg(error));
        note(QStringLiteral("no AddonBuilder on this machine, command not assembled"));
    }

    const RunCommand server = run.serverCommand(&error);
    const RunCommand client = run.clientCommand(&error);
    if (server.isValid() && client.isValid()) {
        out << "  server  " << server.display() << Qt::endl;
        out << "  client  " << client.display() << Qt::endl;

        check(server.arguments.contains(QStringLiteral("-server")),
              QStringLiteral("the server is the one that gets -server"));
        check(!client.arguments.contains(QStringLiteral("-server")),
              QStringLiteral("and the client is not"));
        check(server.arguments.contains(QStringLiteral("-filePatching"))
                  && client.arguments.contains(QStringLiteral("-filePatching")),
              QStringLiteral("both sides file-patch, or the script mod does not load"));
        check(server.program.endsWith(QStringLiteral("DayZDiag_x64.exe"))
                  && client.program == server.program,
              QStringLiteral("both sides run the diag build"));
        check(client.arguments.contains(QStringLiteral("-connect=127.0.0.1")),
              QStringLiteral("the client connects to the local server"));
        check(server.arguments.contains(
                  QStringLiteral("-port=%1").arg(run.port()))
                  && client.arguments.contains(
                      QStringLiteral("-port=%1").arg(run.port())),
              QStringLiteral("both sides agree on the port"));

        const QString serverProfiles =
            QStringLiteral("-profiles=%1")
                .arg(QDir::toNativeSeparators(paths.serverProfiles));
        const QString clientProfiles =
            QStringLiteral("-profiles=%1")
                .arg(QDir::toNativeSeparators(paths.clientProfiles));
        check(server.arguments.contains(serverProfiles)
                  && client.arguments.contains(clientProfiles)
                  && serverProfiles != clientProfiles,
              QStringLiteral("server and client write their logs to separate folders"));

        const QString modArg = QStringLiteral("-mod=%1").arg(paths.modArgument());
        check(server.arguments.contains(modArg) && client.arguments.contains(modArg),
              QStringLiteral("both sides load the same chain"));
        check(paths.modArgument().contains(QStringLiteral("@%1").arg(options.prefix)),
              QStringLiteral("and the chain has this mod in it"));

        bool serverConfig = false;
        for (const QString &a : server.arguments)
            if (a.startsWith(QStringLiteral("-config="))
                && a.endsWith(QStringLiteral("server.cfg")))
                serverConfig = true;
        check(serverConfig,
              QStringLiteral("the server gets the template's server.cfg"));
    } else {
        check(!paths.diagExe.isEmpty(),
              QStringLiteral("launch commands assembled (%1)").arg(error));
        note(QStringLiteral("no diag client on this machine, commands not assembled"));
    }

    // ------------------------------------------------------- offline, one process

    heading(QStringLiteral("Offline, and the four arguments it must not carry"));
    check(run.mode() == LaunchMode::DevServer,
          QStringLiteral("the dev server is still the default"));
    run.setMode(LaunchMode::Offline);
    check(run.mode() == LaunchMode::Offline,
          QStringLiteral("the mode is what was asked for"));

    const RunCommand offline = run.offlineCommand(&error);
    if (offline.isValid()) {
        out << "  offline " << offline.display() << Qt::endl;

        check(offline.program.endsWith(QStringLiteral("DayZDiag_x64.exe")),
              QStringLiteral("offline runs the diag build too, because retail "
                             "stops at the loading screen with file patching on"));
        check(offline.arguments.contains(
                  QStringLiteral("-mission=%1")
                      .arg(QDir::toNativeSeparators(wantedMission))),
              QStringLiteral("it loads the mission that was picked"));
        check(offline.arguments.contains(
                  QStringLiteral("-profiles=%1")
                      .arg(QDir::toNativeSeparators(paths.clientProfiles))),
              QStringLiteral("its RPT goes to the client profile folder"));
        check(offline.arguments.contains(QStringLiteral("-filePatching")),
              QStringLiteral("file patching is on, or the script mod does not load"));
        check(offline.arguments.contains(
                  QStringLiteral("-mod=%1").arg(paths.modArgument())),
              QStringLiteral("it loads the same chain the pair does"));

        // The four that make a session a pair. Each one is wrong offline, and
        // passing one anyway is how a quick run turns into an hour spent
        // wondering why the game sat at the main menu.
        bool connect = false, config = false, port = false;
        for (const QString &a : offline.arguments) {
            connect = connect || a.startsWith(QStringLiteral("-connect="));
            config = config || a.startsWith(QStringLiteral("-config="));
            port = port || a.startsWith(QStringLiteral("-port="));
        }
        check(!offline.arguments.contains(QStringLiteral("-server")),
              QStringLiteral("no -server, because nothing is hosting"));
        check(!connect, QStringLiteral("no -connect, because there is nothing to "
                                       "connect to"));
        check(!config, QStringLiteral("no -config, because server.cfg is not read"));
        check(!port, QStringLiteral("no -port, because no socket is opened"));
    } else {
        check(!paths.diagExe.isEmpty(),
              QStringLiteral("offline command assembled (%1)").arg(error));
        note(QStringLiteral("no diag build on this machine, offline command not "
                            "assembled"));
    }

    // The mode picks a command. It does not rewrite the other one.
    const RunCommand serverAgain = run.serverCommand(&error);
    const RunCommand clientAgain = run.clientCommand(&error);
    check(serverAgain.display() == server.display()
              && clientAgain.display() == client.display(),
          QStringLiteral("the dev server pair is byte for byte what it was"));

    // --------------------------------------------------- what offline will not show

    heading(QStringLiteral("What an offline run will not show"));
    const QVector<OfflineLimit> limits = offlineLimits(paths.modChain);
    for (const OfflineLimit &limit : limits)
        out << "  " << limit.line() << Qt::endl;

    check(limits.size() >= 5,
          QStringLiteral("there are %1 of them").arg(limits.size()));
    bool wellFormed = true;
    for (const OfflineLimit &limit : limits)
        // The short half is what the panel puts on its one line and must not
        // carry a full stop of its own; the reason is a finished sentence. The
        // ASCII rule is asserted rather than trusted.
        wellFormed = wellFormed && !limit.what.trimmed().isEmpty()
                     && !limit.what.trimmed().endsWith(QLatin1Char('.'))
                     && limit.why.trimmed().endsWith(QLatin1Char('.'))
                     && !limit.line().contains(QChar(0x2014));
    check(wellFormed,
          QStringLiteral("each is a short name and a finished reason, no em dash"));

    const auto mentionsPermissions = [](const QVector<OfflineLimit> &lines) {
        for (const OfflineLimit &limit : lines)
            if (limit.what.contains(QStringLiteral("permission"), Qt::CaseInsensitive))
                return true;
        return false;
    };
    check(mentionsPermissions(limits),
          QStringLiteral("the permission line is printed, because COT is in the chain"));
    const QVector<OfflineLimit> withoutCot = offlineLimits({});
    check(!mentionsPermissions(withoutCot),
          QStringLiteral("and dropped from a chain that does not load COT"));
    check(withoutCot.size() == limits.size() - 1,
          QStringLiteral("nothing else moved with it"));

    // --------------------------------------------------- mods picked for this run

    heading(QStringLiteral("Mods picked to load alongside"));

    // Two mods built here whose config.cpp really does name one another, added
    // in the wrong order on purpose, plus one with no client half at all.
    const QString zeta = buildMod(tmp.path(), QStringLiteral("@SUDO_Zeta"),
                                  QStringLiteral("SUDO_Zeta_Scripts"), {});
    const QString alpha = buildMod(tmp.path(), QStringLiteral("@SUDO_Alpha"),
                                   QStringLiteral("SUDO_Alpha_Scripts"),
                                   { QStringLiteral("SUDO_Zeta_Scripts") });
    const QString logger = buildMod(tmp.path(), QStringLiteral("@SUDO_Logger"),
                                    QStringLiteral("SUDO_Logger_Scripts"), {});
    check(!zeta.isEmpty() && !alpha.isEmpty() && !logger.isEmpty(),
          QStringLiteral("three mod folders built to pick from"));

    const ModFacts alphaFacts = modFactsFor(alpha, QStringLiteral("@SUDO_Alpha"));
    note(alphaFacts.from);
    check(alphaFacts.addons == QStringList{ QStringLiteral("SUDO_Alpha_Scripts") }
              && alphaFacts.requires
                     == QStringList{ QStringLiteral("SUDO_Zeta_Scripts") },
          QStringLiteral("what a mod needs is read off its own config.cpp"));
    const ModFacts noFacts = modFactsFor(QDir(tmp.path()).filePath(
                                             QStringLiteral("no-such-mod")),
                                         QStringLiteral("@Nothing"));
    check(noFacts.addons.isEmpty() && noFacts.from.contains(QStringLiteral("config.bin")),
          QStringLiteral("and a mod that says nothing is reported as saying "
                         "nothing (%1)").arg(noFacts.from));

    Project chained = project;
    QVector<ExtraMod> picks;
    picks << pick(QStringLiteral("@SUDO_Alpha"), alpha, QStringLiteral("Alpha"))
          << pick(QStringLiteral("@SUDO_Zeta"), zeta, QStringLiteral("Zeta"))
          << pick(QStringLiteral("@SUDO_Logger"), logger, QStringLiteral("Logger"),
                  true);
    setExtraMods(chained, picks);

    TestRun chainedRun;
    chainedRun.refresh(chained);
    const TestRunPaths &cp = chainedRun.paths();
    for (const ModRef &m : cp.modChain)
        out << QStringLiteral("  %1  [%2]%3")
                   .arg(m.name, -28)
                   .arg(m.from,
                        m.serverOnly ? QStringLiteral("  server only") : QString())
            << Qt::endl;
    for (const QString &line : cp.chainNotes) note(line);

    const int zetaAt = chainIndex(cp.modChain, QStringLiteral("SUDO_Zeta"));
    const int alphaAt = chainIndex(cp.modChain, QStringLiteral("SUDO_Alpha"));
    check(zetaAt >= 0 && alphaAt >= 0 && zetaAt < alphaAt,
          QStringLiteral("a picked mod loads after the picked mod it requires, "
                         "not in the order it was ticked (%1 then %2)")
              .arg(zetaAt).arg(alphaAt));

    // A run's picks must not quietly replace what the mod itself declares.
    int declaredCount = 0, extraCount = 0, selfCount = 0;
    for (const ModRef &m : cp.modChain) {
        if (m.origin == ModOrigin::Dependency) ++declaredCount;
        if (m.origin == ModOrigin::Extra) ++extraCount;
        if (m.origin == ModOrigin::Self) ++selfCount;
    }
    check(declaredCount == chained.dependencies.size(),
          QStringLiteral("every declared dependency is still in the chain (%1)")
              .arg(declaredCount));
    check(extraCount == picks.size() && selfCount == 1,
          QStringLiteral("beside the picks, with the mod itself still last"));
    check(cp.modChain.last().origin == ModOrigin::Self,
          QStringLiteral("and the two are told apart, so the panel can draw them "
                         "apart"));
    check(chainIndexByAddon(cp.modChain, QStringLiteral("JM_CF_Scripts"))
              < chainIndexByAddon(cp.modChain, QStringLiteral("JM_COT_Scripts")),
          QStringLiteral("the declared pair is still ordered by what it requires"));

    // One mod under two names is the case the folder name cannot settle, and it
    // is the normal case rather than a contrived one: a dependency resolves to
    // the folder it is installed as, @CF, and the user adding the same mod by
    // hand writes @CommunityFramework. Nothing but the folder says they are one
    // mod, and loading a framework twice is a chain the engine will not thank
    // anyone for.
    Project sameFolder = chained;
    QVector<ExtraMod> twicePicks = picks;
    twicePicks << pick(QStringLiteral("@SUDO_Alpha_Other_Spelling"), alpha,
                       QStringLiteral("Alpha again"));
    setExtraMods(sameFolder, twicePicks);
    TestRun twiceRun;
    twiceRun.refresh(sameFolder);
    int alphaCount = 0;
    for (const ModRef &m : twiceRun.paths().modChain)
        if (!m.path.isEmpty()
            && QDir(m.path).absolutePath() == QDir(alpha).absolutePath())
            ++alphaCount;
    check(alphaCount == 1,
          QStringLiteral("one folder picked under two names is in the chain once "
                         "(%1)").arg(alphaCount));
    check(twiceRun.paths().chainNotes.join(QStringLiteral(" "))
              .contains(QStringLiteral("Other_Spelling")),
          QStringLiteral("and the copy that was dropped is named"));

    // ------------------------------------------------------ the server-only half

    heading(QStringLiteral("A mod loaded by the server and not by the client"));
    check(!cp.modArgument().contains(QStringLiteral("@SUDO_Logger")),
          QStringLiteral("a server-only mod is not in -mod="));
    check(cp.serverModArgument().contains(QStringLiteral("@SUDO_Logger")),
          QStringLiteral("it is in -serverMod="));
    check(cp.modNames().contains(QStringLiteral("@SUDO_Alpha"))
              && !cp.modNames().contains(QStringLiteral("@SUDO_Logger"))
              && cp.serverModNames()
                     == QStringList{ QStringLiteral("@SUDO_Logger") },
          QStringLiteral("and project.cfg's two lines are split the same way"));

    QString chainWhy;
    const RunCommand chainServer = chainedRun.serverCommand(&chainWhy);
    const RunCommand chainClient = chainedRun.clientCommand(&chainWhy);
    chainedRun.setMode(LaunchMode::Offline);
    const RunCommand chainOffline = chainedRun.offlineCommand(&chainWhy);
    chainedRun.setMode(LaunchMode::DevServer);
    if (chainServer.isValid() && chainClient.isValid()) {
        out << "  server  " << chainServer.display() << Qt::endl;
        out << "  client  " << chainClient.display() << Qt::endl;
        const QString modArg =
            QStringLiteral("-mod=%1").arg(cp.modArgument());
        check(chainServer.arguments.contains(modArg)
                  && chainClient.arguments.contains(modArg),
              QStringLiteral("both sides of the pair carry the whole chain"));
        check(chainServer.arguments.contains(
                  QStringLiteral("-serverMod=%1").arg(cp.serverModArgument())),
              QStringLiteral("the server is the one that gets -serverMod="));
        check(!anyArgumentStartsWith(chainClient, QStringLiteral("-serverMod=")),
              QStringLiteral("and the client is not, which is the whole point of "
                             "the parameter"));
    } else {
        note(QStringLiteral("no diag client on this machine, the pair was not "
                            "assembled"));
    }
    if (chainOffline.isValid()) {
        out << "  offline " << chainOffline.display() << Qt::endl;
        check(chainOffline.arguments.contains(
                  QStringLiteral("-mod=%1").arg(cp.modArgument())),
              QStringLiteral("offline carries the same chain the pair does"));
        check(!anyArgumentStartsWith(chainOffline, QStringLiteral("-serverMod=")),
              QStringLiteral("and no server-only chain, because nothing on disk "
                             "shows a client process taking one"));
    } else {
        note(QStringLiteral("no diag build on this machine, offline was not "
                            "assembled"));
    }

    bool saidServerOnly = false;
    for (const OfflineLimit &limit : offlineLimits(cp.modChain))
        saidServerOnly = saidServerOnly
                         || limit.what.contains(QStringLiteral("server-only"),
                                                Qt::CaseInsensitive);
    check(saidServerOnly,
          QStringLiteral("and the offline list says the server-only chain is "
                         "left out"));

    // Both lines land in the file the Workbench project reads.
    const RunStep bothLines = chainedRun.writeModChain();
    check(bothLines.ok, QStringLiteral("the two lines are written (%1)")
                            .arg(bothLines.output));
    const QByteArray withServer = readFile(paths.projectCfg);
    check(withServer.contains("ServerMods=@SUDO_Logger"),
          QStringLiteral("ServerMods carries the server-only mod"));
    // Compared line by line: "ServerMods=@SUDO_Logger" contains the whole of
    // "Mods=@SUDO_Logger", so a substring test here would pass on the wrong line.
    QByteArray modsLine;
    for (const QByteArray &line : withServer.split('\n'))
        if (line.trimmed().startsWith("Mods=")) modsLine = line.trimmed();
    check(!modsLine.isEmpty() && modsLine.contains("@SUDO_Alpha")
              && !modsLine.contains("@SUDO_Logger"),
          QStringLiteral("and the Mods line does not (%1)")
              .arg(QString::fromUtf8(modsLine)));

    // ------------------------------------------------- a pick that is not there

    heading(QStringLiteral("A mod picked for the run and then uninstalled"));
    Project stale = chained;
    QVector<ExtraMod> stalePicks = picks;
    const QString ghostName = QStringLiteral("@SUDO_Uninstalled_Probe");
    stalePicks << pick(ghostName,
                       QDir(tmp.path()).filePath(ghostName),
                       QStringLiteral("Gone"));
    setExtraMods(stale, stalePicks);

    TestRun staleRun;
    staleRun.refresh(stale);
    check(staleRun.paths().missingMods() == QStringList{ ghostName },
          QStringLiteral("the entry is reported as not installed here"));
    check(!staleRun.paths().modArgument().contains(ghostName)
              && !staleRun.paths().modNames().contains(ghostName),
          QStringLiteral("and produces no -mod= entry pointing at nothing"));
    const PrereqCheck ghostRow =
        findCheck(staleRun.check(), QStringLiteral("modchain"));
    check(ghostRow.state == PrereqState::Missing
              && ghostRow.detail.contains(ghostName),
          QStringLiteral("the checklist blocks on it before a button is pressed"));
    check(!ghostRow.fix.isEmpty(),
          QStringLiteral("and says what to do about it (%1)").arg(ghostRow.fix));

    QString ghostWhy;
    const RunCommand ghostServer = staleRun.serverCommand(&ghostWhy);
    check(!ghostServer.isValid() && ghostWhy.contains(ghostName),
          QStringLiteral("the server refuses to assemble, and names it (%1)")
              .arg(ghostWhy));
    ghostWhy.clear();
    const RunCommand ghostClient = staleRun.clientCommand(&ghostWhy);
    check(!ghostClient.isValid() && ghostWhy.contains(ghostName),
          QStringLiteral("so does the client"));
    ghostWhy.clear();
    staleRun.setMode(LaunchMode::Offline);
    const RunCommand ghostOffline = staleRun.offlineCommand(&ghostWhy);
    check(!ghostOffline.isValid() && ghostWhy.contains(ghostName),
          QStringLiteral("and so does offline"));
    check(!staleRun.missingChainReason().isEmpty(),
          QStringLiteral("one reason, given by the same call the panel prints"));

    // ------------------------------------------------ the picks, saved and read

    heading(QStringLiteral("The picks, saved with the project"));
    Project store = newProject();
    store.name = QStringLiteral("picks");
    setExtraMods(store, picks);
    const QString storePath = QDir(tmp.path()).filePath(QStringLiteral("picks.sdzn"));
    QString storeWhy;
    check(saveProject(store, storePath, &storeWhy),
          QStringLiteral("the project saved (%1)").arg(storeWhy));
    const QByteArray sdzn = readFile(storePath);
    check(sdzn.contains("\"testRun\"") && sdzn.contains("\"extraMods\""),
          QStringLiteral("the .sdzn carries them under testRun/extraMods"));

    Project reopened;
    check(loadProject(storePath, reopened, &storeWhy),
          QStringLiteral("and opened again (%1)").arg(storeWhy));
    const QVector<ExtraMod> back = extraModsOf(reopened);
    bool same = back.size() == picks.size();
    for (int i = 0; same && i < back.size(); ++i)
        same = back.at(i).name == picks.at(i).name
               && back.at(i).folder == picks.at(i).folder
               && back.at(i).label == picks.at(i).label
               && back.at(i).serverOnly == picks.at(i).serverOnly;
    check(same,
          QStringLiteral("name, folder, label and the server-only mark all came "
                         "back, in the order they were picked (%1 of %2)")
              .arg(back.size()).arg(picks.size()));

    setExtraMods(store, {});
    check(!store.extra.contains(QStringLiteral("testRun")),
          QStringLiteral("clearing the list takes the key back out, so a project "
                         "that never used the picker keeps the file it had"));

    // ------------------------------------------------------- a mod with no mission

    heading(QStringLiteral("A mod with no mission at all"));
    // Two halves, and both are needed. A mod scaffolded without missions, and
    // DayZServer pointed at an empty folder so nothing can be borrowed from it
    // either: on a machine that has DayZServer installed the borrow would hide
    // the case this is here to prove.
    const QString emptyServer = QDir(tmp.path()).filePath(QStringLiteral("no-server"));
    QDir().mkpath(emptyServer);
    const bool hadServerEnv = qEnvironmentVariableIsSet("DAYZ_SERVER_PATH");
    const QByteArray oldServerEnv = qgetenv("DAYZ_SERVER_PATH");
    qputenv("DAYZ_SERVER_PATH", QDir::toNativeSeparators(emptyServer).toLocal8Bit());

    ModTemplateOptions bare;
    bare.prefix = QStringLiteral("SUDO_RunTestBare");
    bare.displayName = QStringLiteral("Run test, no mission");
    bare.author = QStringLiteral("tests");
    bare.linkWorkDrive = false;
    const ModTemplateResult bareMade = scaffoldMod(tmp.path(), bare);
    check(bareMade.ok, QStringLiteral("a mod with no missions scaffolded (%1)")
                           .arg(bareMade.ok ? bareMade.modRoot : bareMade.error));

    Project bareProject;
    bareProject.name = bare.prefix;
    bareProject.modRoot = bareMade.modRoot;
    bareProject.modPrefix = bare.prefix;

    TestRun bareRun;
    bareRun.setMode(LaunchMode::Offline);
    bareRun.refresh(bareProject);
    check(bareRun.paths().missions.isEmpty(),
          QStringLiteral("no mission is found, and none is invented"));

    QString bareWhy;
    const RunCommand bareOffline = bareRun.offlineCommand(&bareWhy);
    check(!bareOffline.isValid() && !bareWhy.isEmpty(),
          QStringLiteral("offline refuses to assemble a command"));
    check(bareWhy.contains(QStringLiteral("mission"), Qt::CaseInsensitive)
              && bareWhy.contains(bare.prefix),
          QStringLiteral("and names the folder it wanted (%1)").arg(bareWhy));

    const PrereqCheck bareMission =
        findCheck(bareRun.check(), QStringLiteral("mission"));
    check(bareMission.state == PrereqState::Missing,
          QStringLiteral("the checklist blocks on it before the button is pressed"));
    check(!bareMission.fix.isEmpty(),
          QStringLiteral("and says what to do about it (%1)").arg(bareMission.fix));

    // The same state on a dev server is a warning rather than a block, because
    // the server has server.cfg's own template line to fall back on.
    bareRun.setMode(LaunchMode::DevServer);
    check(findCheck(bareRun.check(), QStringLiteral("mission")).state
              == PrereqState::Warning,
          QStringLiteral("a dev server treats the same state as a warning"));
    // Assembled into a variable first: the message has to read the reason this
    // call produced, not the one still sitting in bareWhy from the refusal.
    QString bareServerWhy;
    const RunCommand bareServer = bareRun.serverCommand(&bareServerWhy);
    check(bareServer.isValid() || paths.diagExe.isEmpty(),
          QStringLiteral("and still assembles a server command (%1)")
              .arg(bareServerWhy.isEmpty() ? QStringLiteral("no complaint")
                                           : bareServerWhy));
    bool bareHasMission = false;
    for (const QString &a : bareServer.arguments)
        bareHasMission = bareHasMission || a.startsWith(QStringLiteral("-mission="));
    check(!bareHasMission,
          QStringLiteral("and leaves -mission off it, so server.cfg's own "
                         "template line decides"));

    if (hadServerEnv) qputenv("DAYZ_SERVER_PATH", oldServerEnv);
    else qunsetenv("DAYZ_SERVER_PATH");

    // ------------------------------------------------------------------ the dock

    // A shape nobody has looked at is a shape nobody has checked. The panel is
    // built on the mod scaffolded above, so the picture is of a real project
    // with a real mod chain rather than of an empty dock.
    const QStringList args = QCoreApplication::arguments();
    const auto argAfter = [&args](const QString &flag) {
        const int at = args.indexOf(flag);
        return at > 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
    };
    const QString shot = argAfter(QStringLiteral("--shot"));
    const QString offlineShot = argAfter(QStringLiteral("--shot-offline"));
    const QString modsShot = argAfter(QStringLiteral("--shot-mods"));
    {
        heading(QStringLiteral("The dock"));
        theme::apply(app);
        Document doc;
        doc.project() = project;
        auto *panel = new TestPanel(&doc);
        panel->resize(760, 560);
        panel->show();
        QApplication::processEvents();
        if (!shot.isEmpty())
            out << (panel->grab().save(shot) ? "wrote " : "could not write ")
                << shot << Qt::endl;

        // The mode is a combo box in the panel and there is no other way in,
        // which is the point: this drives the same widget the user does, so a
        // selector wired to nothing would be caught here rather than in game.
        const QList<QComboBox *> boxes = panel->findChildren<QComboBox *>();
        QComboBox *modeBox = boxes.value(0);
        check(modeBox && modeBox->count() == 2,
              QStringLiteral("the dock offers both ways to run"));
        check(panel->launchAction()->text().contains(QStringLiteral("dev server"),
                                                     Qt::CaseInsensitive),
              QStringLiteral("the launch button starts on the dev server (%1)")
                  .arg(panel->launchAction()->text()));
        if (modeBox) {
            const int at = modeBox->findData(int(LaunchMode::Offline));
            check(at >= 0, QStringLiteral("offline is one of the entries"));
            modeBox->setCurrentIndex(at);
            QApplication::processEvents();
            check(panel->launchAction()->text().contains(QStringLiteral("offline"),
                                                         Qt::CaseInsensitive),
                  QStringLiteral("choosing it renames the launch button (%1)")
                      .arg(panel->launchAction()->text()));
            if (!offlineShot.isEmpty())
                out << (panel->grab().save(offlineShot) ? "wrote "
                                                        : "could not write ")
                    << offlineShot << Qt::endl;
        }

        // The picker, which is modal and therefore cannot be grabbed from
        // outside its own event loop. The timer fires inside exec, takes the
        // picture and turns the dialog down, so nothing is saved into the
        // project and the suite keeps its promise to change nothing.
        if (!modsShot.isEmpty()) {
            // Polled rather than grabbed at once: the list fills from a scan on
            // a worker thread, and a picture of an empty list says nothing about
            // the thing being looked at. Gives up after a minute either way.
            auto *shotTimer = new QTimer(panel);
            int tries = 0;
            QObject::connect(shotTimer, &QTimer::timeout, panel,
                             [&out, &modsShot, shotTimer, &tries]() {
                                 ++tries;
                                 for (QWidget *top : QApplication::topLevelWidgets()) {
                                     auto *dialog = qobject_cast<QDialog *>(top);
                                     if (!dialog || !dialog->isVisible()) continue;
                                     const QList<QTreeWidget *> lists =
                                         dialog->findChildren<QTreeWidget *>();
                                     const bool filled =
                                         !lists.isEmpty()
                                         && lists.first()->topLevelItemCount() > 0;
                                     if (!filled && tries < 240) return;
                                     shotTimer->stop();
                                     out << (dialog->grab().save(modsShot)
                                                 ? "wrote "
                                                 : "could not write ")
                                         << modsShot << Qt::endl;
                                     dialog->reject();
                                 }
                             });
            shotTimer->start(250);
            panel->modsAction()->trigger();
        }
        delete panel;
    }

    // Nothing above may have started anything.
    check(!run.isBusy(), QStringLiteral("nothing was launched by this suite"));
    check(!bareRun.isBusy(),
          QStringLiteral("and the refusal started nothing either"));

    out << Qt::endl
        << (failures == 0 ? QStringLiteral("ALL TEST RUN TESTS PASSED")
                          : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
