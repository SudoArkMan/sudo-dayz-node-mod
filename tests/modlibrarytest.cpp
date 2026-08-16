// The mod library, against the mods actually installed on this machine.
//
// Two halves. The first builds a mod of its own in a temporary folder, pbo and
// all, so the read path is checked against bytes this file wrote and the read
// only promise is checked by comparing the folder before and after. The second
// walks the real corpus and prints the numbers that say whether the feature is
// worth having:
//
//   mods found        how many the scan sees, and how many ship script
//   scripts extracted .c files pulled out of their pbos
//   classes imported  classes the importer got out of those files
//   modelled          methods that became nodes, against methods that stayed text
//
// The last one is the honest measure. A mod whose methods mostly stay text
// opens as a list of text boxes, and that is worth knowing before you go
// looking for a graph that is not there.
//
// Opening every mod would take far longer than a test should, so the corpus run
// samples: it spreads its picks evenly over the mods that ship script and stops
// at a wall clock budget. Both numbers are printed, so a sampled run never
// reads as a complete one.
//
//   ./tests/modlibrarytest ../resources [sample size] [--shot browser.png]
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "document.h"
#include "graph.h"
#include "modlibrary.h"
#include "panels/modbrowser.h"
#include "project.h"
#include "theme.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>

static int failures = 0;
static QTextStream *out = nullptr;

static void line(const QString &text)
{
    *out << text << Qt::endl;
    out->flush();
}

static void check(bool ok, const QString &what)
{
    line((ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) + what);
    if (!ok) failures++;
}

// ------------------------------------------------------------- a pbo to read

static void putU32(QByteArray &out, quint32 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 24) & 0xff));
}

static void putAsciiz(QByteArray &out, const QString &text)
{
    out.append(text.toLatin1());
    out.append('\0');
}

// The layout the reader has to walk: a product entry carrying the prefix, an
// entry table, a terminating entry, the data back to back, then the trailer.
static bool writePbo(const QString &path, const QString &prefix,
                     const QVector<QPair<QString, QByteArray>> &files)
{
    QByteArray blob;

    putAsciiz(blob, QString());
    putU32(blob, 0x56657273); // "Vers"
    for (int i = 0; i < 4; ++i) putU32(blob, 0);
    putAsciiz(blob, QStringLiteral("prefix"));
    putAsciiz(blob, prefix);
    putAsciiz(blob, QStringLiteral("Mikero"));
    putAsciiz(blob, QStringLiteral("DePbo.dll.9.87"));
    putAsciiz(blob, QString());

    for (const auto &file : files) {
        putAsciiz(blob, file.first);
        putU32(blob, 0);                          // stored, not packed
        putU32(blob, quint32(file.second.size()));
        putU32(blob, 0);
        putU32(blob, 0);
        putU32(blob, quint32(file.second.size()));
    }
    putAsciiz(blob, QString());
    for (int i = 0; i < 5; ++i) putU32(blob, 0);

    for (const auto &file : files) blob.append(file.second);

    blob.append('\0');
    blob.append(QCryptographicHash::hash(blob.left(blob.size() - 1),
                                         QCryptographicHash::Sha1));

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return false;
    return out.write(blob) == blob.size();
}

// (path, size, modified) for everything under a folder, so "nothing was
// written here" can be proven rather than asserted.
static QStringList folderPrint(const QString &folder)
{
    QStringList rows;
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        rows.append(QStringLiteral("%1|%2|%3")
                        .arg(fi.absoluteFilePath())
                        .arg(fi.size())
                        .arg(fi.lastModified().toMSecsSinceEpoch()));
    }
    rows.sort();
    return rows;
}

static const char *kPlayerScript = R"(modded class PlayerBase
{
	int m_Hits;

	override void EEKilled(Class killer)
	{
		super.EEKilled(killer);
		m_Hits = m_Hits + 1;
		Print("down");
	}

	void CountHit()
	{
		m_Hits = m_Hits + 1;
	}
}
)";

static const char *kToolScript = R"(class SudoTestTool
{
	void Fire()
	{
		Print("fire");
	}
}
)";

// ------------------------------------------------------------ the fixed part

