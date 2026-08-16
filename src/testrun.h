// Running the mod in DayZ, from inside the app.
//
// Everything else here produces files: a graph becomes Enforce Script, the
// scripts become a mod folder. None of that says whether the mod loads. This
// module covers the four things that sit between an exported script and a
// character standing in a test server, and it is plain logic with no widgets so
// the panel above it stays a view.
//
// The four things, in the order they have to happen:
//
//   1. P: junctions. Binarize and Workbench resolve every path through the work
//      drive, so a mod folder that is not junctioned to P:\<Prefix> cannot be
//      built at all. resources/mod-template/SetupWorkdrive.bat is the reference
//      and linkWorkDrive mirrors it: each child of the mod root that carries a
//      Workbench/dayz.gproj, plus everything under Dependencies.
//   2. The mod chain. Workbench/project.cfg carries the Mods line the test
//      session launches with, so declaring a dependency in the app is what
//      makes it load in a test. A project's declared dependencies are not the
//      whole answer: a mod written against another mod has to be tested with
//      that mod loaded, whether or not it was declared, so the chain also
//      carries whatever the user picked for this run. Order is the part that
//      bites. The engine loads -mod= left to right and a mod must come after
//      everything it needs, so the chain is sorted from what each entry
//      declares rather than from the order anybody clicked.
//   3. The PBO. AddonBuilder packs the folder holding config.cpp into
//      P:\Mods\@<Prefix>\Addons, which is the only place the engine looks for a
//      mod that is not from the Workshop.
//   4. The session. Either one DayZDiag_x64.exe on the mod's own mission, or
//      two of them with the server first, both with -filePatching, because a
//      script mod is unloadable without it.
//
// The two ways to run are not interchangeable and the choice is the user's.
// Offline is one process and comes up in a fraction of the time, which is what
// you want while a script is still changing every minute. A dev server is the
// only one of the two that is a server, so it is the one that can tell you the
// mod works. offlineLimits() below is the list of what the fast loop cannot
// answer, and the panel prints it beside the selector.
//
// Every step hands back what it ran and what came back rather than a boolean.
// A build or a launch that failed with nothing on screen is the thing that
// costs an evening, so the command line is kept even when the step worked and
// the output is kept even when it is empty.
//
// Nothing in here starts on its own. Writing junctions and launching a game are
// things the user asks for by pressing a button.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QProcess;
class QTimer;
struct ModDependency;
struct Project;

// A command assembled, whether or not it is ever run. Assembling one is the
// part that can be checked without a game installed, so it is a value the
// caller can print.
struct RunCommand {
    QString program;
    QStringList arguments;
    QString workingDir;

    // One paste-able line, native separators, quotes around anything with a
    // space. What the log shows and what a bug report needs.
    QString display() const;
    bool isValid() const { return !program.isEmpty(); }
};

// What one step tried, what it ran, and what came back.
struct RunStep {
    QString title;
    bool ok = false;
    RunCommand command;  // program is empty when the step ran nothing external
    QString output;      // what came back, or what was written
    QString detail;      // the reason, in words the user can act on
};

enum class PrereqState {
    Ok,
    Missing,  // blocks the step that needs it
    Warning,  // worth saying, does not block
};

// Which of the two sessions the launch button starts. Nothing else in the app
// branches on this: the prerequisites, the mod chain and the PBO are the same
// either way, and only the command that gets assembled differs.
enum class LaunchMode {
    Offline,    // one diag process on a mission, no server, no port
    DevServer,  // a diag server, then a diag client on its port
};

struct PrereqCheck {
    QString id;
    QString label;
    PrereqState state = PrereqState::Missing;
    QString detail;  // the path that was found, or what is not there
    QString fix;     // the next thing to do about it
};

// Where a chain entry came from. The distinction is not cosmetic: a declared
// dependency is a property of the mod and belongs in the .sdzn's dependency
// list, while an extra is a property of this test run and belongs to the run.
// The panel draws them apart for the same reason.
enum class ModOrigin {
    Dependency,  // declared by the project
    Extra,       // picked by the user for this run
    Self,        // the mod being tested
};

