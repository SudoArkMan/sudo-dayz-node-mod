// Depending on another mod.
//
// Five things have to hold, and each one is a way this feature breaks in the
// user's hands rather than in a build:
//
//   presets      COT's addon names have to be the real ones. A mod that lists
//                the wrong addon does not load, and the error the engine gives
//                back names nothing useful.
//   config.cpp   pointing at a folder has to read the addon names out of its
//                CfgPatches, because a typed addon name is a typo waiting to
//                happen.
//   .sdzn        a dependency has to survive save and load with its path stored
//                relative, so moving a project folder does not strand it.
//   indexing     a mod folder has to turn into classes and methods that can be
//                nodes, with keys that cannot collide with vanilla keys.
//   no folder    a scriptRoot that is not on this machine has to degrade to
//                what the project already knows, not fail.
//
// Every mod folder here is built in a QTemporaryDir, so this needs no mod
// installed and leaves nothing behind:
//   ./tests/depstest ../resources

#include "builtins.h"
#include "catalog.h"
#include "moddeps.h"
#include "project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

static int fails = 0;

static void check(bool ok, const QString &what)
{
    QTextStream o(stdout);
    o << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) fails++;
}

static void section(const QString &title)
{
    QTextStream o(stdout);
    o << Qt::endl << title << Qt::endl;
}

static bool writeFile(const QString &path, const char *text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QByteArray(text));
    return true;
}