// A whole mod folder, pbo and all, under `folder`. Used by both halves below,
// so the read path and the export rule are proven against the same bytes.
static bool buildFixtureMod(const QString &folder)
{
    QDir().mkpath(folder + QStringLiteral("/Addons"));
    QDir().mkpath(folder + QStringLiteral("/Keys"));

    {
        QFile mod(folder + QStringLiteral("/mod.cpp"));
        if (!mod.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        // The missing semicolon is deliberate: a real mod.cpp in the corpus has
        // one, and the scan has to read the keys after it anyway.
        mod.write("name = \"Sudo Test Mod\";\n"
                  "picture = \"SudoTest\\GUI\\logo.paa\"\n"
                  "author = \"Dillan\";\n"
                  "tooltip = \"A mod written by the test\";  // trailing comment\n");
    }
    {
        QFile meta(folder + QStringLiteral("/meta.cpp"));
        if (!meta.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        meta.write("protocol = 1;\npublishedid = 1234567890;\nname = \"Workshop Title\";\n");
    }

    const QString pbo = folder + QStringLiteral("/Addons/SudoTest_Scripts.pbo");
    QVector<QPair<QString, QByteArray>> files;
    files.append({QStringLiteral("Scripts\\4_World\\player.c"), QByteArray(kPlayerScript)});
    files.append({QStringLiteral("Scripts\\4_World\\tool.c"), QByteArray(kToolScript)});
    files.append({QStringLiteral("config.cpp"), QByteArray("class CfgPatches {};\n")});
    if (!writePbo(pbo, QStringLiteral("SudoTest_Scripts"), files)) return false;
    {
        QFile sign(pbo + QStringLiteral(".SudoKey.bisign"));
        if (!sign.open(QIODevice::WriteOnly)) return false;
        sign.write("not a real signature");
    }
    return true;
}

static void testSyntheticMod(const Catalog &cat, const Builtins &builtins)
{
    line(QString());
    line(QStringLiteral("A mod of our own"));

    QTemporaryDir temp;
    if (!temp.isValid()) {
        check(false, QStringLiteral("a temporary folder for the fixture"));
        return;
    }
    const QString folder = QDir(temp.path()).filePath(QStringLiteral("@SudoTestMod"));
    check(buildFixtureMod(folder), QStringLiteral("the fixture pbo is written"));

    const QStringList before = folderPrint(folder);

    const ModEntry entry = ModLibrary::readMod(folder);
    check(entry.isValid(), QStringLiteral("the folder reads as a mod"));
    check(entry.name == QLatin1String("Sudo Test Mod"),
          QStringLiteral("mod.cpp names it, not meta.cpp (got \"%1\")").arg(entry.name));
    check(entry.author == QLatin1String("Dillan"),
          QStringLiteral("the author survives the line with no semicolon on it"));
    check(entry.picture == QLatin1String("SudoTest\\GUI\\logo.paa"),
          QStringLiteral("a picture path keeps its backslashes (got \"%1\")").arg(entry.picture));
    check(entry.overview == QLatin1String("A mod written by the test"),
          QStringLiteral("a trailing comment is not part of the value"));
    check(entry.publishedId == QLatin1String("1234567890"),
          QStringLiteral("meta.cpp gives up the workshop id"));
    check(entry.pbos.size() == 1, QStringLiteral("one pbo is found"));
    if (!entry.pbos.isEmpty()) {
        const ModPbo &only = entry.pbos.first();
        check(only.readable, QStringLiteral("its header parses (%1)").arg(only.error));
        check(only.prefix == QLatin1String("SudoTest_Scripts"),
              QStringLiteral("the prefix comes off the product entry (got \"%1\")")
                  .arg(only.prefix));
        check(only.entryCount == 3, QStringLiteral("all three entries are counted"));
        check(only.scriptCount == 2, QStringLiteral("only the two .c files count as script"));
    }

    Project project;
    const ModOpenResult opened = openMod(entry, cat, builtins, project);
    check(opened.ok, QStringLiteral("the mod opens (%1)").arg(opened.error));
    check(opened.filesExtracted == 2, QStringLiteral("two files are extracted"));
    check(opened.classes.size() == 2,
          QStringLiteral("two classes come out (got %1)").arg(opened.classes.size()));

    bool foundPlayer = false;
    for (const ModClassView &view : opened.classes) {
        if (view.className != QLatin1String("PlayerBase")) continue;
        foundPlayer = true;
        check(view.modded, QStringLiteral("PlayerBase is read as a modded class"));
        check(graphIsReadOnly(view.graph),
              QStringLiteral("its graph is marked read only"));
        check(graphOrigin(view.graph).contains(QLatin1String("player.c")),
              QStringLiteral("the graph says where it came from (\"%1\")")
                  .arg(graphOrigin(view.graph)));
        check(view.methodsAsNodes + view.methodsAsText + view.methodsEmpty >= 2,
              QStringLiteral("both of its methods are accounted for"));
    }
    check(foundPlayer, QStringLiteral("PlayerBase is one of them"));

    for (const ModScriptFile &file : opened.files) {
        check(ModLibrary::isInsideScriptCache(file.path),
              QStringLiteral("%1 was extracted into the app's cache")
                  .arg(QFileInfo(file.path).fileName()));
        check(!QDir::cleanPath(file.path).startsWith(QDir::cleanPath(folder),
                                                     Qt::CaseInsensitive),
              QStringLiteral("%1 was not written inside the mod folder")
                  .arg(QFileInfo(file.path).fileName()));
    }

    const QStringList after = folderPrint(folder);
    check(before == after,
          QStringLiteral("opening the mod left every file in its folder untouched"));

    // A second open has to answer the same, because the cache is keyed by the
    // mod's modified time and reuses what is already extracted.
    const ModOpenResult again = openMod(entry, cat, builtins, project);
    check(again.classes.size() == opened.classes.size()
              && again.methodsAsNodes == opened.methodsAsNodes,
          QStringLiteral("opening it twice gives the same answer"));
    check(folderPrint(folder) == before,
          QStringLiteral("and still leaves the mod folder alone"));
}

// ---------------------------------------------- a browsed graph in a project
//
// The hazard the browser creates, and the one thing it must not be allowed to
// do. Opening somebody else's class puts their code in your project, and the
// export writes the project into your mod folder. So this builds a project
// holding one class of the user's own and one read out of another mod, exports
// it the way the window does, and proves the second is not on disk anywhere,
// that the mod it came from is untouched, and that a save and a load leave it
// read only rather than handing it back as work of the user's own.
static void testBrowsedGraphNeverExported(const Catalog &cat, const Builtins &builtins)
{
    line(QString());
    line(QStringLiteral("A browsed class inside your own project"));

    QTemporaryDir temp;
    if (!temp.isValid()) {
        check(false, QStringLiteral("a temporary folder for the fixture"));
        return;
    }
    const QString modFolder = QDir(temp.path()).filePath(QStringLiteral("@SudoTestMod"));
    if (!buildFixtureMod(modFolder)) {
        check(false, QStringLiteral("the fixture mod is written"));
        return;
    }

    Project blank;
    const ModOpenResult opened =
        openMod(ModLibrary::readMod(modFolder), cat, builtins, blank);
    Graph browsed;
    for (const ModClassView &view : opened.classes)
        if (view.className == QLatin1String("PlayerBase")) browsed = view.graph;
    check(graphIsReadOnly(browsed),
          QStringLiteral("the browser hands over a graph marked read only"));

    // The user's project: one class of their own, and one out of the mod above,
    // appended the way the window appends what the browser emits.
    Project project;
    project.name = QStringLiteral("MyMod");

    ScriptEntry mine;
    mine.id = QStringLiteral("s1");
    mine.name = QStringLiteral("MyItem");
    mine.folder = QStringLiteral("4_World");
    mine.graph.className = QStringLiteral("MyItem");
    mine.graph.baseClass = QStringLiteral("ItemBase");
    project.scripts.append(mine);

    ScriptEntry theirs;
    theirs.id = QStringLiteral("s2");
    theirs.name = QStringLiteral("PlayerBase");
    theirs.folder = QStringLiteral("4_World");
    theirs.graph = browsed;
    // Pointed straight back at the file inside the mod, which the window never
    // does. A .sdzn is a file anyone can hand you, so the rule has to hold on
    // the graph's own mark rather than on the entry having no path in it.
    theirs.sourcePath =
        QDir(modFolder).filePath(QStringLiteral("Scripts/4_World/player.c"));
    project.scripts.append(theirs);

    check(scriptIsWritable(project.scripts.at(0)),
          QStringLiteral("a class of your own may be written"));
    check(!scriptIsWritable(project.scripts.at(1)),
          QStringLiteral("a class read out of another mod may not"));
    check(scriptsNeedingFolder(project) == 1,
          QStringLiteral("one script needs a folder, not two"));

    // The export, run the way the window runs it: the plan decides, the loop
    // only writes.
    const QString exportRoot = QDir(temp.path()).filePath(QStringLiteral("export"));
    QVector<const ScriptEntry *> held;
    const QVector<ExportTarget> plan = exportPlan(project, exportRoot, &held);
    check(plan.size() == 1,
          QStringLiteral("the export plans one file, not two (planned %1)").arg(plan.size()));
    check(held.size() == 1 && held.first()->name == QLatin1String("PlayerBase"),
          QStringLiteral("and names the one it is holding back"));

    const QStringList modBefore = folderPrint(modFolder);

    int written = 0;
    for (const ExportTarget &target : plan) {
        QDir().mkpath(QFileInfo(target.path).absolutePath());
        const GenResult gen =
            generateEnforce(target.script->graph, cat, builtins, project);
        QFile out(target.path);
        if (!out.open(QIODevice::WriteOnly)) continue;
        out.write(gen.code.toUtf8());
        out.close();
        written++;
    }
    check(written == 1, QStringLiteral("one file is written"));
    check(QFileInfo::exists(QDir(exportRoot).filePath(QStringLiteral("4_World/MyItem.c"))),
          QStringLiteral("your own class lands in the mod folder"));
    check(!QFileInfo::exists(
              QDir(exportRoot).filePath(QStringLiteral("4_World/PlayerBase.c"))),
          QStringLiteral("theirs does not"));
    check(folderPrint(modFolder) == modBefore,
          QStringLiteral("and the mod it was read out of is byte for byte as it was"));

    // Not just under the name it would have used: nothing written anywhere in
    // the export carries a line only their class has.
    int theirCodeOnDisk = 0;
    QDirIterator walk(exportRoot, QDir::Files | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        walk.next();
        QFile f(walk.filePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (QString::fromUtf8(f.readAll()).contains(QLatin1String("EEKilled")))
            theirCodeOnDisk++;
    }
    check(theirCodeOnDisk == 0,
          QStringLiteral("no file in the export holds a line of their code (%1 did)")
              .arg(theirCodeOnDisk));

    // Saving is allowed and coming back editable is not. The mark rides in
    // Graph::extra, which the .sdzn reader and writer carry through untouched.
    const QString sdzn = QDir(temp.path()).filePath(QStringLiteral("MyMod.sdzn"));
    QString error;
    check(saveProject(project, sdzn, &error),
          QStringLiteral("the project saves with the browsed class in it (%1)").arg(error));

    Project reloaded;
    check(loadProject(sdzn, reloaded, &error),
          QStringLiteral("and loads back (%1)").arg(error));
    const ScriptEntry *back = nullptr;
    for (const ScriptEntry &s : reloaded.scripts)
        if (s.name == QLatin1String("PlayerBase")) back = &s;
    check(back != nullptr, QStringLiteral("the browsed class survives the round trip"));
    if (back) {
        check(graphIsReadOnly(back->graph),
              QStringLiteral("and comes back read only"));
        check(graphOrigin(back->graph) == graphOrigin(browsed),
              QStringLiteral("still saying where it came from (\"%1\")")
                  .arg(graphOrigin(back->graph)));
    }
    QVector<const ScriptEntry *> heldAgain;
    check(exportPlan(reloaded, exportRoot, &heldAgain).size() == 1
              && heldAgain.size() == 1,
          QStringLiteral("and is still left out of an export after the round trip"));

    // Every other door into a graph, asked the same question. Opening a mod is
    // no longer only "pick a row in the scan": a loose .pbo, a mod folder
    // nobody scanned and a tree somebody has already unpacked are three more
    // ways in, and a door that skipped markGraphReadOnly would put another
    // author's class in the user's export without anything here noticing.
    const QString unpacked = QDir(temp.path()).filePath(QStringLiteral("UnpackedMod"));
    QDir().mkpath(unpacked + QStringLiteral("/Scripts/4_World"));
    {
        QFile f(unpacked + QStringLiteral("/Scripts/4_World/player.c"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(kPlayerScript);
    }

    struct Door {
        QString what;
        QString path;
    };
    const QVector<Door> doors{
        {QStringLiteral("a loose pbo"),
         QDir(modFolder).filePath(QStringLiteral("Addons/SudoTest_Scripts.pbo"))},
        {QStringLiteral("a mod folder pointed at by hand"), modFolder},
        {QStringLiteral("an unpacked script tree"), unpacked},
    };

    for (const Door &door : doors) {
        QString why;
        const ModEntry entry = readModPath(door.path, &why);
        check(entry.isValid(), QStringLiteral("%1 reads as a mod (%2)").arg(door.what, why));
        if (!entry.isValid()) continue;

        Project blankAgain;
        const ModOpenResult through = openMod(entry, cat, builtins, blankAgain);
        check(through.ok && !through.classes.isEmpty(),
              QStringLiteral("%1 opens and gives up a class (%2)")
                  .arg(door.what, through.error));

        // Appended the way the window appends what the browser emits, and with
        // a sourcePath on it, which is the shape that would slip past a rule
        // written on "did this come from a file" instead of on the mark.
        Project host;
        for (const ModClassView &view : through.classes) {
            ScriptEntry script;
            script.id = QStringLiteral("s%1").arg(host.scripts.size() + 1);
            script.name = view.className;
            script.folder = QStringLiteral("4_World");
            script.graph = view.graph;
            script.sourcePath =
                QDir(modFolder).filePath(QStringLiteral("Scripts/4_World/player.c"));
            host.scripts.append(script);
        }

        int writable = 0;
        for (const ScriptEntry &script : host.scripts)
            if (scriptIsWritable(script)) writable++;
        check(writable == 0,
              QStringLiteral("nothing %1 produced may be written (%2 could)")
                  .arg(door.what).arg(writable));

        QVector<const ScriptEntry *> back;
        const QVector<ExportTarget> nothing =
            exportPlan(host, exportRoot, &back);
        check(nothing.isEmpty() && back.size() == host.scripts.size(),
              QStringLiteral("an export through %1 plans no file and holds all %2 back")
                  .arg(door.what).arg(host.scripts.size()));

        // And nothing of theirs is on disk under any name, which is the check
        // the plan is only a proxy for.
        int leaked = 0;
        QDirIterator sweep(exportRoot, QDir::Files | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories);
        while (sweep.hasNext()) {
            sweep.next();
            QFile f(sweep.filePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            if (QString::fromUtf8(f.readAll()).contains(QLatin1String("EEKilled")))
                leaked++;
        }
        check(leaked == 0,
              QStringLiteral("and no file in the export holds their code after %1 (%2 did)")
                  .arg(door.what).arg(leaked));
    }

    check(folderPrint(modFolder) == modBefore,
          QStringLiteral("the mod is still byte for byte as it was after all of them"));
}

// ------------------------------------------------- opening a path from disk
//
// Three new ways into a graph: a loose .pbo, a mod folder nobody scanned, and a
// tree somebody has already unpacked. Every one of them has to arrive marked
// read only, because the mark is the whole of the rule: `scriptIsWritable` asks
// it, the export plan asks that, and a graph that reached the project without
// it would be written into the user's mod as if it were theirs.
static void testOpenPathsAreReadOnly(const Catalog &cat, const Builtins &builtins)
{
    line(QString());
    line(QStringLiteral("Opening a path from disk"));

    QTemporaryDir temp;
    if (!temp.isValid()) {
        check(false, QStringLiteral("a temporary folder for the fixture"));
        return;
    }
    const QString modFolder = QDir(temp.path()).filePath(QStringLiteral("@SudoTestMod"));
    if (!buildFixtureMod(modFolder)) {
        check(false, QStringLiteral("the fixture mod is written"));
        return;
    }
    const QString pboPath =
        QDir(modFolder).filePath(QStringLiteral("Addons/SudoTest_Scripts.pbo"));

    // A folder holding mods reads as a root rather than as one mod, or pointing
    // at !Workshop would open 266 mods glued into one.
    check(classifyModPath(temp.path()) == ModPathKind::ModRoot,
          QStringLiteral("a folder of mods is a folder to scan"));
    check(classifyModPath(modFolder) == ModPathKind::ModFolder,
          QStringLiteral("the mod inside it is a mod"));
    check(classifyModPath(pboPath) == ModPathKind::Pbo,
          QStringLiteral("and its archive is a pbo"));
    check(classifyModPath(QDir(temp.path()).filePath(QStringLiteral("nowhere")))
              == ModPathKind::None,
          QStringLiteral("a path that is not there is nothing"));

    // Read into locals first: check() takes both the answer and the line about
    // it, and the order its two arguments are evaluated in is the compiler's to
    // pick, so a message built from `error` in the call would print the value
    // from before the call that sets it.
    QString error;
    const bool rootRefused = !readModPath(temp.path(), &error).isValid();
    check(rootRefused && !error.isEmpty(),
          QStringLiteral("a root refuses to open as one mod, with a reason (\"%1\")")
              .arg(error));

    // A pbo on a stick that has been unplugged, or a row left over from last
    // launch. "It is not a mod" sends somebody looking inside a file that is not
    // there at all, so the two answers have to be different ones.
    QString missing;
    const bool goneRefused =
        !readModPath(QDir(temp.path()).filePath(QStringLiteral("gone.pbo")), &missing)
             .isValid();
    check(goneRefused && missing.contains(QLatin1String("not there")),
          QStringLiteral("a path that is gone says so rather than that it is not a mod "
                         "(\"%1\")")
              .arg(missing));

    // An unpacked tree: the same two scripts, on disk, with no archive at all.
    const QString unpacked = QDir(temp.path()).filePath(QStringLiteral("UnpackedMod"));
    QDir().mkpath(unpacked + QStringLiteral("/Scripts/4_World"));
    {
        QFile f(unpacked + QStringLiteral("/Scripts/4_World/player.c"));
        check(f.open(QIODevice::WriteOnly | QIODevice::Text),
              QStringLiteral("an unpacked script is written"));
        f.write(kPlayerScript);
    }
    check(classifyModPath(unpacked) == ModPathKind::ScriptFolder,
          QStringLiteral("a folder of .c files is an unpacked mod"));

    struct Way {
        QString what;
        QString path;
    };
    const QVector<Way> ways{
        {QStringLiteral("a loose pbo"), pboPath},
        {QStringLiteral("a mod folder"), modFolder},
        {QStringLiteral("an unpacked tree"), unpacked},
    };

    const QStringList modBefore = folderPrint(modFolder);
    const QStringList unpackedBefore = folderPrint(unpacked);

    for (const Way &way : ways) {
        QString why;
        const ModEntry entry = readModPath(way.path, &why);
        check(entry.isValid(), QStringLiteral("%1 reads as a mod (%2)").arg(way.what, why));
        if (!entry.isValid()) continue;
        check(entry.hasScripts(),
              QStringLiteral("%1 is seen to ship script").arg(way.what));

        Project project;
        const ModOpenResult opened = openMod(entry, cat, builtins, project);
        check(opened.ok, QStringLiteral("%1 opens (%2)").arg(way.what, opened.error));
        check(!opened.classes.isEmpty(),
              QStringLiteral("%1 gives up at least one class").arg(way.what));

        int marked = 0, unmarked = 0;
        for (const ModClassView &view : opened.classes) {
            if (graphIsReadOnly(view.graph)) marked++;
            else unmarked++;
        }
        check(unmarked == 0,
              QStringLiteral("every graph out of %1 is marked read only (%2 were not)")
                  .arg(way.what).arg(unmarked));
        check(marked > 0,
              QStringLiteral("and %1 had one to mark").arg(way.what));
        for (const ModClassView &view : opened.classes)
            check(!graphOrigin(view.graph).isEmpty(),
                  QStringLiteral("%1 says where %2 came from (\"%3\")")
                      .arg(way.what, view.className, graphOrigin(view.graph)));
    }

    check(folderPrint(modFolder) == modBefore,
          QStringLiteral("the mod folder is byte for byte as it was"));
    check(folderPrint(unpacked) == unpackedBefore,
          QStringLiteral("and so is the unpacked tree, which is read where it lies"));

    // The library has to keep an opened path across a rescan, or the row the
    // user is looking at vanishes under them the moment the scan comes back.
    ModLibrary library;
    library.setRoots({});
    const ModEntry loose = readModPath(pboPath);
    const QString key = library.addOpened(loose);
    check(!key.isEmpty(), QStringLiteral("a loose pbo joins the library"));
    check(library.mod(key) != nullptr, QStringLiteral("and is found by its key"));
    check(library.mods().size() == 1, QStringLiteral("and shows up as a row"));
    library.addOpened(loose);
    check(library.mods().size() == 1,
          QStringLiteral("adding it twice is still one row (%1)").arg(library.mods().size()));
    check(library.removeOpened(key) && library.mods().isEmpty(),
          QStringLiteral("and it can be dropped again"));
}

static void testCacheRoundTrip()
{
    line(QString());
    line(QStringLiteral("The scan cache"));

    ModLibrary library;
    const QString path = library.cacheFile();
    check(!path.isEmpty(), QStringLiteral("there is a writable place for it"));
    check(!ModLibrary::scriptCacheRoot().isEmpty(),
          QStringLiteral("and a place for extracted scripts"));

    QString error;
    const bool saved = library.saveCache(&error);
    check(saved, QStringLiteral("an empty library saves (%1)").arg(error));

    ModLibrary reloaded;
    const bool loaded = reloaded.loadCache(&error);
    check(loaded, QStringLiteral("and loads back (%1)").arg(error));
    check(reloaded.mods().size() == library.mods().size(),
          QStringLiteral("with the same number of mods in it"));
}

// ------------------------------------------------------------- the real thing

struct Worst {
    QString name;
    int percent = 0;
    int methods = 0;
};

static void testInstalledCorpus(const Catalog &cat, const Builtins &builtins, int sampleTarget)
{
    line(QString());
    line(QStringLiteral("Installed mods"));

    ModLibrary library;
    const QStringList roots = library.roots();
    line(QStringLiteral("  roots            %1")
             .arg(roots.isEmpty() ? QStringLiteral("none on this machine") : roots.join(QStringLiteral(", "))));
    if (roots.isEmpty()) {
        line(QStringLiteral("  nothing installed to walk, so the corpus half is skipped"));
        return;
    }

    QElapsedTimer clock;
    clock.start();
    const QVector<ModEntry> mods = ModLibrary::scanRoots(roots);
    const qint64 scanMs = clock.elapsed();

    int withScripts = 0, pbos = 0, pbosRefused = 0, scriptsInside = 0;
    for (const ModEntry &mod : mods) {
        if (mod.hasScripts()) withScripts++;
        for (const ModPbo &pbo : mod.pbos) {
            pbos++;
            if (!pbo.readable) pbosRefused++;
            scriptsInside += pbo.scriptCount;
        }
    }

    // The second pass is the one that matters day to day: the cache is what
    // stops 254 mods being re-read every time the dock is opened.
    QHash<QString, ModEntry> known;
    for (const ModEntry &mod : mods) known.insert(mod.folder.toLower(), mod);
    clock.restart();
    const QVector<ModEntry> again = ModLibrary::scanRoots(roots, known);
    const qint64 rescanMs = clock.elapsed();

    line(QStringLiteral("  mods found       %1").arg(mods.size()));
    line(QStringLiteral("  ship script      %1").arg(withScripts));
    line(QStringLiteral("  pbos read        %1 of %2").arg(pbos - pbosRefused).arg(pbos));
    line(QStringLiteral("  scripts inside   %1").arg(scriptsInside));
    line(QStringLiteral("  cold scan        %1 ms").arg(scanMs));
    line(QStringLiteral("  cached rescan    %1 ms").arg(rescanMs));

    check(again.size() == mods.size(),
          QStringLiteral("a cached rescan finds the same mods (%1 against %2)")
              .arg(again.size()).arg(mods.size()));
    check(rescanMs * 2 < scanMs || scanMs < 500,
          QStringLiteral("and is quicker than reading every header again"));

    // The corpus holds archives run through an obfuscator, which pad the entry
    // table with hundreds of thousands of junk names. Naming the biggest is how
    // a script count of half a million stops looking like a bug in the scan.
    QVector<ModEntry> byScripts = mods;
    std::sort(byScripts.begin(), byScripts.end(),
              [](const ModEntry &a, const ModEntry &b) { return a.scriptCount() > b.scriptCount(); });
    line(QStringLiteral("  most scripts"));
    for (int i = 0; i < qMin(5, byScripts.size()); ++i)
        line(QStringLiteral("    %1  %2")
                 .arg(byScripts.at(i).scriptCount(), 7)
                 .arg(byScripts.at(i).name));

    check(!mods.isEmpty(), QStringLiteral("the scan finds the installed mods"));
    check(pbosRefused * 4 < pbos,
          QStringLiteral("the reader gets through most pbos (%1 refused of %2)")
              .arg(pbosRefused).arg(pbos));
    if (pbosRefused > 0) {
        int shown = 0;
        for (const ModEntry &mod : mods) {
            for (const ModPbo &pbo : mod.pbos) {
                if (pbo.readable || shown >= 5) continue;
                line(QStringLiteral("    refused        %1: %2")
                         .arg(QFileInfo(pbo.path).fileName(), pbo.error));
                shown++;
            }
        }
    }

    // The mods worth opening, in a fixed order so two runs sample the same set.
    QVector<ModEntry> candidates;
    for (const ModEntry &mod : mods)
        if (mod.hasScripts()) candidates.append(mod);
    std::sort(candidates.begin(), candidates.end(),
              [](const ModEntry &a, const ModEntry &b) { return a.folder < b.folder; });

    const int stride = candidates.isEmpty()
                           ? 1
                           : qMax(1, (candidates.size() + sampleTarget - 1) / sampleTarget);

    line(QString());
    line(QStringLiteral("Opening a sample"));

    // Both caps are per mod, and both are printed, because a capped run that
    // says nothing about its caps reads as a full one.
    ModOpenOptions opts;
    opts.maxFiles = 20;
    opts.maxBytes = 1024 * 1024;

    constexpr qint64 kBudgetMs = 70000;
    QElapsedTimer budget;
    budget.start();

    int sampled = 0, extracted = 0, imported = 0, refused = 0, classes = 0;
    int asNodes = 0, asText = 0, empty = 0, lowered = 0, statements = 0, notes = 0;
    int outsideCache = 0, notesDropped = 0;
    bool ranOut = false;
    QVector<Worst> worst;

    for (int i = 0; i < candidates.size(); i += stride) {
        if (budget.elapsed() > kBudgetMs) { ranOut = true; break; }
        const ModEntry &mod = candidates.at(i);
        Project project;
        const ModOpenResult result = openMod(mod, cat, builtins, project, opts);
        sampled++;
        extracted += result.filesExtracted;
        imported += result.filesImported;
        refused += result.filesRefused;
        classes += result.classes.size();
        asNodes += result.methodsAsNodes;
        asText += result.methodsAsText;
        empty += result.methodsEmpty;
        lowered += result.statementsLowered;
        statements += result.statementsTotal;
        notes += result.notes.size();
        notesDropped += result.notesDropped;

        for (const ModScriptFile &file : result.files)
            if (!ModLibrary::isInsideScriptCache(file.path)) outsideCache++;

        const int methods = result.methodsAsNodes + result.methodsAsText;
        if (methods >= 20) worst.append({mod.name, result.modelledPercent(), methods});
    }

    line(QStringLiteral("  sampled          %1 of %2 mods that ship script, 1 in %3%4")
             .arg(sampled)
             .arg(candidates.size())
             .arg(stride)
             .arg(ranOut ? QStringLiteral(", stopped at the time budget") : QString()));
    line(QStringLiteral("  per mod caps     %1 files, %2 KB in all, %3 KB a file")
             .arg(opts.maxFiles).arg(opts.maxBytes / 1024).arg(opts.maxFileBytes / 1024));
    line(QStringLiteral("  files extracted  %1").arg(extracted));
    line(QStringLiteral("  files imported   %1, %2 refused").arg(imported).arg(refused));
    line(QStringLiteral("  classes imported %1").arg(classes));

    const int methodTotal = asNodes + asText;
    const int percent = methodTotal > 0 ? int(qRound(100.0 * asNodes / methodTotal)) : 0;
    line(QStringLiteral("  methods          %1 became nodes, %2 kept their text  (%3%)")
             .arg(asNodes).arg(asText).arg(percent));
    line(QStringLiteral("  empty bodies     %1, counted in neither").arg(empty));
    if (statements > 0)
        line(QStringLiteral("  statements       %1 of %2 lowered  (%3%)")
                 .arg(lowered).arg(statements)
                 .arg(int(qRound(100.0 * lowered / statements))));
    line(QStringLiteral("  notes            %1 kept, %2 dropped past the cap")
             .arg(notes).arg(notesDropped));
    // Same measure importtest prints for vanilla source, so the two are worth
    // reading side by side: mod code calls mod methods the catalogue does not
    // know, and a call it cannot resolve is a statement that stays text.
    line(QStringLiteral("  compare with     the methods-as-nodes line in importtest"));
    line(QStringLiteral("  took             %1 s").arg(budget.elapsed() / 1000));

    check(sampled > 0, QStringLiteral("at least one installed mod was opened"));
    check(classes > 0, QStringLiteral("classes came out of them"));
    check(outsideCache == 0,
          QStringLiteral("every extracted file landed in the app's cache (%1 did not)")
              .arg(outsideCache));
    // The obfuscated archives carry six figures of junk entry names. A run that
    // kept a note for each would hold a list nobody can read.
    check(notes <= sampled * 200,
          QStringLiteral("no mod floods the note list (%1 notes over %2 mods)")
              .arg(notes).arg(sampled));

    if (!worst.isEmpty()) {
        std::sort(worst.begin(), worst.end(),
                  [](const Worst &a, const Worst &b) { return a.percent < b.percent; });
        line(QString());
        line(QStringLiteral("  Mods the importer models least, of those sampled"));
        for (int i = 0; i < qMin(5, worst.size()); ++i)
            line(QStringLiteral("    %1%  %2 methods  %3")
                     .arg(worst.at(i).percent, 3)
                     .arg(worst.at(i).methods, 4)
                     .arg(worst.at(i).name));
        line(QStringLiteral("  and the ones it models best"));
        for (int i = qMax(0, worst.size() - 3); i < worst.size(); ++i)
            line(QStringLiteral("    %1%  %2 methods  %3")
                     .arg(worst.at(i).percent, 3)
                     .arg(worst.at(i).methods, 4)
                     .arg(worst.at(i).name));
    }
}

// ------------------------------------------------------------------ the panel

static void shootPanel(QApplication &app, Document *doc, const QString &path)
{
    theme::apply(app);
    auto *panel = new ModBrowserPanel(doc);
    panel->resize(680, 620);
    panel->show();

    // The scan runs on a thread, so the picture is worth taking once it has
    // finished rather than of an empty list.
    QElapsedTimer clock;
    clock.start();
    while (panel->library()->isScanning() && clock.elapsed() < 60000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    QApplication::processEvents();

    // Then open a mod that ships script, so the class list is not empty in the
    // picture either.
    for (const ModEntry &mod : panel->library()->mods()) {
        if (!mod.hasScripts() || mod.scriptCount() > 40) continue;
        panel->selectMod(mod.folder);
        break;
    }
    clock.restart();
    while (clock.elapsed() < 8000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }

    line(panel->grab().save(path) ? QStringLiteral("  wrote %1").arg(path)
                                  : QStringLiteral("  could not write %1").arg(path));

    // The Open dialog, which is the way in for a mod that is not installed and
    // is the one piece of this panel with no row of its own to look at. exec()
    // blocks, so the picture is taken from a timer inside it and the dialog is
    // closed from there too, which is also the cancel path.
    QString dialogShot = path;
    dialogShot.replace(QRegularExpression(QStringLiteral("\\.png$"),
                                          QRegularExpression::CaseInsensitiveOption),
                       QString());
    dialogShot += QStringLiteral("-open.png");
    QTimer::singleShot(600, [dialogShot]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (!dialog) {
            line(QStringLiteral("  no Open dialog came up"));
            return;
        }
        line(dialog->grab().save(dialogShot)
                 ? QStringLiteral("  wrote %1").arg(dialogShot)
                 : QStringLiteral("  could not write %1").arg(dialogShot));
        dialog->close();
    });
    check(!panel->openFromDisk(),
          QStringLiteral("a cancelled Open dialog opens nothing"));

    delete panel;
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Its own application name on purpose: the cache and the extracted scripts
    // then live beside the real ones instead of overwriting them.
    QCoreApplication::setOrganizationName(QStringLiteral("SUDO"));
    QCoreApplication::setApplicationName(QStringLiteral("SUDO DayZ Node Mod Test"));

    QTextStream stream(stdout);
    out = &stream;

    const QStringList args = QCoreApplication::arguments();
    QString root = QStringLiteral("resources");
    int sampleTarget = 40;
    QString shot;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QLatin1String("--shot") && i + 1 < args.size()) { shot = args.at(++i); continue; }
        bool number = false;
        const int value = arg.toInt(&number);
        if (number && value > 0) { sampleTarget = value; continue; }
        if (!arg.startsWith(QLatin1Char('-'))) root = arg;
    }

    line(QStringLiteral("Mod library"));
    line(QStringLiteral("  cache            %1").arg(ModLibrary::scriptCacheRoot()));

    Catalog cat;
    if (!cat.load(QDir(root).filePath(QStringLiteral("catalog.json")))) {
        line(QStringLiteral("cannot load catalog.json: ") + cat.error());
        return 2;
    }
    Builtins builtins;

    testSyntheticMod(cat, builtins);
    testBrowsedGraphNeverExported(cat, builtins);
    testOpenPathsAreReadOnly(cat, builtins);
    testCacheRoundTrip();
    testInstalledCorpus(cat, builtins, sampleTarget);

    if (!shot.isEmpty()) {
        line(QString());
        line(QStringLiteral("The browser"));
        Document doc;
        doc.loadCatalog(QDir(root).filePath(QStringLiteral("catalog.json")));
        shootPanel(app, &doc, shot);
    }

    line(QString());
    line(failures == 0 ? QStringLiteral("All checks passed.")
                       : QStringLiteral("%1 checks failed.").arg(failures));
    return failures == 0 ? 0 : 1;
}