// One entry in the mod chain. The name is what project.cfg's Mods line holds;
// the path is what -mod= gets, because a bare @Name only resolves if the engine
// happens to look in the right folder and an absolute path always resolves.
struct ModRef {
    QString name;  // "@CommunityOnlineTools"
    QString path;  // the folder it was found in, empty when it is not installed
    QString from;  // which rule produced the name, so a wrong guess is traceable
    // True when nothing on this machine confirmed the spelling. Only a guess
    // gets overruled by what project.cfg already says.
    bool guessed = false;
    ModOrigin origin = ModOrigin::Dependency;
    // Loaded by the server and not by the client, so it goes in -serverMod=.
    // The evidence for that parameter is on withServerModChain below.
    bool serverOnly = false;
    // A folder was recorded for this entry and it is not there now. Different
    // from a guess, which never had a folder: a guess may still resolve against
    // the game install, while this one is a mod that was uninstalled or a Steam
    // library that moved, and launching with it in the chain would hand the
    // engine a path to nothing.
    bool missing = false;
    // What the entry ships and what it needs, in addon names, which is the only
    // vocabulary the ordering has. Either may be empty: an installed mod keeps
    // its config rapified inside config.bin, so for most of the library there is
    // nothing on disk to read and the ordering has to say so rather than invent
    // an edge.
    QStringList addons;
    QStringList requires;
    QString factsFrom;  // where those two came from, for the row's tooltip
};

// Where everything lives for one project, resolved once per refresh. Any field
// may be empty: that is what the prerequisite list is for.
struct TestRunPaths {
    QString modRoot;        // the folder the template scaffolded
    QString modPrefix;      // "SUDO_Link"
    QString modFolder;      // <modRoot>/<prefix>, what P:\<prefix> points at
    QString sourceDir;      // the folder holding config.cpp, which is what packs
    QString pboPrefix;      // that folder's path inside the PBO
    QString workbenchDir;   // <modFolder>/Workbench
    QString projectCfg;     // its project.cfg, carrying Mods and ServerMods
    QString serverCfg;      // its server.cfg, with allowFilePatching already on
    QString workDrive;      // "P:/"
    QString link;           // <workDrive>/<prefix>
    QString dayzTools;      // the DayZ Tools install
    QString toolsFrom;      // which rule found it, so a wrong answer is traceable
    QString addonBuilder;   // its Bin/AddonBuilder/AddonBuilder.exe
    QString gamePath;       // the DayZ client install
    QString gameFrom;
    QString diagExe;        // its DayZDiag_x64.exe, used for both server and client
    QString retailExe;      // its DayZ_x64.exe, only ever used to say why it is no good
    QString serverPath;     // the DayZServer install, for a mission to borrow
    QString deployDir;      // <workDrive>/Mods/@<prefix>/Addons
    QString pbo;            // the file a build produces there
    QStringList missions;   // every mission that can be run, in folder order
    QString mission;        // the chosen one, what -mission= gets
    QString missionFrom;    // where the list came from, so a surprise is traceable
    QString serverProfiles; // where the server writes its RPT
    QString clientProfiles; // where the client writes its RPT
    QString tempDir;        // AddonBuilder's scratch folder
    QVector<ModRef> modChain;
    // What the ordering could not decide, one line each. Printed rather than
    // hidden: a chain in the wrong order is a mod that silently does nothing,
    // and the only warning anybody gets is this list.
    QStringList chainNotes;

    QStringList modNames() const;        // for project.cfg's Mods line
    QStringList serverModNames() const;  // for its ServerMods line
    QString modArgument() const;   // for -mod=, absolute where a folder was found
    // For -serverMod=. Empty unless something in the chain was marked server
    // only, and empty is the normal state.
    QString serverModArgument() const;
    // Entries whose folder was recorded and is not there now. Anything in here
    // blocks a launch instead of being quietly dropped from the command.
    QStringList missingMods() const;
};

// ------------------------------------------------------- mods added by hand
//
// A mod the user picked to load beside their own for this run. It is not a
// dependency: nothing about the project changes, no addon is declared, and
// nothing is imported. It exists because a mod that reopens a class from
// another mod cannot be tested without that mod loaded.
//
// Persisted in the project, under Project::extra["testRun"]["extraMods"], which
// the .sdzn reader and writer carry through untouched. That is deliberate: the
// picks belong to the project rather than to the machine, so opening the same
// project tomorrow launches the same chain. The folder is stored absolute
// because an installed mod is machine-wide rather than project-relative, so a
// project carried to another machine finds the folder gone, and a folder that
// is gone is reported rather than launched.
struct ExtraMod {
    QString name;    // the @folder name, which is what -mod= carries
    QString folder;  // absolute, where it was when it was picked
    QString label;   // the mod's own name, for the list
    bool serverOnly = false;

