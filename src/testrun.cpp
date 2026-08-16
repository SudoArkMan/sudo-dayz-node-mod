#include "testrun.h"

#include "config/configtree.h"
#include "moddeps.h"
#include "project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
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

// Where the picks live inside the .sdzn. Nothing else writes under this key, and
// project.cpp carries any root key it does not recognise straight through, so
// the list survives a save and a load without project.cpp knowing about it.
const QLatin1String kExtraKey("testRun");
const QLatin1String kExtraMods("extraMods");

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

// Two spellings of one mod, compared. "@Community-Online-Tools" off disk and
// "Community Online Tools" out of mod.cpp name the same thing, and the only
// difference is punctuation nobody agrees on.
QString packedName(const QString &text)
{
    QString out = text.trimmed();
    if (out.startsWith(QLatin1Char('@'))) out.remove(0, 1);
    out.remove(QLatin1Char(' '));
    out.remove(QLatin1Char('-'));
    return out.toLower();
}

// The folder inside `parent` whose name matches one of `names`. Returns the
// folder as it is actually spelt on disk, because that spelling is what -mod=
// has to carry.
//
// Two passes. The first compares the way Windows compares, and the second drops
// the punctuation, because the Workshop lets an author write the same mod as
// @CommunityOnlineTools, @Community-Online-Tools or @Community Online Tools and
// the installed folder here is the hyphenated one. Exact first, so a machine
// carrying both spellings gets the one that was asked for.
QString matchFolder(const QString &parent, const QStringList &names)
{
    if (!isDir(parent)) return {};
    const QDir dir(parent);
    const QStringList have =
        dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &want : names)
        for (const QString &entry : have)
            if (entry.compare(want, Qt::CaseInsensitive) == 0)
                return clean(dir.filePath(entry));
    for (const QString &want : names) {
        const QString key = packedName(want);
        if (key.isEmpty()) continue;
        for (const QString &entry : have)
            if (packedName(entry) == key) return clean(dir.filePath(entry));
    }
    return {};
}

// A file under resources/, found the way modTemplateAvailable finds the mod
// template: by walking up from the executable, because a build tree and an
// install tree put the folder at different depths.
QString resourceFile(const QString &leaf)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList places = {
        appDir + QStringLiteral("/resources/"),
        appDir + QStringLiteral("/../resources/"),
        appDir + QStringLiteral("/../../resources/"),
        appDir + QStringLiteral("/../../../resources/"),
        appDir + QStringLiteral("/../../../../resources/"),
    };
    for (const QString &place : places)
        if (isFile(place + leaf)) return clean(place + leaf);
    return {};
}

// One entry of resources/known-mods.json, reduced to what the ordering needs.
struct KnownMod {
    QString id;           // the addon name, "JM_COT_Scripts"
    QString displayName;  // "Community Online Tools"
    QStringList addons;
    QStringList requires;
};

QStringList jsonStrings(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty() && !out.contains(text)) out.append(text);
    }
    return out;
}

// resources/known-mods.json, read once. Its own note says the requires lists
// were read out of each framework's config.cpp rather than remembered, which is
// what makes them usable as ordering edges for mods whose config is packed.
//
// Falls back to the presets compiled into moddeps when the file is not beside
// the executable, so an install layout that moved resources still orders CF and
// COT correctly rather than losing every edge.
const QVector<KnownMod> &knownMods()
{
    static const QVector<KnownMod> mods = []() {
        QVector<KnownMod> out;
        const QString path = resourceFile(QStringLiteral("known-mods.json"));
        if (!path.isEmpty()) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonObject root =
                    QJsonDocument::fromJson(file.readAll()).object();
                for (const QJsonValue &v : root.value(QStringLiteral("mods")).toArray()) {
                    const QJsonObject o = v.toObject();
                    KnownMod mod;
                    mod.id = o.value(QStringLiteral("id")).toString().trimmed();
                    if (mod.id.isEmpty()) continue;
                    mod.displayName =
                        o.value(QStringLiteral("displayName")).toString().trimmed();
                    mod.addons = jsonStrings(o.value(QStringLiteral("addons")));
                    if (mod.addons.isEmpty()) mod.addons << mod.id;
                    mod.requires = jsonStrings(o.value(QStringLiteral("requires")));
                    out.append(mod);
                }
            }
        }
        if (!out.isEmpty()) return out;
        for (const ModDependency &dep : knownDependencies()) {
            KnownMod mod;
            mod.id = dep.id;
            mod.displayName = dep.displayName;
            mod.addons = dep.addons.isEmpty() ? QStringList{dep.id} : dep.addons;
            mod.requires = dep.requires;
            out.append(mod);
        }
        return out;
    }();
    return mods;
}

