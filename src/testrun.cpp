#include "testrun.h"

#include "moddeps.h"
#include "project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

namespace {

const QLatin1String kSettingsTools("testrun/dayzToolsPath");

// The one folder the engine loads an unpublished mod from. Anything built
// anywhere else is invisible to the game however correct the PBO is.
const QLatin1String kModsFolder("Mods");

// Long enough for the server to bind its port on a cold start with a mod chain
// to load. Shorter and the client knocks on a socket that is not listening yet
// and drops to the main menu, which reads as the mod having failed.
constexpr int kClientDelayMs = 8000;

bool isDir(const QString &path)
{
    return !path.isEmpty() && QFileInfo(path).isDir();
}

bool isFile(const QString &path)
{
    return !path.isEmpty() && QFileInfo(path).isFile();
}

QString clean(const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

QString firstDir(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (isDir(c)) return clean(c);
    return {};
}

QString quoted(const QString &text)
{
    return text.contains(QLatin1Char(' ')) ? QLatin1Char('"') + text + QLatin1Char('"')
                                           : text;
}

// The child of the mod root that gets junctioned to P:. SetupWorkdrive.bat keys
// on Workbench/dayz.gproj, so this does too rather than guessing from the name:
// a mod folder renamed on disk still junctions to the prefix its project was
// built with. Falls back to the child holding Scripts, then to the root's own
// name, so a folder assembled by hand still resolves to something.
QString prefixOf(const QString &modRoot, const QString &declared)
{
    if (!declared.isEmpty()) return declared;
    const QDir root(modRoot);
    const QStringList children = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                QDir::Name);
    for (const QString &child : children)
        if (isFile(root.filePath(child + QStringLiteral("/Workbench/dayz.gproj"))))
            return child;
    for (const QString &child : children)
        if (isDir(root.filePath(child + QStringLiteral("/Scripts"))))
            return child;
    return root.dirName();
}

// Every drive letter, for the Steam library sweep. QDir::drives is one call to
// GetLogicalDrives, so it costs nothing next to walking a folder tree.
QStringList driveRoots()
{
    QStringList roots;
    for (const QFileInfo &d : QDir::drives()) roots << d.absoluteFilePath();
    return roots;
}

// The Steam layouts a second library folder actually takes on this platform.
// The default install is under Program Files (x86); a library added later is
// <drive>\SteamLibrary, which is where this machine keeps DayZ.
QStringList steamCandidates(const QString &leaf)
{
    QStringList out;
    for (const QString &root : driveRoots()) {
        out << root + QStringLiteral("SteamLibrary/steamapps/common/") + leaf
            << root + QStringLiteral("Program Files (x86)/Steam/steamapps/common/") + leaf
            << root + QStringLiteral("Program Files/Steam/steamapps/common/") + leaf
            << root + QStringLiteral("Steam/steamapps/common/") + leaf
            << root + QStringLiteral("Games/Steam/steamapps/common/") + leaf
            << root + QStringLiteral("steamapps/common/") + leaf;
    }
    return out;
}

QString registryString(const QString &key, const QString &value,
                       QSettings::Format format)
{
    QSettings reg(key, format);
    return reg.value(value).toString().trimmed();
}

// A name a mod folder could plausibly be called. "Community Online Tools" ships
// as @CommunityOnlineTools and "Dabs Framework" ships as @Dabs Framework, so
// both spellings are tried rather than one being declared correct.
QStringList folderNamesFor(const ModDependency &dep)
{
    QStringList names;
    const QString display = dep.displayName.trimmed();
    if (!display.isEmpty()) {
        QString packed = display;
        packed.remove(QLatin1Char(' '));
        names << QStringLiteral("@") + packed;
        names << QStringLiteral("@") + display;
    }
    if (!dep.id.isEmpty()) names << QStringLiteral("@") + dep.id;
    names.removeDuplicates();
    return names;
}

// The folder inside `parent` whose name matches one of `names`, compared the
// way Windows compares them. Returns the folder as it is actually spelt on
// disk, because that spelling is what -mod= has to carry.
QString matchFolder(const QString &parent, const QStringList &names)
{
    if (!isDir(parent)) return {};
    const QDir dir(parent);
    for (const QString &want : names)
        for (const QString &have :
             dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (have.compare(want, Qt::CaseInsensitive) == 0)
                return clean(dir.filePath(have));
    return {};
}

PrereqCheck ok(const QString &id, const QString &label, const QString &detail)
{
    PrereqCheck c;
    c.id = id;
    c.label = label;
    c.state = PrereqState::Ok;
    c.detail = detail;
    return c;
}

PrereqCheck bad(const QString &id, const QString &label, const QString &detail,
                const QString &fix, PrereqState state = PrereqState::Missing)
{
    PrereqCheck c;
    c.id = id;
    c.label = label;
    c.state = state;
    c.detail = detail;
    c.fix = fix;
    return c;
}

// Runs something short and waits for it. Only used for mklink and taskkill,
// which are instant; anything that streams gets a live QProcess instead.
RunStep runNow(const QString &title, const RunCommand &cmd, int timeoutMs = 15000)
{
    RunStep step;
    step.title = title;
    step.command = cmd;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    if (!cmd.workingDir.isEmpty()) proc.setWorkingDirectory(cmd.workingDir);
    proc.start(cmd.program, cmd.arguments);
    if (!proc.waitForStarted(timeoutMs)) {
        step.detail = QStringLiteral("Could not start %1: %2")
                          .arg(cmd.program, proc.errorString());
        return step;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        step.output = QString::fromLocal8Bit(proc.readAll()).trimmed();
        step.detail = QStringLiteral("%1 did not finish within %2 seconds.")
                          .arg(cmd.program).arg(timeoutMs / 1000);
        return step;
    }
    step.output = QString::fromLocal8Bit(proc.readAll()).trimmed();
    step.ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    if (!step.ok)
        step.detail = QStringLiteral("Exit code %1.").arg(proc.exitCode());
    return step;
}

} // namespace

// ---------------------------------------------------------------- value types

QString RunCommand::display() const
{
    if (program.isEmpty()) return {};
    QStringList parts;
    parts << quoted(QDir::toNativeSeparators(program));
    for (const QString &a : arguments) parts << quoted(a);
    return parts.join(QLatin1Char(' '));
}

QStringList TestRunPaths::modNames() const
{
    QStringList names;
    for (const ModRef &m : modChain)
        if (!m.name.isEmpty()) names << m.name;
    return names;
}

QString TestRunPaths::modArgument() const
{
    QStringList parts;
    for (const ModRef &m : modChain) {
        const QString value = m.path.isEmpty() ? m.name : m.path;
        if (!value.isEmpty()) parts << QDir::toNativeSeparators(value);
    }
    return parts.join(QLatin1Char(';'));
}

// -------------------------------------------------------------- the mod chain

ModRef modFolderFor(const ModDependency &dep, const QString &workDrive,
                    const QString &gamePath)
{
    ModRef ref;

    // The user pointing the app at a script folder has already said which mod
    // folder it is, so that answer beats any search.
    if (!dep.scriptRoot.isEmpty()) {
        QDir walk(clean(dep.scriptRoot));
        for (;;) {
            const QString name = walk.dirName();
            if (name.startsWith(QLatin1Char('@'))) {
                ref.name = name;
                ref.path = clean(walk.absolutePath());
                ref.from = QStringLiteral("from its script folder");
                return ref;
            }
            if (!walk.cdUp()) break;
        }
    }

    const QStringList names = folderNamesFor(dep);
    const QStringList places = {
        QDir(workDrive).filePath(kModsFolder),
        QDir(gamePath).filePath(QStringLiteral("!Workshop")),
        gamePath,
    };
    for (const QString &place : places) {
        const QString hit = matchFolder(place, names);
        if (hit.isEmpty()) continue;
        ref.name = QFileInfo(hit).fileName();
        ref.path = hit;
        ref.from = QStringLiteral("found in %1")
                       .arg(QDir::toNativeSeparators(clean(place)));
        return ref;
    }

    // Nothing on this machine to check against, so this is the Workshop's
    // dominant spelling rather than a fact: @CommunityFramework and
    // @CommunityOnlineTools carry no space. applyExistingSpelling lets
    // project.cfg overrule it, which is how "@Dabs Framework" survives.
    ref.name = names.isEmpty() ? QString() : names.first();
    ref.guessed = true;
    ref.from = QStringLiteral("guessed from the name; nothing here to confirm it");
    return ref;
}

QVector<ModRef> modChainFor(const Project &project, const QString &workDrive,
                            const QString &gamePath)
{
    QVector<ModRef> chain;
    for (const ModDependency &dep : project.dependencies) {
        if (!dep.isValid()) continue;
        const ModRef ref = modFolderFor(dep, workDrive, gamePath);
        if (ref.name.isEmpty()) continue;
        bool seen = false;
        for (const ModRef &have : chain)
            seen = seen || have.name.compare(ref.name, Qt::CaseInsensitive) == 0;
        if (!seen) chain.append(ref);
    }

    // The mod itself goes last: the engine loads the chain in order and a mod
    // that overrides a class its dependency declares has to come after it.
    if (project.modRoot.isEmpty()) return chain;
    const QString prefix = prefixOf(project.modRoot, project.modPrefix);
    if (!prefix.isEmpty()) {
        ModRef self;
        self.name = QStringLiteral("@") + prefix;
        self.path = clean(QDir(workDrive).filePath(QString(kModsFolder)
                                                   + QLatin1Char('/') + self.name));
        self.from = QStringLiteral("this project");
        chain.append(self);
    }
    return chain;
}

void applyExistingSpelling(QVector<ModRef> &chain, const QByteArray &projectCfg)
{
    static const QRegularExpression modsLine(
        QStringLiteral("^[ \\t]*Mods[ \\t]*=(.*)$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match =
        modsLine.match(QString::fromUtf8(projectCfg));
    if (!match.hasMatch()) return;

    const QStringList existing =
        match.captured(1).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    const auto packed = [](QString text) {
        return text.remove(QLatin1Char(' ')).trimmed();
    };
    for (ModRef &ref : chain) {
        if (!ref.guessed) continue;
        for (const QString &have : existing) {
            const QString trimmed = have.trimmed();
            if (trimmed.isEmpty()) continue;
            if (packed(trimmed).compare(packed(ref.name), Qt::CaseInsensitive) != 0)
                continue;
            if (trimmed != ref.name) {
                ref.name = trimmed;
                ref.from = QStringLiteral("kept the spelling already in project.cfg");
            }
            break;
        }
    }
}

QByteArray withModChain(const QByteArray &projectCfg, const QStringList &modNames)
{
    const QByteArray line =
        QByteArray("Mods=") + modNames.join(QLatin1Char(';')).toUtf8();
    static const QRegularExpression modsKey(QStringLiteral("^\\s*Mods\\s*="));

    QByteArray out;
    out.reserve(projectCfg.size() + line.size());
    bool replaced = false;
    int pos = 0;
    for (;;) {
        const int nl = projectCfg.indexOf('\n', pos);
        const int end = nl < 0 ? projectCfg.size() : nl;
        int contentEnd = end;
        // The carriage return belongs to the terminator, not to the content, so
        // a CRLF file stays a CRLF file after the swap.
        if (contentEnd > pos && projectCfg.at(contentEnd - 1) == '\r') --contentEnd;

        const QString text = QString::fromUtf8(projectCfg.mid(pos, contentEnd - pos));
        if (!replaced && modsKey.match(text).hasMatch()) {
            out += line;
            out += projectCfg.mid(contentEnd, end - contentEnd);
            replaced = true;
        } else {
            out += projectCfg.mid(pos, end - pos);
        }

        if (nl < 0) break;
        out += '\n';
        pos = nl + 1;
    }

    if (!replaced) {
        if (!out.isEmpty() && !out.endsWith('\n')) out += '\n';
        out += line;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------- P: junctions

RunStep makeJunction(const QString &link, const QString &target)
{
    RunStep step;
    step.title = QStringLiteral("Link %1").arg(QDir::toNativeSeparators(link));

    const QFileInfo targetInfo(target);
    if (!targetInfo.isDir()) {
        step.detail = QStringLiteral("%1 is not a folder, so there is nothing to link.")
                          .arg(QDir::toNativeSeparators(target));
        return step;
    }
    const QString wanted = clean(targetInfo.absoluteFilePath());

    const QFileInfo linkInfo(link);
    if (linkInfo.isJunction()) {
        const QString have = clean(linkInfo.junctionTarget());
        if (have.compare(wanted, Qt::CaseInsensitive) == 0) {
            step.ok = true;
            // No full stop after a path, or the path reads as if it ends in one.
            step.detail = QStringLiteral("Already points at %1")
                              .arg(QDir::toNativeSeparators(wanted));
            return step;
        }
        step.detail =
            QStringLiteral("%1 already points at %2. Remove it yourself if that is "
                           "stale: this will not delete a link it did not make.")
                .arg(QDir::toNativeSeparators(link), QDir::toNativeSeparators(have));
        return step;
    }
    if (linkInfo.exists()) {
        step.detail = QStringLiteral("%1 already exists and is a real %2.")
                          .arg(QDir::toNativeSeparators(link),
                               linkInfo.isDir() ? QStringLiteral("folder")
                                                : QStringLiteral("file"));
        return step;
    }

    // mklink is a cmd builtin, so there is no executable to call directly.
    RunCommand cmd;
    cmd.program = QStringLiteral("cmd.exe");
    cmd.arguments = { QStringLiteral("/c"), QStringLiteral("mklink"),
                      QStringLiteral("/J"), QDir::toNativeSeparators(link),
                      QDir::toNativeSeparators(wanted) };
    step = runNow(step.title, cmd);
    if (step.ok && !QFileInfo(link).isJunction()) {
        step.ok = false;
        step.detail = QStringLiteral("mklink reported success but %1 is not a junction.")
                          .arg(QDir::toNativeSeparators(link));
    }
    if (step.ok)
        step.detail = QStringLiteral("Now points at %1")
                          .arg(QDir::toNativeSeparators(wanted));
    return step;
}

RunStep removeJunction(const QString &link)
{
    RunStep step;
    step.title = QStringLiteral("Unlink %1").arg(QDir::toNativeSeparators(link));

    const QFileInfo info(link);
    if (!info.exists()) {
        step.ok = true;
        step.detail = QStringLiteral("Nothing there.");
        return step;
    }
    if (!info.isJunction()) {
        step.detail = QStringLiteral("%1 is a real folder, not a junction. Left alone.")
                          .arg(QDir::toNativeSeparators(link));
        return step;
    }
    // RemoveDirectory on a reparse point takes the link out and leaves what it
    // pointed at, which is why this is rmdir rather than removeRecursively.
    if (!QDir().rmdir(link)) {
        step.detail = QStringLiteral("Could not remove %1.")
                          .arg(QDir::toNativeSeparators(link));
        return step;
    }
    step.ok = true;
    step.detail = QStringLiteral("Removed.");
    return step;
}

// ------------------------------------------------------------------- discovery

QString TestRun::dayzToolsPath()
{
    return QSettings().value(kSettingsTools).toString();
}

void TestRun::setDayZToolsPath(const QString &path)
{
    QSettings settings;
    if (path.trimmed().isEmpty()) settings.remove(kSettingsTools);
    else settings.setValue(kSettingsTools, clean(path));
}

QString TestRun::discoverTools(QString *how)
{
    const auto answer = [how](const QString &path, const QString &source) {
        if (how) *how = source;
        return clean(path);
    };

    const QString remembered = dayzToolsPath();
    if (isDir(remembered)) return answer(remembered, QStringLiteral("remembered"));

    const QString env = qEnvironmentVariable("DAYZ_TOOLS_PATH");
    if (isDir(env)) return answer(env, QStringLiteral("DAYZ_TOOLS_PATH"));

    // The key LaunchWorkbench.bat reads. It is not written on every machine, so
    // it is one source among several rather than the only one.
    const QString reg = registryString(
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Bohemia Interactive\\Dayz Tools"),
        QStringLiteral("path"), QSettings::NativeFormat);
    if (isDir(reg)) return answer(reg, QStringLiteral("registry"));

    const QString steam = firstDir(steamCandidates(QStringLiteral("DayZ Tools")));
    if (!steam.isEmpty()) return answer(steam, QStringLiteral("Steam library"));

    if (how)
        *how = QStringLiteral("not found in the registry or any Steam library");
    return {};
}

QString TestRun::discoverGame(const QString &toolsPath, QString *how)
{
    const auto answer = [how](const QString &path, const QString &source) {
        if (how) *how = source;
        return clean(path);
    };

    const QString env = qEnvironmentVariable("DAYZ_PATH");
    if (isDir(env)) return answer(env, QStringLiteral("DAYZ_PATH"));

    // DayZ Tools keeps the game path it was configured with, which is the one
    // the user's own builds already resolve against.
    const QString ini = QDir(toolsPath).filePath(QStringLiteral("settings.ini"));
    if (isFile(ini)) {
        const QString path =
            QSettings(ini, QSettings::IniFormat).value(QStringLiteral("Game/path"))
                .toString().trimmed();
        if (isDir(path)) return answer(path, QStringLiteral("DayZ Tools settings.ini"));
    }

    // The installer writes this under the 32-bit view, so the 64-bit view of the
    // same key is empty and reading only NativeFormat finds nothing.
    for (const QSettings::Format format : { QSettings::Registry32Format,
                                            QSettings::NativeFormat }) {
        const QString reg = registryString(
            QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Bohemia Interactive\\DayZ"),
            QStringLiteral("main"), format);
        if (isDir(reg)) return answer(reg, QStringLiteral("registry"));
    }

    const QString steam = firstDir(steamCandidates(QStringLiteral("DayZ")));
    if (!steam.isEmpty()) return answer(steam, QStringLiteral("Steam library"));

    if (how) *how = QStringLiteral("not found in the registry or any Steam library");
    return {};
}

QString TestRun::discoverServer(const QString &gamePath, QString *how)
{
    const auto answer = [how](const QString &path, const QString &source) {
        if (how) *how = source;
        return clean(path);
    };

    const QString env = qEnvironmentVariable("DAYZ_SERVER_PATH");
    if (isDir(env)) return answer(env, QStringLiteral("DAYZ_SERVER_PATH"));

    // Steam puts the server beside the client in the same library folder.
    if (!gamePath.isEmpty()) {
        const QString sibling =
            QDir(gamePath).filePath(QStringLiteral("../DayZServer"));
        if (isDir(sibling)) return answer(sibling, QStringLiteral("beside the game"));
    }

    const QString steam = firstDir(steamCandidates(QStringLiteral("DayZServer")));
    if (!steam.isEmpty()) return answer(steam, QStringLiteral("Steam library"));

    if (how) *how = QStringLiteral("not installed");
    return {};
}

// ----------------------------------------------------------------- the class

TestRun::TestRun(QObject *parent) : QObject(parent)
{
    m_clientDelay = new QTimer(this);
    m_clientDelay->setSingleShot(true);
    connect(m_clientDelay, &QTimer::timeout, this, [this]() {
        const RunCommand cmd = m_pendingClient;
        m_pendingClient = RunCommand();
        if (!cmd.isValid()) {
            emit log(QStringLiteral("! The client command went missing during the "
                                    "wait; nothing was started."));
            emit busyChanged();
            return;
        }
        m_client = makeProcess(QStringLiteral("Client"), &m_clientCarry);
        emitCommand(cmd);
        m_client->setWorkingDirectory(cmd.workingDir);
        m_client->start(cmd.program, cmd.arguments);
        emit busyChanged();
    });
}

TestRun::~TestRun()
{
    // Closing the editor takes the session with it, server first. QProcess
    // would kill these anyway on the way out, but as a warning with no reason
    // attached, and a diag server left running with nothing on screen to stop
    // it is worse than losing the session.
    for (QProcess *proc : { m_server, m_client, m_build }) {
        if (!proc || proc->state() == QProcess::NotRunning) continue;
        proc->kill();
        proc->waitForFinished(3000);
    }
}

void TestRun::refresh(const Project &project)
{
    TestRunPaths p;
    p.modRoot = clean(project.modRoot);
    p.modPrefix = p.modRoot.isEmpty()
                      ? project.modPrefix
                      : prefixOf(p.modRoot, project.modPrefix);

    // The work drive letter is fixed by the tools, not by us: binarize resolves
    // every path through it and the template's own scripts hardcode P:.
    p.workDrive = QStringLiteral("P:/");

    p.dayzTools = discoverTools(&p.toolsFrom);
    p.addonBuilder =
        p.dayzTools.isEmpty()
            ? QString()
            : clean(QDir(p.dayzTools)
                        .filePath(QStringLiteral("Bin/AddonBuilder/AddonBuilder.exe")));
    if (!isFile(p.addonBuilder)) p.addonBuilder.clear();

    p.gamePath = discoverGame(p.dayzTools, &p.gameFrom);
    p.diagExe = p.gamePath.isEmpty()
                    ? QString()
                    : clean(QDir(p.gamePath).filePath(QStringLiteral("DayZDiag_x64.exe")));
    if (!isFile(p.diagExe)) p.diagExe.clear();
    p.serverPath = discoverServer(p.gamePath, nullptr);

    if (!p.modRoot.isEmpty() && !p.modPrefix.isEmpty()) {
        const QDir root(p.modRoot);
        p.modFolder = clean(root.filePath(p.modPrefix));
        p.workbenchDir = clean(QDir(p.modFolder).filePath(QStringLiteral("Workbench")));
        p.projectCfg = clean(QDir(p.workbenchDir).filePath(QStringLiteral("project.cfg")));
        p.serverCfg = clean(QDir(p.workbenchDir).filePath(QStringLiteral("server.cfg")));
        p.link = clean(QDir(p.workDrive).filePath(p.modPrefix));

        // What packs is the folder holding config.cpp, because that is the file
        // the engine reads CfgPatches and CfgMods out of and it has to sit at
        // the root of the PBO. The template puts it under Scripts; a mod folder
        // assembled by hand often has it one level up.
        const QString scripts = QDir(p.modFolder).filePath(QStringLiteral("Scripts"));
        if (isFile(QDir(scripts).filePath(QStringLiteral("config.cpp")))) {
            p.sourceDir = clean(scripts);
            p.pboPrefix = p.modPrefix + QStringLiteral("\\Scripts");
        } else if (isFile(QDir(p.modFolder).filePath(QStringLiteral("config.cpp")))) {
            p.sourceDir = p.modFolder;
            p.pboPrefix = p.modPrefix;
        }
        // A $PBOPREFIX$ beside the config is the mod author's own answer and
        // AddonBuilder honours it, so following it keeps the two in step.
        if (!p.sourceDir.isEmpty()) {
            const QString marker = QDir(p.sourceDir).filePath(QStringLiteral("$PBOPREFIX$"));
            QFile file(marker);
            if (file.open(QIODevice::ReadOnly)) {
                const QString declared = QString::fromUtf8(file.readAll()).trimmed();
                if (!declared.isEmpty()) p.pboPrefix = declared;
            }
        }

        p.deployDir = clean(QDir(p.workDrive)
                                .filePath(QStringLiteral("%1/@%2/Addons")
                                              .arg(QString(kModsFolder), p.modPrefix)));
        p.pbo = clean(QDir(p.deployDir).filePath(p.modPrefix + QStringLiteral(".pbo")));
        p.tempDir = clean(QDir(p.workDrive)
                              .filePath(QStringLiteral("temp/") + p.modPrefix));

        // Server and client keep separate profile folders. Sharing one means two
        // processes writing the same RPT, and the log you need is the one that
        // gets overwritten.
        p.serverProfiles =
            clean(QDir(p.modRoot).filePath(QStringLiteral("Profiles/Server")));
        p.clientProfiles =
            clean(QDir(p.modRoot).filePath(QStringLiteral("Profiles/Client")));

        // The mod's own mission first, because that is the one the user can edit
        // without touching the DayZ install.
        const QDir missions(QDir(p.modRoot).filePath(QStringLiteral("Missions")));
        const QStringList own =
            missions.entryList({ p.modPrefix + QStringLiteral(".*") },
                               QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (!own.isEmpty()) {
            p.mission = clean(missions.filePath(own.first()));
        } else if (!p.serverPath.isEmpty()) {
            const QString offline =
                QDir(p.serverPath)
                    .filePath(QStringLiteral("mpmissions/dayzOffline.chernarusplus"));
            if (isDir(offline)) p.mission = clean(offline);
        }
    }

    p.modChain = modChainFor(project, p.workDrive, p.gamePath);
    // Reconciled here rather than at write time, so the chain the launch uses
    // and the chain written into project.cfg can never be two different things.
    if (isFile(p.projectCfg)) {
        QFile file(p.projectCfg);
        if (file.open(QIODevice::ReadOnly))
            applyExistingSpelling(p.modChain, file.readAll());
    }
    m_paths = p;
}

QVector<PrereqCheck> TestRun::check() const
{
    const TestRunPaths &p = m_paths;
    QVector<PrereqCheck> out;

    if (p.modRoot.isEmpty()) {
        out << bad(QStringLiteral("modroot"), QStringLiteral("Mod folder"),
                   QStringLiteral("This project has no mod folder."),
                   QStringLiteral("File, Set mod folder, or File, New mod."));
    } else if (!isDir(p.modFolder)) {
        out << bad(QStringLiteral("modroot"), QStringLiteral("Mod folder"),
                   QStringLiteral("%1 is not there.")
                       .arg(QDir::toNativeSeparators(p.modFolder)),
                   QStringLiteral("Point the project at the folder the template "
                                  "scaffolded."));
    } else {
        out << ok(QStringLiteral("modroot"), QStringLiteral("Mod folder"),
                  QDir::toNativeSeparators(p.modFolder));
    }

    if (isDir(p.workDrive))
        out << ok(QStringLiteral("workdrive"), QStringLiteral("Work drive"),
                  QDir::toNativeSeparators(p.workDrive));
    else
        out << bad(QStringLiteral("workdrive"), QStringLiteral("Work drive"),
                   QStringLiteral("%1 is not mounted.")
                       .arg(QDir::toNativeSeparators(p.workDrive)),
                   QStringLiteral("Mount it from DayZ Tools, or with "
                                  "subst P: <your work drive folder>."));

    const QString vanilla = QDir(p.workDrive).filePath(QStringLiteral("DZ"));
    if (isDir(vanilla))
        out << ok(QStringLiteral("vanilla"), QStringLiteral("Vanilla data"),
                  QDir::toNativeSeparators(clean(vanilla)));
    else
        out << bad(QStringLiteral("vanilla"), QStringLiteral("Vanilla data"),
                   QStringLiteral("%1 is not there, so binarize has nothing to "
                                  "resolve against.")
                       .arg(QDir::toNativeSeparators(vanilla)),
                   QStringLiteral("Unpack the game data to the work drive from "
                                  "DayZ Tools."),
                   PrereqState::Warning);

    if (p.link.isEmpty()) {
        out << bad(QStringLiteral("junction"), QStringLiteral("Work drive link"),
                   QStringLiteral("No mod folder to link."), QString());
    } else {
        const QFileInfo linkInfo(p.link);
        const QString want = QDir::toNativeSeparators(p.modFolder);
        if (linkInfo.isJunction()
            && clean(linkInfo.junctionTarget()).compare(p.modFolder,
                                                        Qt::CaseInsensitive) == 0)
            out << ok(QStringLiteral("junction"), QStringLiteral("Work drive link"),
                      QStringLiteral("%1 points at %2")
                          .arg(QDir::toNativeSeparators(p.link), want));
        else if (linkInfo.isJunction())
            out << bad(QStringLiteral("junction"), QStringLiteral("Work drive link"),
                       QStringLiteral("%1 points at %2, not at %3.")
                           .arg(QDir::toNativeSeparators(p.link),
                                QDir::toNativeSeparators(clean(linkInfo.junctionTarget())),
                                want),
                       QStringLiteral("Remove that junction yourself, then set up "
                                      "the work drive again."));
        else if (linkInfo.exists())
            out << bad(QStringLiteral("junction"), QStringLiteral("Work drive link"),
                       QStringLiteral("%1 is a real folder.")
                           .arg(QDir::toNativeSeparators(p.link)),
                       QStringLiteral("Move or rename it, then set up the work "
                                      "drive again."));
        else
            out << bad(QStringLiteral("junction"), QStringLiteral("Work drive link"),
                       QStringLiteral("%1 is not linked yet.")
                           .arg(QDir::toNativeSeparators(p.link)),
                       QStringLiteral("Press Set up work drive."));
    }

    if (!p.addonBuilder.isEmpty())
        out << ok(QStringLiteral("tools"), QStringLiteral("AddonBuilder"),
                  QStringLiteral("%1 (%2)")
                      .arg(QDir::toNativeSeparators(p.addonBuilder), p.toolsFrom));
    else
        out << bad(QStringLiteral("tools"), QStringLiteral("AddonBuilder"),
                   p.dayzTools.isEmpty()
                       ? QStringLiteral("DayZ Tools was not found.")
                       : QStringLiteral("No Bin\\AddonBuilder\\AddonBuilder.exe "
                                        "under %1.")
                             .arg(QDir::toNativeSeparators(p.dayzTools)),
                   QStringLiteral("Press Set DayZ Tools folder and point at the "
                                  "install."));

    if (!p.diagExe.isEmpty())
        out << ok(QStringLiteral("diag"), QStringLiteral("Diag client"),
                  QStringLiteral("%1 (%2)")
                      .arg(QDir::toNativeSeparators(p.diagExe), p.gameFrom));
    else
        out << bad(QStringLiteral("diag"), QStringLiteral("Diag client"),
                   p.gamePath.isEmpty()
                       ? QStringLiteral("The DayZ install was not found.")
                       : QStringLiteral("No DayZDiag_x64.exe under %1.")
                             .arg(QDir::toNativeSeparators(p.gamePath)),
                   QStringLiteral("Install the diagnostic build from the DayZ "
                                  "Tools launcher. The retail exe cannot load a "
                                  "file-patched mod."));

    if (isFile(p.projectCfg))
        out << ok(QStringLiteral("projectcfg"), QStringLiteral("Mod chain"),
                  QStringLiteral("Mods=%1").arg(p.modNames().join(QLatin1Char(';'))));
    else
        out << bad(QStringLiteral("projectcfg"), QStringLiteral("Mod chain"),
                   QStringLiteral("No %1.")
                       .arg(QDir::toNativeSeparators(p.projectCfg)),
                   QStringLiteral("The template writes this file. Scaffold the mod "
                                  "with File, New mod."),
                   PrereqState::Warning);

    if (isFile(p.serverCfg))
        out << ok(QStringLiteral("servercfg"), QStringLiteral("Server config"),
                  QDir::toNativeSeparators(p.serverCfg));
    else
        out << bad(QStringLiteral("servercfg"), QStringLiteral("Server config"),
                   QStringLiteral("No %1.")
                       .arg(QDir::toNativeSeparators(p.serverCfg)),
                   QStringLiteral("The template ships one with allowFilePatching "
                                  "already on."));

    if (!p.mission.isEmpty())
        out << ok(QStringLiteral("mission"), QStringLiteral("Mission"),
                  QDir::toNativeSeparators(p.mission));
    else
        out << bad(QStringLiteral("mission"), QStringLiteral("Mission"),
                   QStringLiteral("No mission under the mod folder and no "
                                  "DayZServer install to borrow one from."),
                   QStringLiteral("The server will fall back to the template line "
                                  "in server.cfg."),
                   PrereqState::Warning);

    if (isFile(p.pbo))
        out << ok(QStringLiteral("pbo"), QStringLiteral("Built PBO"),
                  QStringLiteral("%1, %2 KB")
                      .arg(QDir::toNativeSeparators(p.pbo))
                      .arg(QFileInfo(p.pbo).size() / 1024));
    else
        out << bad(QStringLiteral("pbo"), QStringLiteral("Built PBO"),
                   p.pbo.isEmpty()
                       ? QStringLiteral("No mod folder, so there is nothing to pack.")
                       : QStringLiteral("%1 has not been built.")
                             .arg(QDir::toNativeSeparators(p.pbo)),
                   QStringLiteral("Press Build PBO."), PrereqState::Warning);

    return out;
}

QStringList TestRun::missingIds(const QVector<PrereqCheck> &checks)
{
    QStringList ids;
    for (const PrereqCheck &c : checks)
        if (c.state == PrereqState::Missing) ids << c.id;
    return ids;
}

// ---------------------------------------------------------------------- steps

QVector<RunStep> TestRun::linkWorkDrive()
{
    QVector<RunStep> steps;
    if (m_paths.modRoot.isEmpty()) {
        RunStep step;
        step.title = QStringLiteral("Set up work drive");
        step.detail = QStringLiteral("This project has no mod folder.");
        steps << step;
        return steps;
    }
    if (!isDir(m_paths.workDrive)) {
        RunStep step;
        step.title = QStringLiteral("Set up work drive");
        step.detail = QStringLiteral("%1 is not mounted.")
                          .arg(QDir::toNativeSeparators(m_paths.workDrive));
        steps << step;
        return steps;
    }

    const QDir root(m_paths.modRoot);
    const QDir drive(m_paths.workDrive);

    // SetupWorkdrive.bat's own rule: a child that carries Workbench/dayz.gproj
    // is a mod and gets its own letter on the work drive.
    for (const QString &child :
         root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (child.compare(QStringLiteral("Dependencies"), Qt::CaseInsensitive) == 0)
            continue;
        const QString folder = root.filePath(child);
        if (!isFile(QDir(folder).filePath(QStringLiteral("Workbench/dayz.gproj"))))
            continue;
        steps << makeJunction(clean(drive.filePath(child)), clean(folder));
    }

    // Everything under Dependencies, the same way, so a framework checked out
    // beside the mod resolves through P: as well.
    const QDir deps(root.filePath(QStringLiteral("Dependencies")));
    if (deps.exists())
        for (const QString &child :
             deps.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            steps << makeJunction(clean(drive.filePath(child)),
                                  clean(deps.filePath(child)));

    if (steps.isEmpty()) {
        RunStep step;
        step.title = QStringLiteral("Set up work drive");
        step.detail =
            QStringLiteral("No folder under %1 holds a Workbench\\dayz.gproj, so "
                           "there is nothing the work drive needs.")
                .arg(QDir::toNativeSeparators(m_paths.modRoot));
        steps << step;
    }
    return steps;
}

RunStep TestRun::writeModChain()
{
    RunStep step;
    step.title = QStringLiteral("Write the mod chain");

    if (m_paths.projectCfg.isEmpty() || !isFile(m_paths.projectCfg)) {
        step.detail = QStringLiteral("No %1 to write to.")
                          .arg(QDir::toNativeSeparators(m_paths.projectCfg));
        return step;
    }

    QFile file(m_paths.projectCfg);
    if (!file.open(QIODevice::ReadOnly)) {
        step.detail = QStringLiteral("Cannot read %1: %2")
                          .arg(QDir::toNativeSeparators(m_paths.projectCfg),
                               file.errorString());
        return step;
    }
    const QByteArray before = file.readAll();
    file.close();

    const QStringList names = m_paths.modNames();
    const QByteArray after = withModChain(before, names);
    if (after == before) {
        step.ok = true;
        step.output = QStringLiteral("Mods=%1").arg(names.join(QLatin1Char(';')));
        step.detail = QStringLiteral("Already correct.");
        return step;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        step.detail = QStringLiteral("Cannot write %1: %2")
                          .arg(QDir::toNativeSeparators(m_paths.projectCfg),
                               file.errorString());
        return step;
    }
    if (file.write(after) != qint64(after.size())) {
        step.detail = QStringLiteral("Cannot write %1: %2")
                          .arg(QDir::toNativeSeparators(m_paths.projectCfg),
                               file.errorString());
        return step;
    }
    file.close();

    step.ok = true;
    step.output = QStringLiteral("Mods=%1").arg(names.join(QLatin1Char(';')));
    QStringList how;
    for (const ModRef &m : m_paths.modChain)
        how << QStringLiteral("%1 (%2)").arg(m.name, m.from);
    step.detail = how.join(QStringLiteral(", "));
    return step;
}

RunCommand TestRun::buildCommand(bool clean_, QString *error) const
{
    const TestRunPaths &p = m_paths;
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return RunCommand();
    };

    if (p.addonBuilder.isEmpty())
        return fail(QStringLiteral("AddonBuilder was not found."));
    if (p.sourceDir.isEmpty())
        return fail(QStringLiteral("No config.cpp under %1, so there is nothing to "
                                   "pack.").arg(QDir::toNativeSeparators(p.modFolder)));
    if (p.deployDir.isEmpty())
        return fail(QStringLiteral("No mod folder, so there is nowhere to build to."));

    // Every path is given on the work drive rather than as its real location:
    // binarize rewrites paths relative to P: and a source given as C:\... comes
    // out with prefixes nothing can resolve.
    const QDir root(p.modRoot);
    const QString rel = root.relativeFilePath(p.sourceDir);
    const QString onDrive = clean(QDir(p.workDrive).filePath(rel));

    RunCommand cmd;
    cmd.program = p.addonBuilder;
    cmd.workingDir = p.workDrive;
    cmd.arguments << QDir::toNativeSeparators(onDrive)
                  << QDir::toNativeSeparators(p.deployDir)
                  << QStringLiteral("-prefix=%1").arg(p.pboPrefix)
                  << QStringLiteral("-temp=%1")
                         .arg(QDir::toNativeSeparators(p.tempDir));
    if (clean_) cmd.arguments << QStringLiteral("-clear");
    if (error) error->clear();
    return cmd;
}

RunCommand TestRun::serverCommand(QString *error) const
{
    const TestRunPaths &p = m_paths;
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return RunCommand();
    };

    if (p.diagExe.isEmpty())
        return fail(QStringLiteral("DayZDiag_x64.exe was not found."));
    if (p.serverCfg.isEmpty() || !isFile(p.serverCfg))
        return fail(QStringLiteral("No server.cfg at %1.")
                        .arg(QDir::toNativeSeparators(p.serverCfg)));

    RunCommand cmd;
    cmd.program = p.diagExe;
    cmd.workingDir = QFileInfo(p.diagExe).absolutePath();
    cmd.arguments << QStringLiteral("-server")
                  << QStringLiteral("-config=%1")
                         .arg(QDir::toNativeSeparators(p.serverCfg))
                  << QStringLiteral("-profiles=%1")
                         .arg(QDir::toNativeSeparators(p.serverProfiles));
    // Left off when there is nothing to point at, so server.cfg's own template
    // line decides rather than the server failing on an empty argument.
    if (!p.mission.isEmpty())
        cmd.arguments << QStringLiteral("-mission=%1")
                             .arg(QDir::toNativeSeparators(p.mission));
    const QString mods = p.modArgument();
    if (!mods.isEmpty()) cmd.arguments << QStringLiteral("-mod=%1").arg(mods);
    // Without this the engine reads scripts out of the PBO only, which is the
    // whole reason server.cfg sets allowFilePatching.
    cmd.arguments << QStringLiteral("-filePatching")
                  << QStringLiteral("-port=%1").arg(m_port);
    if (error) error->clear();
    return cmd;
}

RunCommand TestRun::clientCommand(QString *error) const
{
    const TestRunPaths &p = m_paths;
    if (p.diagExe.isEmpty()) {
        if (error) *error = QStringLiteral("DayZDiag_x64.exe was not found.");
        return {};
    }

    RunCommand cmd;
    cmd.program = p.diagExe;
    cmd.workingDir = QFileInfo(p.diagExe).absolutePath();
    cmd.arguments << QStringLiteral("-profiles=%1")
                         .arg(QDir::toNativeSeparators(p.clientProfiles));
    const QString mods = p.modArgument();
    if (!mods.isEmpty()) cmd.arguments << QStringLiteral("-mod=%1").arg(mods);
    cmd.arguments << QStringLiteral("-connect=127.0.0.1")
                  << QStringLiteral("-port=%1").arg(m_port)
                  << QStringLiteral("-filePatching")
                  // Windowed so the log stays readable beside it. A test session
                  // is something you watch, not something you play.
                  << QStringLiteral("-window");
    if (error) error->clear();
    return cmd;
}

// ------------------------------------------------------------------- processes

void TestRun::emitCommand(const RunCommand &cmd)
{
    if (!cmd.workingDir.isEmpty())
        emit log(QStringLiteral("  in %1").arg(QDir::toNativeSeparators(cmd.workingDir)));
    emit log(QStringLiteral("> %1").arg(cmd.display()));
}

void TestRun::pipe(QProcess *proc, QString *carry)
{
    if (!proc) return;
    QString buf = *carry + QString::fromLocal8Bit(proc->readAll());
    buf.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    // A bare carriage return is AddonBuilder redrawing its progress counter.
    // The log has no cursor to move, so each redraw becomes its own line.
    buf.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    int start = 0;
    for (;;) {
        const int nl = buf.indexOf(QLatin1Char('\n'), start);
        if (nl < 0) break;
        const QString line = buf.mid(start, nl - start);
        if (!line.trimmed().isEmpty()) emit log(line);
        start = nl + 1;
    }
    *carry = buf.mid(start);
}

void TestRun::flushCarry(QString *carry)
{
    if (carry->trimmed().isEmpty()) {
        carry->clear();
        return;
    }
    emit log(*carry);
    carry->clear();
}

QProcess *TestRun::makeProcess(const QString &title, QString *carry)
{
    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyRead, this, [this, proc, carry]() {
        pipe(proc, carry);
    });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, title](QProcess::ProcessError) {
                emit log(QStringLiteral("! %1: %2").arg(title, proc->errorString()));
            });
    connect(proc, &QProcess::finished, this,
            [this, proc, carry, title](int code, QProcess::ExitStatus status) {
                pipe(proc, carry);
                flushCarry(carry);
                RunStep step;
                step.title = title;
                step.ok = status == QProcess::NormalExit && code == 0;
                step.detail = status == QProcess::CrashExit
                                  ? QStringLiteral("%1 was killed.").arg(title)
                                  : QStringLiteral("%1 finished with exit code %2.")
                                        .arg(title).arg(code);
                emit log(step.detail);
                if (m_build == proc) m_build = nullptr;
                if (m_server == proc) m_server = nullptr;
                if (m_client == proc) m_client = nullptr;
                proc->deleteLater();
                emit stepFinished(step);
                emit busyChanged();
            });
    return proc;
}

bool TestRun::startBuild(bool clean_, QString *error)
{
    if (isBuilding()) {
        if (error) *error = QStringLiteral("A build is already running.");
        return false;
    }
    QString why;
    const RunCommand cmd = buildCommand(clean_, &why);
    if (!cmd.isValid()) {
        if (error) *error = why;
        return false;
    }
    // AddonBuilder does not create either of these and fails on the missing
    // folder rather than saying which one it wanted.
    QDir().mkpath(m_paths.deployDir);
    QDir().mkpath(m_paths.tempDir);

    m_buildCarry.clear();
    m_build = makeProcess(QStringLiteral("Build"), &m_buildCarry);
    emitCommand(cmd);
    m_build->setWorkingDirectory(cmd.workingDir);
    m_build->start(cmd.program, cmd.arguments);
    emit busyChanged();
    if (error) error->clear();
    return true;
}

bool TestRun::startTest(QString *error)
{
    if (isTestRunning()) {
        if (error) *error = QStringLiteral("A test session is already running.");
        return false;
    }
    QString why;
    const RunCommand server = serverCommand(&why);
    if (!server.isValid()) {
        if (error) *error = why;
        return false;
    }
    // Assembled before anything starts, so a client that cannot be built does
    // not leave a server running with nothing to connect to it.
    const RunCommand client = clientCommand(&why);
    if (!client.isValid()) {
        if (error) *error = why;
        return false;
    }

    QDir().mkpath(m_paths.serverProfiles);
    QDir().mkpath(m_paths.clientProfiles);

    m_serverCarry.clear();
    m_clientCarry.clear();
    m_pendingClient = client;
    m_server = makeProcess(QStringLiteral("Server"), &m_serverCarry);
    emitCommand(server);
    m_server->setWorkingDirectory(server.workingDir);
    m_server->start(server.program, server.arguments);

    emit log(QStringLiteral("Waiting %1 seconds for the server to open port %2.")
                 .arg(kClientDelayMs / 1000).arg(m_port));
    m_clientDelay->start(kClientDelayMs);
    emit busyChanged();
    if (error) error->clear();
    return true;
}

QVector<RunStep> TestRun::stop()
{
    QVector<RunStep> steps;

    if (m_clientDelay->isActive()) {
        m_clientDelay->stop();
        m_pendingClient = RunCommand();
        RunStep step;
        step.title = QStringLiteral("Cancel client");
        step.ok = true;
        step.detail = QStringLiteral("The client had not been started yet.");
        steps << step;
    }

    // Only what this object started. Killing every DayZDiag_x64.exe would take
    // down a session the user opened by hand for something else.
    const auto killOne = [&steps](QProcess *proc, const QString &title) {
        if (!proc || proc->state() == QProcess::NotRunning) return;
        const qint64 pid = proc->processId();
        if (pid > 0) {
            RunCommand cmd;
            cmd.program = QStringLiteral("taskkill.exe");
            // /T takes the child processes with it. The diag exe spawns a
            // crash reporter that outlives it otherwise.
            cmd.arguments = { QStringLiteral("/F"), QStringLiteral("/T"),
                              QStringLiteral("/PID"), QString::number(pid) };
            steps << runNow(QStringLiteral("Stop %1").arg(title), cmd);
        }
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(5000);
        }
    };

    // The server goes first. Killing the client first leaves the server holding
    // the port, and the next launch lands on a socket that is already taken.
    killOne(m_server, QStringLiteral("server"));
    killOne(m_client, QStringLiteral("client"));
    killOne(m_build, QStringLiteral("build"));

    if (steps.isEmpty()) {
        RunStep step;
        step.title = QStringLiteral("Stop");
        step.ok = true;
        step.detail = QStringLiteral("Nothing this app started is running.");
        steps << step;
    }
    emit busyChanged();
    return steps;
}

bool TestRun::isBuilding() const
{
    return m_build && m_build->state() != QProcess::NotRunning;
}

bool TestRun::isTestRunning() const
{
    if (m_clientDelay->isActive()) return true;
    if (m_server && m_server->state() != QProcess::NotRunning) return true;
    if (m_client && m_client->state() != QProcess::NotRunning) return true;
    return false;
}