    bool isValid() const { return !name.isEmpty(); }
};

QVector<ExtraMod> extraModsOf(const Project &project);
// Replaces the list. The caller owns marking the project modified, because this
// module has no Document and no business knowing about one.
void setExtraMods(Project &project, const QVector<ExtraMod> &mods);

// -------------------------------------------------------------- free functions
//
// The parts with no process and no state behind them, so a test can call them
// without a game installed.

// The @folder a dependency loads from, and how that answer was reached. Reads
// it off the dependency's own script folder first, because a user who pointed
// the app at P:\Mods\@CommunityOnlineTools\JM\COT\Scripts has already said
// which folder it is. Then looks under the work drive's Mods folder and the
// game's !Workshop folder. Falls back to the display name with its spaces
// intact, which is what the template's own "@Dabs Framework" line looks like,
// and says in `from` that it is a guess.
ModRef modFolderFor(const ModDependency &dep, const QString &workDrive,
                    const QString &gamePath);

// What one mod folder ships and what it needs, in addon names.
//
// Three sources, in the order they are trusted. A loose config.cpp beside the
// mod's scripts is the mod itself talking, so it wins. resources/known-mods.json
// is next: it records the requiredAddons of CF, COT and Dabs, read from their
// own configs rather than remembered. Anything else comes back empty, and the
// reason is on disk rather than a shortcoming here: an installed Workshop mod
// ships mod.cpp, meta.cpp and pbos, and its config lives inside a pbo as a
// rapified config.bin. Nothing here reads that format, so no edge is invented
// for those and the ordering says which entries it could not place.
struct ModFacts {
    QStringList addons;
    QStringList requires;
    QString from;  // in words, for the tooltip and the notes
};

// `name` is the @folder name, used to match a preset when the folder itself
// says nothing.
ModFacts modFactsFor(const QString &folder, const QString &name);

// Sorts the chain so every entry comes after the entries shipping the addons it
// requires, and returns what it could not decide.
//
// DayZ loads -mod= left to right and a mod whose dependency has not compiled
// yet does not fail, it silently does nothing, so getting this wrong is the
// expensive kind of wrong. Where an edge exists it is obeyed. Where none does,
// the entry keeps the place it came in at, so the result is stable and the user
// can still decide by ordering the list. Anything marked Self is pinned last
// whatever the edges say: it is the mod being tested and everything else in the
// chain is something it is written against.
//
// A cycle cannot be honoured by any order, so the entries in it are left in the
// order they arrived and named in the returned notes.
QStringList orderModChain(QVector<ModRef> &chain);

// The whole chain for a project: every declared dependency, every mod the user
// added for this run, then the mod itself last, because a mod loads after what
// it is written against. Ordered by orderModChain before it comes back, and
// `notes` collects everything the caller should print about how that went.
QVector<ModRef> modChainFor(const Project &project, const QString &workDrive,
                            const QString &gamePath, QStringList *notes = nullptr);

// A guessed name that project.cfg already spells another way takes the file's
// spelling. "@Dabs Framework" and "@DabsFramework" name one mod, and the line
// the template shipped is the one known to load here while a guess is not.
// Names that came off a folder on disk are left alone: a folder that exists
// beats a file that can be years out of date.
void applyExistingSpelling(QVector<ModRef> &chain, const QByteArray &projectCfg);

// project.cfg with its Mods line replaced and every other byte, including line
// endings and a missing newline at the end, exactly as it came in. Appends the
// line when the file has none.
QByteArray withModChain(const QByteArray &projectCfg, const QStringList &modNames);