// The mod this test indexes. Small on purpose, and every part of it is there to
// catch one thing: two Bumps for overload keys, a static, a private that must
// stay out of the palette, a member with an initialiser (the shape that fooled
// the catalogue's own index builder into listing 542 non-methods), an `out`
// parameter, a class that extends another, and a modded class.
static bool buildTestMod(const QString &root)
{
    return writeFile(root + "/Scripts/config.cpp", R"CFG(class CfgPatches
{
	class TM_Scripts
	{
		requiredAddons[] = { "DZ_Scripts", "JM_CF_Scripts" };
	};
	class TM_GUI
	{
		requiredAddons[] = { "TM_Scripts" };
	};
};

class CfgMods
{
	class TestMod
	{
		name = "Test Mod";
		dir = "TestMod";
	};
};
)CFG")
           && writeFile(root + "/Scripts/4_World/TMHelper.c", R"ENF(class TMHelper
{
	protected int m_Count;
	int m_Sizes[] = {1, 2};

	int Bump(int by)
	{
		m_Count = m_Count + by;
		return m_Count;
	}

	int Bump(int by, int times)
	{
		m_Count = m_Count + by;
		return m_Count;
	}

	static string Describe(string prefix)
	{
		return prefix;
	}

	private void Hidden()
	{
		m_Count = 0;
	}
}
)ENF")
           && writeFile(root + "/Scripts/5_Mission/TMRegistry.c", R"ENF(class TMRegistry extends TMHelper
{
	void Register(string name, out int slot)
	{
		slot = 1;
	}
}
)ENF")
           && writeFile(root + "/Scripts/3_Game/TMModded.c", R"ENF(modded class Man
{
	void TMExtra()
	{
		Print("extra");
	}
}
)ENF");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream o(stdout);
    const QStringList args = app.arguments();
    QString resources;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i).startsWith(QLatin1Char('-'))) continue;
        resources = args.at(i);
        break;
    }
    if (resources.isEmpty()) resources = QStringLiteral("resources");

    Catalog cat;
    Builtins builtins;
    if (!cat.load(QDir(resources).absoluteFilePath(QStringLiteral("catalog.json"))))
        o << "note: no catalogue at " << resources << ", reading is unaffected" << Qt::endl;

    // ------------------------------------------------------------- presets
    section(QStringLiteral("presets"));
    const ModDependency cot = knownDependency(QStringLiteral("JM_COT_Scripts"));
    const ModDependency cf = knownDependency(QStringLiteral("JM_CF_Scripts"));
    check(knownDependencies().size() == 2, QStringLiteral("COT and CF ship as presets"));
    check(cot.isValid() && cot.id == QLatin1String("JM_COT_Scripts"),
          QStringLiteral("COT's addon is JM_COT_Scripts"));
    check(cot.addons == QStringList({QStringLiteral("JM_COT_Scripts"),
                                     QStringLiteral("JM_COT_GUI")}),
          QStringLiteral("COT ships JM_COT_Scripts and JM_COT_GUI"));
    check(cot.requires == QStringList({QStringLiteral("JM_CF_Scripts"),
                                       QStringLiteral("DZ_Data")}),
          QStringLiteral("COT requires JM_CF_Scripts and DZ_Data"));
    check(cot.loadedDefine == QLatin1String("JM_COT_LOADED"),
          QStringLiteral("COT defines JM_COT_LOADED"));
    check(cot.shortName == QLatin1String("COT") && cot.displayName
                                                       == QLatin1String("Community Online Tools"),
          QStringLiteral("COT's badge reads COT"));
    check(cf.isValid() && cf.id == QLatin1String("JM_CF_Scripts")
              && cf.addons == QStringList({QStringLiteral("JM_CF_Scripts")}),
          QStringLiteral("CF's addon is JM_CF_Scripts"));
    // Nothing about CF beyond its addon name was read from CF's own source, and
    // a guessed define is worse than none: a mod would guard on a macro that is
    // never set and the integration would silently never compile.
    check(cf.requires.isEmpty() && cf.loadedDefine.isEmpty(),
          QStringLiteral("CF carries no unverified facts"));
    check(!knownDependency(QStringLiteral("JM_NOT_A_MOD")).isValid(),
          QStringLiteral("an addon with no preset comes back invalid"));
    check(shortNameFor(QStringLiteral("Community Online Tools")) == QLatin1String("COT"),
          QStringLiteral("a display name gives up its capitals for the badge"));
    check(badgeColorFor(QStringLiteral("JM_COT_Scripts")).isValid()
              && badgeColorFor(QStringLiteral("JM_COT_Scripts"))
                     == badgeColorFor(QStringLiteral("JM_COT_Scripts"))
              && badgeColorFor(QStringLiteral("JM_COT_Scripts"))
                     != badgeColorFor(QStringLiteral("JM_CF_Scripts")),
          QStringLiteral("a badge colour is stable per addon and differs between two"));

    // ------------------------------------------------ reading a real config.cpp
    section(QStringLiteral("addon names out of a config.cpp"));
    const QString templateMod =
        QDir(resources).absoluteFilePath(QStringLiteral("mod-template/ModTemplate"));
    const QString templateConfig = configPathFor(templateMod);
    check(!templateConfig.isEmpty(),
          QStringLiteral("the bundled template's config.cpp is found from its folder"));
    QString why;
    const AddonFacts facts = readAddonFacts(templateConfig, &why);
    check(facts.found && facts.addons == QStringList({QStringLiteral("MT_Scripts")}),
          QStringLiteral("its CfgPatches names MT_Scripts"));
    check(facts.requires == QStringList({QStringLiteral("DZ_Scripts")}),
          QStringLiteral("its requiredAddons is DZ_Scripts"));
    check(facts.modClass == QLatin1String("ModTemplate"),
          QStringLiteral("its CfgMods class is ModTemplate"));
    const ModDependency fromTemplate = dependencyFromFolder(templateMod, &why);
    check(fromTemplate.isValid() && fromTemplate.id == QLatin1String("MT_Scripts"),
          QStringLiteral("pointing at the folder gives the addon without typing it"));
    check(fromTemplate.shortName == QLatin1String("MT"),
          QStringLiteral("its badge reads MT"));
    check(!fromTemplate.scriptRoot.isEmpty()
              && QFileInfo(fromTemplate.scriptRoot + QStringLiteral("/Scripts")).isDir(),
          QStringLiteral("its scriptRoot is the folder holding Scripts"));

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        o << "cannot make a temporary folder, so the rest cannot run" << Qt::endl;
        return 1;
    }
    const QString modRoot = tmp.filePath(QStringLiteral("mods/TestMod"));
    if (!buildTestMod(modRoot)) {
        o << "cannot write the test mod, so the rest cannot run" << Qt::endl;
        return 1;
    }

    const ModDependency mine = dependencyFromFolder(modRoot, &why);
    check(mine.isValid() && mine.id == QLatin1String("TM_Scripts"),
          QStringLiteral("the first CfgPatches class is the addon name"));
    check(mine.addons == QStringList({QStringLiteral("TM_Scripts"), QStringLiteral("TM_GUI")}),
          QStringLiteral("every addon under CfgPatches is kept"));
    // TM_GUI lists TM_Scripts, which is the same mod. Carrying that through
    // would show a mod as depending on itself.
    check(mine.requires == QStringList({QStringLiteral("DZ_Scripts"),
                                        QStringLiteral("JM_CF_Scripts")}),
          QStringLiteral("an addon the mod ships itself is not a dependency of it"));
    check(mine.displayName == QLatin1String("Test Mod") && mine.shortName == QLatin1String("TM"),
          QStringLiteral("CfgMods gives the display name"));

    // ------------------------------------------------------------- .sdzn trip
    section(QStringLiteral("a dependency through a .sdzn"));
    const QString projectPath = tmp.filePath(QStringLiteral("proj/thing.sdzn"));
    QDir().mkpath(QFileInfo(projectPath).absolutePath());
    Project saved = newProject();
    ModDependency stored = mine;
    stored.optional = true;
    stored.loadedDefine = QStringLiteral("TM_LOADED");
    saved.dependencies.append(stored);
    saved.dependencies.append(cot);   // one with no folder on this machine
    QString error;
    check(saveProject(saved, projectPath, &error), QStringLiteral("the project saves"));

    QFile written(projectPath);
    QJsonObject root;
    if (written.open(QIODevice::ReadOnly))
        root = QJsonDocument::fromJson(written.readAll()).object();
    written.close();
    const QJsonArray writtenDeps = root.value(QStringLiteral("dependencies")).toArray();
    const QString writtenRoot =
        writtenDeps.isEmpty() ? QString()
                              : writtenDeps.at(0).toObject().value("scriptRoot").toString();
    check(writtenDeps.size() == 2, QStringLiteral("both dependencies are written"));
    check(writtenRoot == QLatin1String("../mods/TestMod"),
          QStringLiteral("the folder is stored relative to the project file, not absolute"));
    check(!writtenDeps.at(1).toObject().contains(QStringLiteral("scriptRoot")),
          QStringLiteral("a dependency with no folder writes no path"));

    Project reloaded;
    check(loadProject(projectPath, reloaded, &error), QStringLiteral("the project loads"));
    check(reloaded.dependencies.size() == 2, QStringLiteral("both come back"));
    if (reloaded.dependencies.size() == 2) {
        const ModDependency &back = reloaded.dependencies.first();
        check(back.id == stored.id && back.addons == stored.addons
                  && back.requires == stored.requires && back.displayName == stored.displayName
                  && back.shortName == stored.shortName
                  && back.loadedDefine == QLatin1String("TM_LOADED") && back.optional,
              QStringLiteral("every field survives the trip"));
        check(QFileInfo(back.scriptRoot) == QFileInfo(modRoot),
              QStringLiteral("the folder resolves back to where it is"));
        check(reloaded.dependency(QStringLiteral("JM_COT_Scripts")) != nullptr
                  && reloaded.dependency(QStringLiteral("nope")) == nullptr,
              QStringLiteral("a project answers which dependency an addon name is"));
    }

    // The point of storing it relative: move the pair together and the link
    // still lands, which is what happens when a project folder is shared.
    QTemporaryDir moved;
    if (moved.isValid()) {
        const QString movedProject = moved.filePath(QStringLiteral("proj/thing.sdzn"));
        QDir().mkpath(QFileInfo(movedProject).absolutePath());
        QDir().mkpath(moved.filePath(QStringLiteral("mods/TestMod")));
        QFile::copy(projectPath, movedProject);
        Project there;
        loadProject(movedProject, there, &error);
        check(!there.dependencies.isEmpty()
                  && QFileInfo(there.dependencies.first().scriptRoot)
                         == QFileInfo(moved.filePath(QStringLiteral("mods/TestMod"))),
              QStringLiteral("moving the project folder does not strand the link"));
    }

    // A .sdzn can arrive hand-edited. Neither of these can ever be looked up
    // again, so neither is kept quietly.
    const QString messy = tmp.filePath(QStringLiteral("proj/messy.sdzn"));
    writeFile(messy, R"JSON({
  "name": "Messy",
  "scripts": [{"id": "s1", "name": "A", "folder": "4_World", "graph": {}}],
  "activeId": "s1",
  "dependencies": [
    {"id": "JM_CF_Scripts"},
    {"id": "JM_CF_Scripts", "displayName": "again"},
    {"displayName": "nameless"},
    "not an object"
  ]
})JSON");
    Project messyProject;
    check(loadProject(messy, messyProject, &error)
              && messyProject.dependencies.size() == 1,
          QStringLiteral("a repeated addon, a nameless one and a non-object are dropped"));
    check(messyProject.dependencies.size() == 1
              && messyProject.dependencies.first().shortName == QLatin1String("JCS")
              && messyProject.dependencies.first().displayName
                     == QLatin1String("JM_CF_Scripts"),
          QStringLiteral("a badge is derived for an entry that carries no name"));

    // ------------------------------------------------------------- indexing
    section(QStringLiteral("indexing a mod folder"));
    ModIndex index;
    const bool indexed = index.add(mine, cat, builtins, &why);
    check(indexed, indexed ? QStringLiteral("the folder indexes")
                           : QStringLiteral("the folder indexes: %1").arg(why));
    check(index.isIndexed(mine.id), QStringLiteral("the dependency is marked indexed"));
    check(index.classCount() == 3,
          QStringLiteral("its three classes are there (%1)").arg(index.classCount()));
    check(index.classInfo(QStringLiteral("TMHelper")).valid
              && index.classInfo(QStringLiteral("TMRegistry")).base
                     == QLatin1String("TMHelper"),
          QStringLiteral("a class keeps what it extends"));
    check(index.classInfo(QStringLiteral("Man")).modded
              && index.classInfo(QStringLiteral("Man")).base.isEmpty(),
          QStringLiteral("a modded class is marked and has no base of its own"));
    check(index.classInfo(QStringLiteral("TMRegistry")).module == QLatin1String("5_Mission"),
          QStringLiteral("the module comes off the path"));
    check(index.modAncestors(QStringLiteral("TMRegistry"))
              == QStringList({QStringLiteral("TMRegistry"), QStringLiteral("TMHelper")}),
          QStringLiteral("the chain inside the mod is walkable"));

    const QVector<SearchHit> bumps = index.search(QStringLiteral("Bump"));
    check(bumps.size() == 2, QStringLiteral("both Bumps are offered (%1)").arg(bumps.size()));
    check(bumps.size() == 2 && bumps.at(0).key != bumps.at(1).key,
          QStringLiteral("two overloads get two keys"));
    check(index.search(QStringLiteral("Hidden")).isEmpty(),
          QStringLiteral("a private method is not something a node could call"));
    check(index.search(QStringLiteral("m_Sizes")).isEmpty()
              && index.search(QStringLiteral("m_Count")).isEmpty(),
          QStringLiteral("a member declaration never reaches the palette"));
    check(!index.search(QStringLiteral("Describe")).isEmpty(),
          QStringLiteral("a static method is offered"));

    // Every name in the index has to be something that can be written in front
    // of a bracket. This is the same guard Catalog::buildSearchIndex needs, and
    // the reason it needs it is that an index builder walking somebody's source
    // reads member declarations as methods.
    bool allCallable = true;
    for (const ModClass &c : index.classes()) {
        SearchOptions opts;
        opts.limit = 200;
        opts.ofClass = c.name;
        for (const SearchHit &hit : index.search(QString(), opts)) {
            const ModMethod m = index.methodInfo(hit.key);
            if (!m.valid) { allCallable = false; break; }
            for (const QChar ch : m.name)
                if (!ch.isLetterOrNumber() && ch != QLatin1Char('_')) allCallable = false;
            if (m.name.isEmpty() || m.name.at(0).isDigit()) allCallable = false;
        }
    }
    check(allCallable, QStringLiteral("every name the palette can reach is a callable one"));

    const QString bumpKey = bumps.isEmpty() ? QString() : bumps.at(0).key;
    check(ModIndex::isDependencyKey(bumpKey)
              && !ModIndex::isDependencyKey(QStringLiteral("m1204"))
              && !ModIndex::isDependencyKey(QStringLiteral("g7"))
              && !ModIndex::isDependencyKey(QStringLiteral("bi.branch")),
          QStringLiteral("a dependency key cannot be mistaken for a vanilla or builtin one"));
    check(ModIndex::dependencyIdOf(bumpKey) == QLatin1String("TM_Scripts"),
          QStringLiteral("a key says which mod it came from without the index"));
    check(ModIndex::dependencyIdOf(QStringLiteral("m1204")).isEmpty()
              && ModIndex::dependencyIdOf(QStringLiteral("dep.")).isEmpty()
              && ModIndex::dependencyIdOf(QStringLiteral("dep..Thing.Do")).isEmpty(),
          QStringLiteral("a key that names no mod says so"));

    const MethodSig reg = index.method(QStringLiteral("dep.TM_Scripts.TMRegistry.Register"));
    check(reg.valid && reg.owner == QLatin1String("TMRegistry")
              && reg.name == QLatin1String("Register"),
          QStringLiteral("a key by name reaches its method"));
    check(reg.params.size() == 2 && reg.params.at(0).type == QLatin1String("string")
              && reg.params.at(0).dir == 0 && reg.params.at(1).type == QLatin1String("int")
              && reg.params.at(1).dir == 1 && reg.params.at(1).name == QLatin1String("slot"),
          QStringLiteral("an out parameter keeps its direction, not the word"));
    check(!index.method(QStringLiteral("dep.TM_Scripts.TMRegistry.Nope")).valid
              && !index.method(QStringLiteral("m1204")).valid,
          QStringLiteral("a key naming nothing reports nothing"));

    const NodeDef def = index.defFor(QStringLiteral("dep.TM_Scripts.TMHelper.Describe"), cat);
    check(def.valid && def.title == QLatin1String("Describe")
              && def.subtitle == QLatin1String("TMHelper"),
          QStringLiteral("a method becomes a node"));
    check(def.pin(QStringLiteral("target"), PinDir::In) == nullptr,
          QStringLiteral("a static method takes no target"));
    check(def.pin(QStringLiteral("exec"), PinDir::In) != nullptr
              && def.pin(QStringLiteral("ret"), PinDir::Out) != nullptr,
          QStringLiteral("it has the exec pins and its return"));
    const NodeDef bumpDef = index.defFor(bumpKey, cat);
    const Pin *target = bumpDef.pin(QStringLiteral("target"), PinDir::In);
    check(target != nullptr && target->type.cls == QLatin1String("TMHelper"),
          QStringLiteral("a member method takes its own class as the target"));

    // ------------------------------------------------------- no folder here
    section(QStringLiteral("a mod this machine does not have"));
    ModIndex missing;
    QString reason;
    ModDependency absent = cot;
    absent.scriptRoot = tmp.filePath(QStringLiteral("mods/NotInstalled"));
    const bool added = missing.add(absent, cat, builtins, &reason);
    check(!added && !reason.isEmpty(),
          QStringLiteral("indexing says there was nothing to read"));
    check(!missing.isIndexed(absent.id), QStringLiteral("it is not marked indexed"));
    check(missing.classCount() == 0 && missing.methodCount() == 0,
          QStringLiteral("nothing is invented for it"));
    const ModDependency *kept = missing.dependency(QStringLiteral("JM_COT_Scripts"));
    check(kept != nullptr && kept->addons == cot.addons && kept->requires == cot.requires
              && kept->loadedDefine == QLatin1String("JM_COT_LOADED"),
          QStringLiteral("the preset facts are still all there"));

    // Re-adding the same addon replaces it rather than doubling it, which is
    // what happens when the user points at the folder after installing the mod.
    ModDependency nowInstalled = mine;
    ModIndex reindexed;
    reindexed.add(mine, cat, builtins, &why);
    reindexed.add(nowInstalled, cat, builtins, &why);
    check(reindexed.dependencies().size() == 1 && reindexed.classCount() == 3,
          QStringLiteral("indexing the same mod twice does not double it"));

    const QStringList notes = index.notes();
    if (!notes.isEmpty()) {
        section(QStringLiteral("what the walk set aside"));
        for (const QString &n : notes) o << "  " << n << Qt::endl;
    }

    o << Qt::endl << (fails == 0 ? QStringLiteral("DEPS OK")
                                 : QStringLiteral("%1 FAILURES").arg(fails)) << Qt::endl;
    return fails == 0 ? 0 : 1;
}