// The name a mod gives itself. mod.cpp sits loose at the root of every Workshop
// mod folder, unlike config.cpp which is packed, so it is the one thing an
// installed mod says about itself without opening a pbo. Empty when the file is
// not there or carries no name, which is normal: 177 of the 254 installed mods
// ship no mod.cpp at all.
QString modCppName(const QString &folder)
{
    // QDir("").filePath resolves against the working directory, so an empty
    // folder would read whatever mod.cpp happens to sit beside the executable.
    if (folder.isEmpty()) return {};
    const QString path = QDir(folder).filePath(QStringLiteral("mod.cpp"));
    if (!isFile(path)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    // Read as latin1 for the same reason readAddonFacts does: a config is not
    // always valid UTF-8 and a decoder that refuses takes the whole file with it.
    ConfigFile cfg = parseConfig(QString::fromLatin1(file.readAll()));
    const ConfigValue *name = findValue(cfg, QStringLiteral("name"));
    return name ? configUnquote(name->scalar).trimmed() : QString();
}

// The folder inside `parent` whose mod.cpp calls the mod one of `names`.
//
// The last way to resolve a dependency, and the one that catches the case the
// folder name cannot: Community Framework ships as @CF, which shares no letters
// with "Community Framework" or "JM_CF_Scripts", and the only thing on disk
// tying the two together is `name = "Community Framework";` in its mod.cpp.
// Getting this wrong is expensive rather than merely untidy: a -mod= entry the
// engine cannot resolve is not an error, it is a mod that silently did not load.
//
// Cached per parent folder for the life of the process. This walks up to 254
// folders and every checklist refresh would otherwise pay for it; installing a
// mod mid-session is rare and the Mod Browser's Rescan is the way back. The
// cache is a plain static because everything that reaches here does so from the
// GUI thread: modChainFor is called by TestRun::refresh and by the test panel,
// and the mod library's own scan thread has its own reader and never enters.
QString matchFolderByModCpp(const QString &parent, const QStringList &names)
{
    if (!isDir(parent)) return {};
    static QHash<QString, QHash<QString, QString>> cache;
    const QString key = clean(parent).toLower();
    if (!cache.contains(key)) {
        QHash<QString, QString> byName;
        const QDir dir(parent);
        for (const QString &entry :
             dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString said = packedName(modCppName(dir.filePath(entry)));
            // First claim wins, so two copies of one mod resolve to the same
            // folder on every refresh rather than alternating.
            if (!said.isEmpty() && !byName.contains(said))
                byName.insert(said, clean(dir.filePath(entry)));
        }
        cache.insert(key, byName);
    }

    const QHash<QString, QString> &byName = cache.value(key);
    for (const QString &want : names) {
        const QString hit = byName.value(packedName(want));
        if (!hit.isEmpty()) return hit;
    }
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

namespace {

// The chain, filtered to one side and with anything missing left out. A folder
// that is not there produces no argument at all: the engine takes an unresolved
// -mod= entry as a mod that loaded, so a path to nothing is worse than a short
// chain, and every launch refuses while missingMods() has anything in it.
QStringList chainSide(const QVector<ModRef> &chain, bool serverOnly, bool asPaths)
{
    QStringList out;
    for (const ModRef &m : chain) {
        if (m.serverOnly != serverOnly || m.missing) continue;
        const QString value = asPaths && !m.path.isEmpty() ? m.path : m.name;
        if (value.isEmpty()) continue;
        out << (asPaths ? QDir::toNativeSeparators(value) : value);
    }
    return out;
}

} // namespace

QStringList TestRunPaths::modNames() const
{
    return chainSide(modChain, false, false);
}

QStringList TestRunPaths::serverModNames() const
{
    return chainSide(modChain, true, false);
}

QString TestRunPaths::modArgument() const
{
    return chainSide(modChain, false, true).join(QLatin1Char(';'));
}

QString TestRunPaths::serverModArgument() const
{
    return chainSide(modChain, true, true).join(QLatin1Char(';'));
}

QStringList TestRunPaths::missingMods() const
{
    QStringList out;
    for (const ModRef &m : modChain)
        if (m.missing && !m.name.isEmpty()) out << m.name;
    return out;
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

    // Nothing is called what this dependency is called, so what each folder
    // calls itself is asked instead. @CF is Community Framework and no amount
    // of comparing folder names finds that out.
    for (const QString &place : places) {
        const QString hit = matchFolderByModCpp(place, names);
        if (hit.isEmpty()) continue;
        ref.name = QFileInfo(hit).fileName();
        ref.path = hit;
        ref.from = QStringLiteral("found in %1, which its mod.cpp calls %2")
                       .arg(QDir::toNativeSeparators(clean(place)),
                            modCppName(hit));
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

// ---------------------------------------------------------------- what it needs

namespace {

// A preset matched on any spelling of the mod's name. The addon ids are tried
// too, because a dependency read off a folder is keyed by its addon rather than
// by anything a human wrote.
ModFacts presetFacts(const QStringList &spellings)
{
    ModFacts facts;
    for (const QString &spelling : spellings) {
        if (spelling.trimmed().isEmpty()) continue;
        const QString want = packedName(spelling);
        for (const KnownMod &known : knownMods()) {
            bool hit = want == packedName(known.displayName)
                       || want == packedName(known.id)
                       || spelling.compare(known.id, Qt::CaseInsensitive) == 0;
            for (const QString &addon : known.addons)
                hit = hit || spelling.compare(addon, Qt::CaseInsensitive) == 0;
            if (!hit) continue;
            facts.addons = known.addons;
            facts.requires = known.requires;
            facts.from = QStringLiteral("known-mods.json, read from its own config");
            return facts;
        }
    }
    return facts;
}

// A mod listing its own addons under requiredAddons is common and says nothing
// about what has to load first, so its own names come out.
QStringList needsOf(const QStringList &requires, const QStringList &ships)
{
    const QSet<QString> own(ships.begin(), ships.end());
    QStringList out;
    for (const QString &need : requires)
        if (!own.contains(need) && !out.contains(need)) out << need;
    return out;
}

} // namespace

ModFacts modFactsFor(const QString &folder, const QString &name)
{
    // The mod itself talking beats any preset, so a loose config.cpp is read
    // first. Only a mod somebody has unpacked, a framework checked out beside
    // the project, or the user's own mod has one.
    const QString config = isDir(folder) ? configPathFor(folder) : QString();
    if (!config.isEmpty()) {
        const AddonFacts read = readAddonFacts(config);
        if (read.found && !read.addons.isEmpty()) {
            ModFacts facts;
            facts.addons = read.addons;
            facts.requires = needsOf(read.requires, read.addons);
            facts.from = QStringLiteral("read from %1")
                             .arg(QDir::toNativeSeparators(config));
            return facts;
        }
    }

    QStringList spellings;
    spellings << modCppName(folder) << name;
    if (!folder.isEmpty()) spellings << QFileInfo(folder).fileName();
    const ModFacts preset = presetFacts(spellings);
    if (!preset.addons.isEmpty()) return preset;

    ModFacts facts;
    facts.from = QStringLiteral("nothing on disk says; its config is packed as "
                                "config.bin, which nothing here reads");
    return facts;
}

// --------------------------------------------------------------- the load order

QStringList orderModChain(QVector<ModRef> &chain)
{
    QStringList notes;
    if (chain.size() < 2) return notes;

    QStringList wasOrder;
    for (const ModRef &m : chain) wasOrder << m.name;

    // The mod being tested is pinned last rather than sorted, because every
    // other entry in the chain is something it is written against and no edge
    // read off a third-party config can be allowed to move it in front of them.
    QVector<ModRef> self;
    QVector<ModRef> rest;
    for (const ModRef &m : chain) {
        if (m.origin == ModOrigin::Self) self.append(m);
        else rest.append(m);
    }

    // Which entry ships which addon. The first claim wins: two entries naming
    // one addon is a conflict the engine settles by load order anyway, and
    // inventing a second edge from it would only move them about.
    QHash<QString, int> provider;
    for (int i = 0; i < rest.size(); ++i)
        for (const QString &addon : rest.at(i).addons) {
            const QString key = addon.trimmed().toLower();
            if (!key.isEmpty() && !provider.contains(key)) provider.insert(key, i);
        }

    QVector<QVector<int>> follows(rest.size());
    QVector<int> waiting(rest.size(), 0);
    for (int i = 0; i < rest.size(); ++i)
        for (const QString &need : rest.at(i).requires) {
            const int p = provider.value(need.trimmed().toLower(), -1);
            // A required addon nothing in the chain ships is vanilla, or a mod
            // the user did not add. Neither is an edge, and neither is an error
            // here: DayZ warns on a missing requiredAddons target rather than
            // refusing to start.
            if (p < 0 || p == i || follows.at(p).contains(i)) continue;
            follows[p].append(i);
            ++waiting[i];
        }

    // Lowest seed index among the entries whose requirements are already placed,
    // which keeps the result stable: where the edges say nothing, the order the
    // user gave is the order that comes out.
    QVector<ModRef> sorted;
    QVector<bool> placed(rest.size(), false);
    for (int done = 0; done < rest.size(); ++done) {
        int pick = -1;
        for (int i = 0; i < rest.size() && pick < 0; ++i)
            if (!placed.at(i) && waiting.at(i) == 0) pick = i;
        if (pick < 0) break;
        placed[pick] = true;
        sorted.append(rest.at(pick));
        for (int next : follows.at(pick)) --waiting[next];
    }

    QStringList stuck;
    for (int i = 0; i < rest.size(); ++i) {
        if (placed.at(i)) continue;
        stuck << rest.at(i).name;
        sorted.append(rest.at(i));
    }
    if (!stuck.isEmpty())
        notes << QStringLiteral("%1 require each other, which no order satisfies. "
                                "They kept the order they were added in, and one "
                                "of them will load before what it needs.")
                     .arg(stuck.join(QStringLiteral(", ")));

    chain = sorted + self;

    QStringList blind;
    for (const ModRef &m : chain)
        if (m.origin != ModOrigin::Self && m.addons.isEmpty() && m.requires.isEmpty())
            blind << m.name;
    if (!blind.isEmpty())
        notes << QStringLiteral("Nothing on disk says what these need, so they "
                                "kept the place they were added at: %1. An "
                                "installed mod carries its config.cpp packed as "
                                "config.bin and nothing here reads that, so no "
                                "order was invented for them. Move one up the "
                                "list if it has to load first.")
                     .arg(blind.join(QStringLiteral(", ")));

    QStringList nowOrder;
    for (const ModRef &m : chain) nowOrder << m.name;
    if (nowOrder != wasOrder)
        notes << QStringLiteral("Load order came from what each mod requires "
                                "rather than from the order they were picked: %1.")
                     .arg(nowOrder.join(QStringLiteral(", ")));
    return notes;
}

// -------------------------------------------------------------- the whole chain

QVector<ModRef> modChainFor(const Project &project, const QString &workDrive,
                            const QString &gamePath, QStringList *notes)
{
    QVector<ModRef> chain;
    QSet<QString> claimed;
    // Two keys, because a mod has two names here and either one alone lets it
    // into the chain twice.
    //
    // The name is compared with the punctuation out, because "@Dabs Framework"
    // and "@DabsFramework" are one mod and loading it twice is a chain the
    // engine will not thank anyone for. That is not enough on its own: a
    // dependency resolves to the folder it is installed as, which for Community
    // Framework is @CF, and a user who adds the same mod by hand writes
    // @CommunityFramework. The two share no letters and only the folder they
    // both resolve to says they are the same mod.
    const auto claim = [&claimed](const QString &name, const QString &folder) {
        const QString byName = packedName(name);
        const QString byPath = folder.isEmpty()
                                   ? QString()
                                   : QDir::cleanPath(folder).toLower();
        if (byName.isEmpty() && byPath.isEmpty()) return false;
        if (claimed.contains(byName)) return false;
        if (!byPath.isEmpty() && claimed.contains(byPath)) return false;
        if (!byName.isEmpty()) claimed.insert(byName);
        if (!byPath.isEmpty()) claimed.insert(byPath);
        return true;
    };

    // The mod under test claims its name first even though it is appended last.
    // Its own folder is under P:\Mods like any other, so it shows up in the
    // picker, and a user who ticks it there would otherwise get it twice, with
    // the copy that is not pinned last deciding the order.
    const QString prefix = project.modRoot.isEmpty()
                               ? QString()
                               : prefixOf(project.modRoot, project.modPrefix);
    const QString selfName =
        prefix.isEmpty() ? QString() : QStringLiteral("@") + prefix;
    const QString selfPath =
        selfName.isEmpty()
            ? QString()
            : clean(QDir(workDrive).filePath(QString(kModsFolder)
                                             + QLatin1Char('/') + selfName));
    if (!selfName.isEmpty()) claim(selfName, selfPath);

    for (const ModDependency &dep : project.dependencies) {
        if (!dep.isValid()) continue;
        ModRef ref = modFolderFor(dep, workDrive, gamePath);
        if (ref.name.isEmpty() || !claim(ref.name, ref.path)) continue;
        ref.origin = ModOrigin::Dependency;
        if (!dep.addons.isEmpty()) {
            // What the project already recorded, which is either a preset or
            // what dependencyFromFolder read off the folder the user pointed at.
            ref.addons = dep.addons;
            ref.requires = needsOf(dep.requires, dep.addons);
            ref.factsFrom = QStringLiteral("declared by this project");
        } else {
            const ModFacts facts = modFactsFor(ref.path, ref.name);
            ref.addons = facts.addons;
            ref.requires = facts.requires;
            ref.factsFrom = facts.from;
            if (ref.addons.isEmpty()) {
                const ModFacts byId = presetFacts({ dep.id, dep.displayName });
                if (!byId.addons.isEmpty()) {
                    ref.addons = byId.addons;
                    ref.requires = byId.requires;
                    ref.factsFrom = byId.from;
                }
            }
        }
        chain.append(ref);
    }

    // What the user added for this run. These are not dependencies and nothing
    // about the project changes by adding one: the mod is loaded so the classes
    // this project reopens exist, and that is all.
    for (const ExtraMod &extra : extraModsOf(project)) {
        if (!extra.isValid()) continue;
        ModRef ref;
        ref.name = extra.name;
        ref.origin = ModOrigin::Extra;
        ref.serverOnly = extra.serverOnly;
        if (isDir(extra.folder)) {
            ref.path = clean(extra.folder);
            ref.from = QStringLiteral("added for this run");
        } else {
            // A Steam library that moved is not a mod that was uninstalled, so
            // the usual places are searched before the entry is called gone,
            // and by the same three rules a declared dependency is resolved by,
            // or a mod added as @CommunityFramework would be reported missing
            // on a machine that has it installed as @CF.
            const QStringList places = {
                QDir(workDrive).filePath(kModsFolder),
                QDir(gamePath).filePath(QStringLiteral("!Workshop")),
                gamePath,
            };
            for (const QString &place : places) {
                QString hit = matchFolder(place, { extra.name });
                if (hit.isEmpty()) hit = matchFolderByModCpp(place, { extra.name });
                if (hit.isEmpty()) continue;
                ref.path = hit;
                ref.from = QStringLiteral("added for this run, found again in %1")
                               .arg(QDir::toNativeSeparators(clean(place)));
                break;
            }
            if (ref.path.isEmpty()) {
                ref.missing = true;
                ref.from = extra.folder.isEmpty()
                               ? QStringLiteral("added for this run, and no folder "
                                                "of that name is installed here")
                               : QStringLiteral("added for this run; %1 is not "
                                                "there any more")
                                     .arg(QDir::toNativeSeparators(extra.folder));
            }
        }
        // Claimed after the folder is known, because the folder is what tells
        // one mod's two spellings apart from two mods.
        if (!claim(ref.name, ref.path)) {
            // Named rather than dropped quietly, because the entry is still
            // ticked in the picker and a list that disagrees with the chain is
            // worse than either.
            if (notes)
                *notes << (ref.path.isEmpty()
                               ? QStringLiteral("%1 is already in the chain, so "
                                                "the copy added for this run was "
                                                "dropped.")
                                     .arg(extra.name)
                               : QStringLiteral("%1 is already in the chain as %2, "
                                                "so the copy added for this run "
                                                "was dropped.")
                                     .arg(extra.name,
                                          QFileInfo(ref.path).fileName()));
            continue;
        }
        const ModFacts facts = modFactsFor(ref.path, ref.name);
        ref.addons = facts.addons;
        ref.requires = facts.requires;
        ref.factsFrom = facts.from;
        chain.append(ref);
    }

    // The mod itself goes last: the engine loads the chain in order and a mod
    // that overrides a class its dependency declares has to come after it.
    if (!selfName.isEmpty()) {
        ModRef self;
        self.name = selfName;
        self.path = clean(QDir(workDrive).filePath(QString(kModsFolder)
                                                   + QLatin1Char('/') + self.name));
        self.from = QStringLiteral("this project");
        self.origin = ModOrigin::Self;
        const ModFacts facts = modFactsFor(
            clean(QDir(project.modRoot).filePath(prefix)), self.name);
        self.addons = facts.addons;
        self.requires = facts.requires;
        self.factsFrom = facts.from;
        chain.append(self);
    }

    const QStringList ordering = orderModChain(chain);
    if (notes) *notes += ordering;
    return chain;
}

// ------------------------------------------------------------- mods added by hand

QVector<ExtraMod> extraModsOf(const Project &project)
{
    QVector<ExtraMod> out;
    const QJsonArray list = project.extra.value(QString(kExtraKey)).toObject()
                                .value(QString(kExtraMods)).toArray();
    QSet<QString> seen;
    for (const QJsonValue &value : list) {
        const QJsonObject o = value.toObject();
        ExtraMod mod;
        mod.name = o.value(QStringLiteral("name")).toString().trimmed();
        // A record with no folder name names nothing the engine could load, and
        // a second record of one mod would load it twice.
        if (mod.name.isEmpty()) continue;
        const QString key = packedName(mod.name);
        if (seen.contains(key)) continue;
        seen.insert(key);
        mod.folder = o.value(QStringLiteral("folder")).toString().trimmed();
        mod.label = o.value(QStringLiteral("label")).toString().trimmed();
        mod.serverOnly = o.value(QStringLiteral("serverOnly")).toBool();
        out.append(mod);
    }
    return out;
}

void setExtraMods(Project &project, const QVector<ExtraMod> &mods)
{
    QJsonArray list;
    for (const ExtraMod &mod : mods) {
        if (!mod.isValid()) continue;
        QJsonObject o;
        o.insert(QStringLiteral("name"), mod.name);
        if (!mod.folder.isEmpty()) o.insert(QStringLiteral("folder"), mod.folder);
        if (!mod.label.isEmpty()) o.insert(QStringLiteral("label"), mod.label);
        if (mod.serverOnly) o.insert(QStringLiteral("serverOnly"), true);
        list.append(o);
    }

    QJsonObject section = project.extra.value(QString(kExtraKey)).toObject();
    if (list.isEmpty()) section.remove(QString(kExtraMods));
    else section.insert(QString(kExtraMods), list);
    // A project that never used the picker keeps the file it had, rather than
    // gaining an empty object every time the panel refreshes.
    if (section.isEmpty()) project.extra.remove(QString(kExtraKey));
    else project.extra.insert(QString(kExtraKey), section);
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

namespace {

// One key=value line replaced and every other byte kept, line endings and a
// missing final newline included. Anchored at the start of the line with only
// blanks in front, so rewriting Mods can never land on ServerMods.
QByteArray withConfigLine(const QByteArray &projectCfg, const QString &key,
                          const QStringList &values)
{
    const QByteArray line = key.toUtf8() + QByteArray("=")
                            + values.join(QLatin1Char(';')).toUtf8();
    const QRegularExpression keyLine(QStringLiteral("^[ \\t]*%1[ \\t]*=").arg(key));

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
        if (!replaced && keyLine.match(text).hasMatch()) {
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

} // namespace

QByteArray withModChain(const QByteArray &projectCfg, const QStringList &modNames)
{
    return withConfigLine(projectCfg, QStringLiteral("Mods"), modNames);
}

QByteArray withServerModChain(const QByteArray &projectCfg,
                              const QStringList &modNames)
{
    // A file with no ServerMods line and nothing to put on one comes back
    // untouched: appending an empty line rewrites somebody's project.cfg for no
    // reason. A file that has the line always gets it rewritten, so taking the
    // last server-only mod off empties the line rather than leaving a stale
    // chain behind for Workbench to load.
    static const QRegularExpression serverKey(
        QStringLiteral("^[ \\t]*ServerMods[ \\t]*="),
        QRegularExpression::MultilineOption);
    if (modNames.isEmpty()
        && !serverKey.match(QString::fromUtf8(projectCfg)).hasMatch())
        return projectCfg;
    return withConfigLine(projectCfg, QStringLiteral("ServerMods"), modNames);
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

// -------------------------------------------------------------- offline limits

QString OfflineLimit::line() const
{
    return QStringLiteral("%1. %2").arg(what, why);
}

QVector<OfflineLimit> offlineLimits(const QVector<ModRef> &modChain)
{
    QVector<OfflineLimit> limits;
    const auto add = [&limits](const QString &what, const QString &why) {
        OfflineLimit limit;
        limit.what = what;
        limit.why = why;
        limits.append(limit);
    };

    // One process, one character, no socket. Nothing to check in the scripts
    // for this one: it is the shape of the command.
    add(QStringLiteral("Another player"),
        QStringLiteral("Offline is one process and one character, so anything "
                       "that needs a second client cannot happen at all."));

    // 3_game/syncevents.c is the clearest of the 467: the handler for
    // RPC_SYNC_EVENT sits behind IsMultiplayer() and IsClient() together, so
    // offline an RPC is written and nothing ever reads it.
    add(QStringLiteral("Anything networked"),
        QStringLiteral("IsMultiplayer() is false offline, and vanilla gates 467 "
                       "checks across 197 files on it, RPC delivery included."));

    // EntityAI::IsServerCheck returns at its first line offline, because
    // IsServer() is true there. The error it exists to raise cannot fire.
    add(QStringLiteral("A variable written on the wrong side"),
        QStringLiteral("EntityAI.IsServerCheck only raises its error for a "
                       "multiplayer client, and offline is not multiplayer."));

    // game.c's own comment on IsDedicatedServer() names #ifdef SERVER as the
    // compile-time form of the same question.
    add(QStringLiteral("Code behind IsDedicatedServer() or #ifdef SERVER"),
        QStringLiteral("Offline is not a dedicated server, so neither branch is "
                       "taken."));

    // ServerConfigGetInt reads the file given to -config, which offline is
    // never given. enableCfgGameplayFile is read through it, so
    // cfggameplay.json goes with it.
    add(QStringLiteral("Anything set in server.cfg"),
        QStringLiteral("Offline passes no -config, so ServerConfigGetInt finds "
                       "nothing and cfggameplay.json is not loaded."));

    // Only worth saying when there is a server-only chain to leave out. The two
    // launches differ here and it is the one difference the user chose, so it
    // gets said rather than discovered.
    for (const ModRef &m : modChain) {
        if (!m.serverOnly) continue;
        add(QStringLiteral("A server-only mod"),
            QStringLiteral("-serverMod= is what Bohemia's own server script "
                           "passes to the server executable and to nothing else, "
                           "so an offline run, which is one process and not a "
                           "server launch, is given none of that chain."));
        break;
    }

    // Only worth saying to somebody loading COT. Spaces and dashes come out
    // because the folder ships as @Community-Online-Tools while the guessed
    // name is @CommunityOnlineTools.
    for (const ModRef &m : modChain) {
        QString packed = m.name;
        packed.remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
        if (!packed.contains(QStringLiteral("CommunityOnlineTools"),
                             Qt::CaseInsensitive))
            continue;
        add(QStringLiteral("A Community Online Tools permission"),
            QStringLiteral("HasPermission returns true whenever the mission is "
                           "offline and the roles are only loaded on a "
                           "multiplayer server, so a gated feature always opens."));
        break;
    }

    return limits;
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
    // Found so the checklist can name it. A DayZ folder with only the retail
    // exe in it is not a partial install, it is the wrong install for this, and
    // saying so beats "no DayZDiag_x64.exe under D:\...".
    p.retailExe = p.gamePath.isEmpty()
                      ? QString()
                      : clean(QDir(p.gamePath).filePath(QStringLiteral("DayZ_x64.exe")));
    if (!isFile(p.retailExe)) p.retailExe.clear();
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

        // The mod's own missions first, because those are the ones the user can
        // edit without touching the DayZ install. The template ships one folder
        // per map it was scaffolded with, named <Prefix>.<Terrain>, and offline
        // has to be pointed at exactly one of them.
        const QDir missions(QDir(p.modRoot).filePath(QStringLiteral("Missions")));
        for (const QString &name :
             missions.entryList({ p.modPrefix + QStringLiteral(".*") },
                                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            p.missions << clean(missions.filePath(name));
        if (!p.missions.isEmpty()) {
            p.missionFrom = QStringLiteral("shipped with this mod");
        } else if (!p.serverPath.isEmpty()) {
            // Nothing of the mod's own, so the vanilla offline missions are
            // better than refusing to run: they load the mod chain the same way
            // and they are already on disk.
            const QDir mp(QDir(p.serverPath).filePath(QStringLiteral("mpmissions")));
            for (const QString &name :
                 mp.entryList({ QStringLiteral("dayzOffline.*") },
                              QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                p.missions << clean(mp.filePath(name));
            if (!p.missions.isEmpty())
                p.missionFrom = QStringLiteral("borrowed from the DayZServer install");
        }

        // The user's pick wins while it is still there. Falling back to the
        // first is what makes a freshly opened project runnable without asking.
        for (const QString &have : p.missions)
            if (have.compare(m_mission, Qt::CaseInsensitive) == 0) p.mission = have;
        if (p.mission.isEmpty()) p.mission = p.missions.value(0);
    }

    p.modChain = modChainFor(project, p.workDrive, p.gamePath, &p.chainNotes);
    // Reconciled here rather than at write time, so the chain the launch uses
    // and the chain written into project.cfg can never be two different things.
    if (isFile(p.projectCfg)) {
        QFile file(p.projectCfg);
        if (file.open(QIODevice::ReadOnly))
            applyExistingSpelling(p.modChain, file.readAll());
    }
    m_paths = p;
}

void TestRun::setMission(const QString &path)
{
    m_mission = clean(path);
    // Taken up straight away rather than at the next refresh, so the command
    // the user reads in the log is the mission they just picked.
    for (const QString &have : m_paths.missions)
        if (have.compare(m_mission, Qt::CaseInsensitive) == 0) m_paths.mission = have;
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

    if (!p.diagExe.isEmpty()) {
        out << ok(QStringLiteral("diag"), QStringLiteral("Diag build"),
                  QStringLiteral("%1 (%2)")
                      .arg(QDir::toNativeSeparators(p.diagExe), p.gameFrom));
    } else if (!p.retailExe.isEmpty()) {
        // The one case worth spelling out. Both modes need -filePatching, and
        // retail DayZ_x64.exe and DayZServer_x64.exe both stop at the loading
        // screen once it is on, so a retail install is not a smaller version of
        // what is needed here, it is the wrong one.
        out << bad(QStringLiteral("diag"), QStringLiteral("Diag build"),
                   QStringLiteral("Only the retail %1 is here. Neither offline "
                                  "nor a dev server can use it: both need "
                                  "-filePatching, and the retail client and the "
                                  "retail server each stop at the loading screen "
                                  "once it is on.")
                       .arg(QDir::toNativeSeparators(p.retailExe)),
                   QStringLiteral("Install the diagnostic build from the DayZ "
                                  "Tools launcher. It lands beside the retail "
                                  "exe as DayZDiag_x64.exe."));
    } else {
        out << bad(QStringLiteral("diag"), QStringLiteral("Diag build"),
                   p.gamePath.isEmpty()
                       ? QStringLiteral("The DayZ install was not found.")
                       : QStringLiteral("No DayZDiag_x64.exe under %1.")
                             .arg(QDir::toNativeSeparators(p.gamePath)),
                   QStringLiteral("Install the diagnostic build from the DayZ "
                                  "Tools launcher. The retail exe cannot load a "
                                  "file-patched mod."));
    }

    // The chain itself, before the file it gets written into. A folder that is
    // not there is the one state that blocks: the engine treats an unresolved
    // -mod= entry as a mod that loaded, so the mod written against it comes up
    // hooking classes that do not exist and nothing on screen says why.
    const QStringList gone = p.missingMods();
    QStringList unsure;
    QStringList blind;
    for (const ModRef &m : p.modChain) {
        if (m.guessed) unsure << m.name;
        if (m.origin != ModOrigin::Self && !m.missing && m.addons.isEmpty()
            && m.requires.isEmpty())
            blind << m.name;
    }
    // The names are not repeated here. The panel prints the whole chain in
    // order above the checklist, and a row that says it again is a row nobody
    // reads. This one is about the state of the chain rather than its content.
    const QString chainLine =
        p.modChain.isEmpty()
            ? QStringLiteral("Nothing to load")
            : p.modChain.size() == 1
                  ? QStringLiteral("One mod")
                  : QStringLiteral("%1 mods, in the order each one requires")
                        .arg(p.modChain.size());
    if (!gone.isEmpty())
        out << bad(QStringLiteral("modchain"), QStringLiteral("Mod chain"),
                   QStringLiteral("%1 was added for this run and is not installed "
                                  "here any more.")
                       .arg(gone.join(QStringLiteral(", "))),
                   QStringLiteral("Install it again, or take it out with Choose "
                                  "mods. Nothing launches while the chain names a "
                                  "folder that is not there."));
    else if (!unsure.isEmpty())
        out << bad(QStringLiteral("modchain"), QStringLiteral("Mod chain"),
                   QStringLiteral("%1. Nothing here confirmed the spelling of %2, "
                                  "so the Workshop's usual one was used.")
                       .arg(chainLine, unsure.join(QStringLiteral(", "))),
                   QStringLiteral("Point the dependency at its folder, or add the "
                                  "installed mod with Choose mods, and the name "
                                  "comes off disk instead."),
                   PrereqState::Warning);
    else if (!blind.isEmpty())
        out << bad(QStringLiteral("modchain"), QStringLiteral("Mod chain"),
                   QStringLiteral("%1. Nothing on disk says what these need, so "
                                  "they kept the place they were added at: %2.")
                       .arg(chainLine, blind.join(QStringLiteral(", "))),
                   QStringLiteral("An installed mod carries its config packed as "
                                  "config.bin. If it has to load before something "
                                  "else, put it higher in Choose mods."),
                   PrereqState::Warning);
    else
        out << ok(QStringLiteral("modchain"), QStringLiteral("Mod chain"), chainLine);

    if (isFile(p.projectCfg))
        out << ok(QStringLiteral("projectcfg"), QStringLiteral("Workbench project"),
                  QStringLiteral("Mods=%1").arg(p.modNames().join(QLatin1Char(';'))));
    else
        out << bad(QStringLiteral("projectcfg"), QStringLiteral("Workbench project"),
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
                  p.missions.size() > 1
                      ? QStringLiteral("%1, one of %2 %3")
                            .arg(QDir::toNativeSeparators(p.mission))
                            .arg(p.missions.size())
                            .arg(p.missionFrom)
                      : QStringLiteral("%1 (%2)")
                            .arg(QDir::toNativeSeparators(p.mission), p.missionFrom));
    else if (m_mode == LaunchMode::Offline)
        // Offline has nothing to fall back on. There is no server.cfg in the
        // command and no server to read one, so a run with no mission is a run
        // that comes up at the main menu and looks like the mod failed.
        out << bad(QStringLiteral("mission"), QStringLiteral("Mission"),
                   QStringLiteral("No %1\\Missions\\%2.<map> and no DayZServer "
                                  "install to borrow one from. Offline has "
                                  "nothing to load.")
                       .arg(QDir::toNativeSeparators(p.modRoot), p.modPrefix),
                   QStringLiteral("Scaffold the mod again with a map selected, "
                                  "or copy a mission folder in and name it "
                                  "%1.ChernarusPlus. Switching to Dev server "
                                  "also runs without one.")
                       .arg(p.modPrefix));
    else
        out << bad(QStringLiteral("mission"), QStringLiteral("Mission"),
                   QStringLiteral("No mission under the mod folder and no "
                                  "DayZServer install to borrow one from."),
                   QStringLiteral("The server will fall back to the template line "
                                  "in server.cfg. Offline cannot: it needs one "
                                  "named %1.<map> under Missions.")
                       .arg(p.modPrefix),
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
    const QStringList serverNames = m_paths.serverModNames();
    // Both lines, in one pass over the file, so Workbench and a launch from
    // here can never be loading two different chains.
    const QByteArray after =
        withServerModChain(withModChain(before, names), serverNames);
    const QString written =
        serverNames.isEmpty()
            ? QStringLiteral("Mods=%1").arg(names.join(QLatin1Char(';')))
            : QStringLiteral("Mods=%1 / ServerMods=%2")
                  .arg(names.join(QLatin1Char(';')),
                       serverNames.join(QLatin1Char(';')));
    if (after == before) {
        step.ok = true;
        step.output = written;
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
    step.output = written;
    QStringList how;
    for (const ModRef &m : m_paths.modChain)
        how << QStringLiteral("%1 (%2)").arg(m.name, m.from);
    // Anything not installed was left off both lines rather than written as a
    // path to nothing, and a line that quietly lost an entry is worth saying.
    const QStringList gone = m_paths.missingMods();
    if (!gone.isEmpty())
        how << QStringLiteral("left out because it is not installed here: %1")
                   .arg(gone.join(QStringLiteral(", ")));
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

// A chain naming a folder that is not there. Refused before anything starts,
// rather than launched with the entry quietly dropped: the mod under test is
// written against that mod, so a session without it comes up hooking classes
// that do not exist, which looks exactly like the mod being broken.
QString TestRun::missingChainReason() const
{
    const QStringList gone = m_paths.missingMods();
    if (gone.isEmpty()) return {};
    QStringList where;
    for (const ModRef &m : m_paths.modChain)
        if (m.missing) where << QStringLiteral("%1 (%2)").arg(m.name, m.from);
    return QStringLiteral("%1 %2 in the mod chain and not installed here. %3. "
                          "Install it again, or take it out with Choose mods.")
        .arg(gone.join(QStringLiteral(", ")),
             gone.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
             where.join(QStringLiteral("; ")));
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
    const QString gone = missingChainReason();
    if (!gone.isEmpty()) return fail(gone);

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
    // The server-only half of the chain, and the only place it appears. The
    // evidence for the parameter is on withServerModChain in the header: it is
    // what Bohemia's own Server_manager.ps1 hands DayZServer_x64.exe, alongside
    // -mod= and never instead of it.
    const QString serverMods = p.serverModArgument();
    if (!serverMods.isEmpty())
        cmd.arguments << QStringLiteral("-serverMod=%1").arg(serverMods);
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
    const QString gone = missingChainReason();
    if (!gone.isEmpty()) {
        if (error) *error = gone;
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

RunCommand TestRun::offlineCommand(QString *error) const
{
    const TestRunPaths &p = m_paths;
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return RunCommand();
    };

    if (p.diagExe.isEmpty())
        return fail(QStringLiteral("DayZDiag_x64.exe was not found."));
    const QString gone = missingChainReason();
    if (!gone.isEmpty()) return fail(gone);
    // Refused here rather than launched without it. The engine takes a missing
    // -mission= as a request for the main menu, so the session would come up
    // looking exactly like a mod that failed to load.
    if (p.mission.isEmpty())
        return fail(QStringLiteral("No mission to run. Offline loads one mission "
                                   "folder, and there is no %1.<map> under "
                                   "%2\\Missions to load. Scaffold the mod again "
                                   "with a map selected, or run on a dev server "
                                   "instead.")
                        .arg(p.modPrefix,
                             QDir::toNativeSeparators(p.modRoot)));
    if (!isDir(p.mission))
        return fail(QStringLiteral("%1 is not there any more.")
                        .arg(QDir::toNativeSeparators(p.mission)));

    RunCommand cmd;
    cmd.program = p.diagExe;
    cmd.workingDir = QFileInfo(p.diagExe).absolutePath();
    cmd.arguments << QStringLiteral("-mission=%1")
                         .arg(QDir::toNativeSeparators(p.mission))
                  << QStringLiteral("-profiles=%1")
                         .arg(QDir::toNativeSeparators(p.clientProfiles));
    const QString mods = p.modArgument();
    if (!mods.isEmpty()) cmd.arguments << QStringLiteral("-mod=%1").arg(mods);
    // Same reason as the pair: without it the engine reads scripts out of the
    // PBO only, and editing a script would mean rebuilding to see it.
    cmd.arguments << QStringLiteral("-filePatching")
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

    // Offline is the whole session in one process, so there is no port to wait
    // on and nothing to sequence. It lands in the client slot because that is
    // what it is: the diag client, running the mission itself.
    if (m_mode == LaunchMode::Offline) {
        const RunCommand offline = offlineCommand(&why);
        if (!offline.isValid()) {
            if (error) *error = why;
            return false;
        }
        QDir().mkpath(m_paths.clientProfiles);
        m_clientCarry.clear();
        m_client = makeProcess(QStringLiteral("Offline"), &m_clientCarry);
        emitCommand(offline);
        m_client->setWorkingDirectory(offline.workingDir);
        m_client->start(offline.program, offline.arguments);
        emit busyChanged();
        if (error) error->clear();
        return true;
    }

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
    killOne(m_client, m_mode == LaunchMode::Offline
                          ? QStringLiteral("offline session")
                          : QStringLiteral("client"));
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