// The same for the ServerMods line, which Workbench keeps beside Mods and which
// nothing here used to write.
//
// -serverMod= is real and it is server side only. Two things on this machine say
// so, neither of them a recollection:
//
//   - DayZServer\server_manager\Server_manager.ps1, Bohemia's own server script,
//     line 1014, launches DayZServer_x64.exe with -config, -mod=, -serverMod=,
//     -bepath, -profiles and -port. The parameter is passed to the server
//     executable and to nothing else in that file.
//   - The same folder ships example_launch_params.txt, which is a server command
//     line carrying both "-mod=" and "-serverMod=" empty and ready to fill, plus
//     two separate lists: example_mod_list.txt holds content mods a client needs
//     and example_server_mod_list.txt holds BaseBuildingLogs and PlayerCounter,
//     which have no client half at all.
//
// So the two chains are disjoint by design, and the template's own project.cfg
// already carries an empty ServerMods line for the second one. What this app
// passes it to is DayZDiag_x64.exe -server rather than DayZServer_x64.exe: the
// same parameter on the diag build, which is the exe this app already runs with
// -server because retail refuses -filePatching.
//
// Nothing writes an empty ServerMods line over an empty one, so a project with
// no server-only mod comes back byte for byte unchanged.
QByteArray withServerModChain(const QByteArray &projectCfg,
                              const QStringList &modNames);

// mklink /J, with the two ways it can already be satisfied separated from the
// two ways it can fail. An existing junction pointing at the same target is a
// success and runs nothing; one pointing somewhere else is a failure that names
// where it points, because removing another project's link silently is how you
// break a mod you were not working on.
RunStep makeJunction(const QString &link, const QString &target);

// Removes a junction, and only a junction. A real folder at that path comes
// back as a failure rather than being deleted: the whole point of the check is
// that this function can never recurse into somebody's mod.
RunStep removeJunction(const QString &link);

// One thing an offline run cannot answer, split so the panel can print the
// short half where it only has one line and the whole of it on hover.
struct OfflineLimit {
    QString what;  // "Anything set in server.cfg"
    QString why;   // the reason, and where it was checked
    QString line() const;
};

// What an offline run cannot answer. A mod that works offline can still be
// broken on a server, and finding that out by chasing a bug that only exists on
// one side of the split is the expensive way to learn it, so the panel prints
// this beside the mode selector.
//
// Every line was checked against the vanilla scripts on the work drive or
// against the mod's own source before it was written here, and anything that
// could not be confirmed was dropped rather than repeated:
//
//   - Offline is IsServer() and not IsMultiplayer(). That is Community
//     Framework's own definition of an offline mission, in
//     3_Game/CommunityFramework/CommunityFramework.c.
//   - Vanilla gates 467 checks across 197 files on IsMultiplayer(). RPC
//     delivery is one of them: SyncEvents only reads RPC_SYNC_EVENT when the
//     game is a multiplayer client, in 3_game/syncevents.c.
//   - EntityAI::IsServerCheck only raises its "changed client side" error for
//     a multiplayer client, 3_game/entities/entityai.c.
//   - game.c's own note on IsDedicatedServer() says #ifdef SERVER is the same
//     question asked at compile time, 3_game/global/game.c.
//   - ServerConfigGetInt reads the -config file, and enableCfgGameplayFile is
//     read through it, 3_game/cfggameplayhandler.c.
//   - Community Online Tools returns true from HasPermission whenever the
//     mission is offline, and loads its roles only when the game is a
//     multiplayer server. Its own source, JMPermissionManager and
//     CommunityOnlineToolsBase.
//
// The chain decides the last line, because the permission note is only worth
// printing to somebody who is loading COT.
QVector<OfflineLimit> offlineLimits(const QVector<ModRef> &modChain);

// ------------------------------------------------------------------- the class

class TestRun : public QObject {
    Q_OBJECT
public:
    explicit TestRun(QObject *parent = nullptr);
    ~TestRun() override;

    // Re-reads the project and the machine. Touches nothing on disk.
    void refresh(const Project &project);
    const TestRunPaths &paths() const { return m_paths; }

    // Where DayZ Tools is, remembered in QSettings once the user has pointed at
    // it. The registry key the template's LaunchWorkbench.bat reads is not on
    // every machine, so discovery tries it, then the environment, then the
    // usual Steam layouts, and the answer the user gave wins over all of them.
    static QString dayzToolsPath();
    static void setDayZToolsPath(const QString &path);
    // How the current answer was reached, for the checklist's detail column.
    static QString discoverTools(QString *how = nullptr);
    static QString discoverGame(const QString &toolsPath, QString *how = nullptr);
    static QString discoverServer(const QString &gamePath, QString *how = nullptr);

