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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

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
    project.dependencies = { dabs,
                             knownDependency(QStringLiteral("JM_CF_Scripts")),
                             knownDependency(QStringLiteral("JM_COT_Scripts")) };
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

    check(paths.modChain.size() == project.dependencies.size() + 1,
          QStringLiteral("every dependency, plus the mod itself"));
    check(!paths.modChain.isEmpty()
              && paths.modChain.last().name == QStringLiteral("@") + options.prefix,
          QStringLiteral("the mod loads last, after what it is written against"));

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
        const QString driveLink =
            QDir(paths.workDrive).filePath(QStringLiteral("SUDO_RunTest_Probe"));
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