    // Which session the launch button starts. Changing it assembles a
    // different command and nothing else: no path moves and no file is
    // written, so it is safe to flip while the checklist is on screen.
    LaunchMode mode() const { return m_mode; }
    void setMode(LaunchMode mode) { m_mode = mode; }

    // The mission both modes point at. Offline cannot run without one; the
    // server can, because server.cfg carries a template line of its own. The
    // choice survives a refresh, so re-checking does not silently move a user
    // who picked Enoch back to Chernarus.
    void setMission(const QString &path);

    // Every prerequisite, in the order they have to hold. Cheap enough to call
    // on every refresh: it is a handful of stat calls. The mission row is the
    // only one that reads the mode, because a mission missing offline blocks
    // and a mission missing on a server does not.
    QVector<PrereqCheck> check() const;
    // The ids that are Missing among `checks`, so a caller can say which button
    // is not going to work and why without repeating the rules.
    static QStringList missingIds(const QVector<PrereqCheck> &checks);

    // ------------------------------------------------------------------ steps

    // The junctions SetupWorkdrive.bat makes: the mod folder, then everything
    // under Dependencies. One step per link, so a chain that half worked says
    // which half.
    QVector<RunStep> linkWorkDrive();

    // The Mods line in Workbench/project.cfg, and its ServerMods line when
    // something in the chain is marked server only.
    RunStep writeModChain();

    // Why a launch will not assemble while the chain names a folder that is not
    // there, or empty when it does not. Public because the panel says it before
    // the button is pressed and every launch command says it again on refusal.
    QString missingChainReason() const;

    // Assembled, not run. `error` says which path was missing when the command
    // comes back invalid.
    RunCommand buildCommand(bool clean, QString *error = nullptr) const;
    RunCommand serverCommand(QString *error = nullptr) const;
    RunCommand clientCommand(QString *error = nullptr) const;
    // One process on the mission. No -server, no -config, no port and no
    // -connect: there is nothing to connect to.
    RunCommand offlineCommand(QString *error = nullptr) const;

    // Start the assembled command. False and a reason when it could not start;
    // the log carries everything after that.
    bool startBuild(bool clean, QString *error = nullptr);
    // Offline, or the server and then the client once the server has had time
    // to open its port. Which one is mode()'s answer.
    bool startTest(QString *error = nullptr);

    // Kills what this object started, server first, and cancels a client that
    // has not been spawned yet. Nothing else on the machine is touched: killing
    // every DayZDiag_x64.exe would take down a session the user started by hand.
    QVector<RunStep> stop();

    bool isBuilding() const;
    bool isTestRunning() const;
    bool isBusy() const { return isBuilding() || isTestRunning(); }

    int port() const { return m_port; }
    void setPort(int port) { m_port = port; }

signals:
    // One line for the log view. Commands arrive with a "> " in front so the
    // thing that was run stands out from what it printed.
    void log(const QString &line);
    void stepFinished(const RunStep &step);
    // Anything that changes what the buttons should allow.
    void busyChanged();

private:
    // stdout and stderr are merged, then cut into lines, because AddonBuilder
    // writes progress to one and errors to the other and reading them apart
    // puts the error somewhere other than under the file it came from.
    void pipe(QProcess *proc, QString *carry);
    void flushCarry(QString *carry);
    QProcess *makeProcess(const QString &title, QString *carry);
    void emitCommand(const RunCommand &cmd);

    TestRunPaths m_paths;
    QProcess *m_build = nullptr;
    QProcess *m_server = nullptr;
    QProcess *m_client = nullptr;
    QTimer *m_clientDelay = nullptr;
    // Assembled when the session starts, not when the timer fires. A refresh
    // during the wait would otherwise hand the client a different mod chain
    // from the one the server came up with.
    RunCommand m_pendingClient;
    QString m_buildCarry;
    QString m_serverCarry;
    QString m_clientCarry;
    int m_port = 2302;
    // The dev server is the default because it is what this app already did
    // and because it is the run that can actually clear a mod. Offline is the
    // faster answer, not the more complete one.
    LaunchMode m_mode = LaunchMode::DevServer;
    // The user's pick, kept apart from the resolved path so a refresh that
    // finds the folder gone does not forget which one was wanted.
    QString m_mission;
};
