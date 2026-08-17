// Headless check of the mod scaffolder: a folder made here has to be
// indistinguishable from one made by running the template's own Init.ps1.
//
// The load bearing assertion is that the "ModTemplate" token is gone from
// every file name and every file body. A token left in one config is not a
// visible failure, it is a Workbench load error days later.
//
// The second half covers the work drive junction, which is the other thing
// creating a mod has to get right. A stand in drive is used throughout: a
// QTemporaryDir plays P:, and nothing in this file creates, renames or removes
// anything under the real work drive, which holds the user's unpacked game
// data and every other mod they have.
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "enforce/import.h"
#include "modtemplate.h"
#include "project.h"
#include "workdrive.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
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

static QStringList relativeFiles(const QString &root)
{
    QStringList files;
    const QDir dir(root);
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) files << dir.relativeFilePath(it.next());
    files.sort();
    return files;
}

// Path plus size for every file, so "left untouched" means byte counts too and
// not just the same names.
static QStringList signature(const QString &root)
{
    QStringList out;
    const QDir dir(root);
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        out << QStringLiteral("%1:%2").arg(dir.relativeFilePath(path))
                   .arg(QFileInfo(path).size());
    }
    out.sort();
    return out;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

// The extensions the scaffolder rewrites, restated here on purpose: the test
// should say what the contract is rather than ask the code.
static bool rewritten(const QString &suffix)
{
    static const QStringList exts = {
        QStringLiteral("c"),   QStringLiteral("cpp"), QStringLiteral("gproj"),
        QStringLiteral("cfg"), QStringLiteral("xml"), QStringLiteral("json"),
        QStringLiteral("csv"), QStringLiteral("bat"), QStringLiteral("ps1"),
        QStringLiteral("md"),  QStringLiteral("lst"),
    };
    return exts.contains(suffix.toLower());
}

static QString describeEndings(const QByteArray &data)
{
    return QStringLiteral("%1 CR, %2 LF")
        .arg(data.count('\r'))
        .arg(data.count('\n'));
}

// ------------------------------------------------------------- the work drive

static QString stateName(WorkDriveState state)
{
    switch (state) {
    case WorkDriveState::NoModFolder:     return QStringLiteral("NoModFolder");
    case WorkDriveState::NameReserved:    return QStringLiteral("NameReserved");
    case WorkDriveState::DriveMissing:    return QStringLiteral("DriveMissing");
    case WorkDriveState::Overlapping:     return QStringLiteral("Overlapping");
    case WorkDriveState::Linked:          return QStringLiteral("Linked");
    case WorkDriveState::LinkedElsewhere: return QStringLiteral("LinkedElsewhere");
    case WorkDriveState::NotLinked:       return QStringLiteral("NotLinked");
    case WorkDriveState::FolderIsCopy:    return QStringLiteral("FolderIsCopy");
    case WorkDriveState::FolderHasOwn:    return QStringLiteral("FolderHasOwn");
    case WorkDriveState::FolderUnchecked: return QStringLiteral("FolderUnchecked");
    case WorkDriveState::RealFile:        return QStringLiteral("RealFile");
    }
    return QStringLiteral("?");
}

static void checkState(const WorkDriveLink &link, WorkDriveState want,
                       const QString &what)
{
    check(link.state == want,
          QStringLiteral("%1: %2 (%3)")
              .arg(what, stateName(link.state), link.message()));
}

// Junctions made under the stand in drive, taken back out before the temporary
// directory is cleaned up. QDir::removeRecursively walking into a live junction
// would delete the mod folder it points at, which is the single mistake this
// whole area is about not making. Every link this test makes is a direct child
// of the drive, so one sweep at that depth covers all of them, including any
// left behind by a check that failed early.
struct DriveGuard {
    QString drive;
    ~DriveGuard()
    {
        if (drive.isEmpty()) return;
        const QDir dir(drive);
        for (const QString &name :
             dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden
                           | QDir::System)) {
            const QString path = dir.filePath(name);
            if (QFileInfo(path).isJunction()) QDir().rmdir(path);
        }
    }
};

static bool copyTree(const QString &from, const QString &to)
{
    if (!QDir().mkpath(to)) return false;
    const QDir src(from);
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString dest = to + QLatin1Char('/') + src.relativeFilePath(path);
        if (!QDir().mkpath(QFileInfo(dest).absolutePath())) return false;
        if (!QFile::copy(path, dest)) return false;
    }
    return true;
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data) == qint64(data.size());
}

// ------------------------------------------------- the templates that ship .c
//
// The start page's Files tiles do not write a skeleton, they import real
// Enforce out of resources/templates. That makes one thing checkable that
// nothing else about a template is: read the file, import it, generate it back,
// and compare the bytes.
//
// It matters because starting from a template is exactly that round trip. The
// tile imports the file; the first export writes it back out of the graph. If
// the two differ, the code a user gets is not the code that was reviewed, and a
// template that hands somebody source which does not compile is worse than no
// template at all, because they cannot tell whether they broke it or it arrived
// broken.
//
// The importer already guarantees the halves it could not model come back
// verbatim: a method it cannot lower keeps its text and is written out as it
// was. So a difference here is never a shrug, it is a real loss, and the run
// names the file it happened in.

// What one template file came back as, and how much of it became nodes.
struct RoundTrip {
    QString path;
    QString rel;          // under resources/templates, for the report
    bool ok = false;      // imported at all
    bool exact = false;   // and regenerated byte for byte
    int classes = 0;
    int statements = 0;   // statements the parser found
    int lowered = 0;      // and the ones that became nodes
    int methods = 0;
    int rawBodies = 0;    // methods kept whole as text
    QStringList rawNames; // and which ones, so -v says where to look
    QStringList notes;    // what the importer said about the file
    QString generated;
    QString error;
};

// A method the importer could not model kept its text. Counted rather than
// hidden, because it is the honest measure of how much of a template is a graph
// and how much is a text box with a comment on it.
static int rawBodiesIn(const Graph &g)
{
    int raw = 0;
    for (const GraphFunction &fn : g.functions)
        if (!fn.rawBody.isEmpty()) raw++;
    return raw;
}

// One whole template, imported the way the window imports it: one project,
// every file in script module order, each one landing in the project the ones
// before it are already in.
//
// The order is not tidiness. A 5_Mission file that calls into a class declared
// in 3_Game only resolves that call if the 3_Game file is already in the
// project, and a call that resolves becomes a node while a call that does not
// stays text. Reading each file into a project of its own would report a node
// count nobody ever sees, and would report it as worse than it is.
static QVector<RoundTrip> roundTripTemplate(const QStringList &sources,
                                            const QString &templatesRoot,
                                            const Catalog &cat,
                                            const Builtins &builtins)
{
    QVector<RoundTrip> trips;
    Project project;
    // Where each file's scripts landed in project.scripts, so the second pass
    // can generate exactly the classes that came out of that file.
    QVector<int> firstScript;
    QVector<QString> preambles;
    QVector<QString> eols;
    QVector<QString> sourceText;

    for (const QString &path : sources) {
        RoundTrip trip;
        trip.path = path;
        trip.rel = QDir(templatesRoot).relativeFilePath(path);
        trips.append(trip);
        firstScript.append(project.scripts.size());
        preambles.append(QString());
        eols.append(QStringLiteral("\n"));
        sourceText.append(QString());

        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) {
            trips.last().error = QStringLiteral("cannot open it");
            continue;
        }
        const QString source = QString::fromUtf8(in.readAll());
        in.close();
        sourceText.last() = source;

        const ImportResult result = importEnforceText(source, cat, builtins, project);
        if (!result.ok || result.scripts.isEmpty()) {
            trips.last().error = result.error.isEmpty() ? QStringLiteral("no class in it")
                                                        : result.error;
            continue;
        }

        trips.last().ok = true;
        trips.last().classes = result.scripts.size();
        trips.last().statements = result.totalStatements();
        trips.last().lowered = result.totalLowered();
        trips.last().notes = result.notes;
        preambles.last() = result.preamble;
        eols.last() = result.eol;

        // The same fields MainWindow::appendImportedScripts copies across. The
        // header fields live on the ImportedScript rather than on the graph, so
        // taking them from the struct is what stops `modded class X` coming
        // back as `class X extends X`.
        for (const ImportedScript &imported : result.scripts) {
            ScriptEntry entry;
            entry.id = QStringLiteral("s%1").arg(project.scripts.size());
            entry.graph = imported.graph;
            if (!imported.className.isEmpty()) entry.graph.className = imported.className;
            if (!imported.baseClass.isEmpty()) entry.graph.baseClass = imported.baseClass;
            if (imported.modded) entry.graph.modded = true;
            entry.name = entry.graph.className;
            entry.folder = QFileInfo(QFileInfo(path).absolutePath()).fileName();
            entry.graph.module = entry.folder;
            project.scripts.append(entry);
        }
    }

    // Second pass, with the whole project in hand, which is the state the app
    // is in the moment the tile has finished.
    for (int i = 0; i < trips.size(); ++i) {
        RoundTrip &trip = trips[i];
        if (!trip.ok) continue;
        QStringList classes;
        for (int s = firstScript.at(i); s < firstScript.at(i) + trip.classes; ++s) {
            const Graph &g = project.scripts.at(s).graph;
            trip.methods += g.functions.size();
            trip.rawBodies += rawBodiesIn(g);
            for (const GraphFunction &fn : g.functions)
                if (!fn.rawBody.isEmpty())
                    trip.rawNames << g.className + QStringLiteral("::") + fn.name;
            classes << generateEnforce(g, cat, builtins, project).code;
        }
        trip.generated = assembleScriptFile(classes, preambles.at(i), eols.at(i));
        trip.exact = trip.generated == sourceText.at(i);
    }
    return trips;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QByteArray token = QByteArrayLiteral("ModTemplate");
    const QString prefix = QStringLiteral("SUDO_Link");

    QString templateRoot;
    check(modTemplateAvailable(&templateRoot),
          QStringLiteral("bundled template found (%1)").arg(templateRoot));
    if (templateRoot.isEmpty()) {
        out << Qt::endl << "1 FAILURES" << Qt::endl;
        return 1;
    }
    out << "       template holds " << relativeFiles(templateRoot).size() << " files"
        << Qt::endl;

    // ================================================ the start page templates
    //
    // Six folders of real Enforce under resources/templates. Each one has to
    // import, and each one has to regenerate byte for byte.
    //
    // `--bless` writes the generated text back over the source. That is the
    // supported way to change one of these: edit the .c, run with --bless, read
    // the diff, and commit only if the diff is what you meant. It is never run
    // by the suite.
    {
        const QString resources = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                           : QStringLiteral("resources");
        const bool bless = app.arguments().contains(QStringLiteral("--bless"));
        const bool verbose = app.arguments().contains(QStringLiteral("-v"));
        out << Qt::endl << "start page templates: import, generate, compare" << Qt::endl;

        Catalog cat;
        if (!cat.load(resources + QStringLiteral("/catalog.json"))) {
            check(false, QStringLiteral("catalog.json loaded from %1 (%2)")
                             .arg(resources, cat.error()));
        } else {
            Builtins builtins;
            const QString templatesRoot = resources + QStringLiteral("/templates");
            // Named here rather than read off the folder, so a template that
            // was never installed is a failure and not a silent skip.
            const QStringList expected = {
                QStringLiteral("api-requests"),  QStringLiteral("starting-kit"),
                QStringLiteral("cf-module"),     QStringLiteral("cf-expansion"),
                QStringLiteral("player-stats"),  QStringLiteral("leaderboard"),
            };

            int files = 0;
            int nodeFiles = 0;
            int totalStatements = 0;
            int totalLowered = 0;
            int totalRaw = 0;
            int totalMethods = 0;
            for (const QString &id : expected) {
                const QString dir = templatesRoot + QLatin1Char('/') + id;
                if (!QFileInfo(dir).isDir()) {
                    check(false, QStringLiteral("%1: folder is there").arg(id));
                    continue;
                }

                QStringList sources;
                for (const QString &module : {QStringLiteral("3_Game"),
                                              QStringLiteral("4_World"),
                                              QStringLiteral("5_Mission")}) {
                    const QDir sub(dir + QLatin1Char('/') + module);
                    if (!sub.exists()) continue;
                    for (const QString &name : sub.entryList({QStringLiteral("*.c")},
                                                             QDir::Files, QDir::Name))
                        sources << sub.absoluteFilePath(name);
                }
                check(!sources.isEmpty(),
                      QStringLiteral("%1: ships at least one script").arg(id));

                const QVector<RoundTrip> trips =
                    roundTripTemplate(sources, templatesRoot, cat, builtins);
                for (const RoundTrip &trip : trips) {
                    files++;
                    if (!trip.ok) {
                        check(false, QStringLiteral("%1: imports (%2)")
                                         .arg(trip.rel, trip.error));
                        continue;
                    }
                    if (!trip.exact && bless) {
                        writeFile(trip.path, trip.generated.toUtf8());
                        out << "       blessed " << trip.rel << Qt::endl;
                        continue;
                    }
                    check(trip.exact,
                          QStringLiteral("%1: %2 class%3 back byte for byte")
                              .arg(trip.rel)
                              .arg(trip.classes)
                              .arg(trip.classes == 1 ? QString()
                                                     : QStringLiteral("es")));
                    totalStatements += trip.statements;
                    totalLowered += trip.lowered;
                    totalRaw += trip.rawBodies;
                    totalMethods += trip.methods;
                    if (trip.rawBodies == 0) nodeFiles++;
                    // The split, per file, because it is the real measure of
                    // whether this tool is ready to be started from and it must
                    // not be possible to read it off anything but the run.
                    out << QStringLiteral("       %1: %2 of %3 statements as nodes, "
                                          "%4 of %5 methods kept as text")
                               .arg(trip.rel)
                               .arg(trip.lowered)
                               .arg(trip.statements)
                               .arg(trip.rawBodies)
                               .arg(trip.methods)
                        << Qt::endl;
                    // Which ones, and what the importer said. A count on its
                    // own tells you there is work to do and not where.
                    if (!verbose) continue;
                    for (const QString &name : trip.rawNames)
                        out << "         text  " << name << Qt::endl;
                    for (const QString &note : trip.notes)
                        out << "         note  " << note << Qt::endl;
                }
            }
            out << "       " << files << " template files, " << nodeFiles
                << " with no method kept as text, " << totalRaw << " of "
                << totalMethods << " methods as text, " << totalLowered << " of "
                << totalStatements << " statements lowered" << Qt::endl;
            check(files >= 12, QStringLiteral("every template file was looked at (%1)")
                                   .arg(files));
        }
    }

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temporary directory created"));
    if (!tmp.isValid()) return 1;
    // --keep leaves the scaffolded folders behind, which is the only way to
    // look at what was written when a check goes red.
    if (app.arguments().contains(QStringLiteral("--keep"))) {
        tmp.setAutoRemove(false);
        out << "       keeping " << tmp.path() << Qt::endl;
    }
    const QString sandbox = tmp.path();

    // The stand in for P:. Every scaffold below points at this instead of the
    // real work drive, so the junctions this test makes land here and the
    // user's own P: is only ever read. Declared after tmp and before the guard
    // so the links come out before either directory is cleaned up.
    QTemporaryDir driveTmp;
    check(driveTmp.isValid(), QStringLiteral("stand in work drive created"));
    if (!driveTmp.isValid()) return 1;
    const QString drive = driveTmp.path();
    DriveGuard guard;
    guard.drive = drive;
    out << "       stand in drive " << QDir::toNativeSeparators(drive) << Qt::endl;

    // ---------------------------------------------------------------- refusals
    out << Qt::endl << "refusals" << Qt::endl;
    {
        ModTemplateOptions bad;
        bad.prefix = QStringLiteral("9Lives");
        const ModTemplateResult r = scaffoldMod(sandbox, bad);
        check(!r.ok && !r.error.isEmpty(),
              QStringLiteral("digit leading prefix refused (%1)").arg(r.error));
        check(!QFileInfo::exists(sandbox + QStringLiteral("/9Lives")),
              QStringLiteral("nothing written for an invalid prefix"));
    }
    {
        ModTemplateOptions bad;
        bad.prefix = QStringLiteral("My Mod");
        const ModTemplateResult r = scaffoldMod(sandbox, bad);
        check(!r.ok, QStringLiteral("prefix with a space refused (%1)").arg(r.error));
    }
    {
        ModTemplateOptions bad;
        bad.prefix = QStringLiteral("ModTemplate");
        const ModTemplateResult r = scaffoldMod(sandbox, bad);
        check(!r.ok, QStringLiteral("the token itself refused (%1)").arg(r.error));
        check(!QFileInfo::exists(sandbox + QStringLiteral("/ModTemplate")),
              QStringLiteral("nothing written for the token prefix"));
    }

    // ------------------------------------------------------- scripts only mod
    out << Qt::endl << "scaffold without missions" << Qt::endl;
    ModTemplateOptions opts;
    opts.prefix = prefix;
    opts.displayName = QStringLiteral("SUDO Link");
    opts.author = QStringLiteral("SudoArkMan");
    opts.includeMissions = false;
    opts.workDrive = drive;

    const ModTemplateResult res = scaffoldMod(sandbox, opts);
    check(res.ok, QStringLiteral("scaffolded (%1)").arg(res.error));
    if (!res.ok) {
        out << Qt::endl << QStringLiteral("%1 FAILURES").arg(failures) << Qt::endl;
        return 1;
    }
    const QString modRoot = res.modRoot;
    out << "       modRoot     " << modRoot << Qt::endl;
    out << "       scriptsRoot " << res.scriptsRoot << Qt::endl;
    out << "       created " << res.created.size() << " files, skipped "
        << res.skipped.size() << " entries" << Qt::endl;

    check(QDir::cleanPath(modRoot)
              == QDir::cleanPath(sandbox + QLatin1Char('/') + prefix),
          QStringLiteral("modRoot is <parent>/<prefix>"));
    check(QFileInfo(res.scriptsRoot).isDir(), QStringLiteral("scriptsRoot exists on disk"));

    const QStringList files = relativeFiles(modRoot);
    out << "       tree holds " << files.size() << " files" << Qt::endl;
    check(res.created.size() == files.size(),
          QStringLiteral("created list matches the tree (%1 listed, %2 on disk)")
              .arg(res.created.size())
              .arg(files.size()));

    // Zero tolerance: the token must not survive in a name or in a body.
    QStringList nameHits;
    QStringList bodyHits;
    for (const QString &rel : files) {
        if (rel.contains(QLatin1String("ModTemplate"), Qt::CaseInsensitive))
            nameHits << rel;
        if (readFile(modRoot + QLatin1Char('/') + rel).contains(token))
            bodyHits << rel;
    }
    // Directories carry the token too, and a files walk never sees an empty one.
    QStringList dirHits;
    QDirIterator dirs(modRoot, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    int dirCount = 0;
    while (dirs.hasNext()) {
        const QString path = dirs.next();
        dirCount++;
        if (QFileInfo(path).fileName().contains(QLatin1String("ModTemplate"),
                                                Qt::CaseInsensitive))
            dirHits << QDir(modRoot).relativeFilePath(path);
    }
    out << "       tree holds " << dirCount << " folders" << Qt::endl;
    check(nameHits.isEmpty(),
          QStringLiteral("token in zero file names (%1)").arg(nameHits.join(", ")));
    check(dirHits.isEmpty(),
          QStringLiteral("token in zero folder names (%1)").arg(dirHits.join(", ")));
    check(bodyHits.isEmpty(),
          QStringLiteral("token in zero file bodies (%1)").arg(bodyHits.join(", ")));

    // ------------------------------------------------------------- config.cpp
    out << Qt::endl << "config.cpp" << Qt::endl;
    const QString configPath =
        QStringLiteral("%1/%2/Scripts/config.cpp").arg(modRoot, prefix);
    check(QFile::exists(configPath), QStringLiteral("config.cpp written"));
    const QString config = QString::fromUtf8(readFile(configPath));
    check(config.contains(QLatin1String("class CfgPatches")),
          QStringLiteral("CfgPatches present"));
    check(config.contains(QLatin1String("class CfgAddons")),
          QStringLiteral("CfgAddons present"));
    check(config.contains(QLatin1String("class CfgMods")),
          QStringLiteral("CfgMods present"));
    check(config.contains(QStringLiteral("class %1").arg(prefix)),
          QStringLiteral("mod class renamed to the prefix"));
    check(config.contains(QStringLiteral("dir=\"%1\"").arg(prefix)),
          QStringLiteral("dir points at the prefix folder"));
    for (const QString &module : { QStringLiteral("1_Core"), QStringLiteral("3_Game"),
                                  QStringLiteral("4_World"), QStringLiteral("5_Mission") }) {
        check(config.contains(QStringLiteral("\"%1/Scripts/%2\"").arg(prefix, module)),
              QStringLiteral("%1 module path rewritten").arg(module));
        check(QFileInfo(QStringLiteral("%1/%2/Scripts/%3/%2")
                            .arg(modRoot, prefix, module))
                  .isDir(),
              QStringLiteral("%1/%2 folder created").arg(module, prefix));
        check(QFile::exists(QStringLiteral("%1/%2/Scripts/%3/%2/.gitkeep")
                                .arg(modRoot, prefix, module)),
              QStringLiteral("%1 folder keeps a .gitkeep").arg(module));
    }
    check(config.contains(QStringLiteral("\"%1/Scripts/Inputs.xml\"").arg(prefix)),
          QStringLiteral("inputs path rewritten"));
    check(config.contains(QStringLiteral("name=\"SUDO Link\"")),
          QStringLiteral("display name filled in"));
    check(config.contains(QStringLiteral("author=\"SudoArkMan\"")),
          QStringLiteral("author filled in"));
    check(!config.contains(QLatin1String("name=\"\"")),
          QStringLiteral("no empty name left behind"));

    // ------------------------------------------------------------ dayz.gproj
    out << Qt::endl << "dayz.gproj" << Qt::endl;
    const QString gproj = QString::fromUtf8(
        readFile(QStringLiteral("%1/%2/Workbench/dayz.gproj").arg(modRoot, prefix)));
    check(gproj.contains(QStringLiteral("ID \"%1\"").arg(prefix)),
          QStringLiteral("project ID set to the prefix"));
    check(gproj.contains(QStringLiteral("TITLE \"%1\"").arg(prefix)),
          QStringLiteral("project TITLE set to the prefix"));
    check(gproj.contains(QStringLiteral("\"%1/Scripts/5_Mission\"").arg(prefix)),
          QStringLiteral("script module paths rewritten"));

    // ------------------------------------------------------- the empty folders
    out << Qt::endl << "folders Init.ps1 creates" << Qt::endl;
    for (const QString &dir : { QStringLiteral("Addons"), QStringLiteral("Missions/Global"),
                                QStringLiteral("Profiles/Dev"),
                                QStringLiteral("Profiles/Global") }) {
        check(QFileInfo(modRoot + QLatin1Char('/') + dir).isDir(),
              QStringLiteral("%1 exists").arg(dir));
        check(QFile::exists(modRoot + QLatin1Char('/') + dir + QStringLiteral("/.gitkeep")),
              QStringLiteral("%1 keeps a .gitkeep").arg(dir));
    }
    check(!QFileInfo(QStringLiteral("%1/Missions/%2.ChernarusPlus").arg(modRoot, prefix))
               .exists(),
          QStringLiteral("no ChernarusPlus mission when missions were not requested"));
    check(!QFileInfo(QStringLiteral("%1/Missions/%2.Enoch").arg(modRoot, prefix)).exists(),
          QStringLiteral("no Enoch mission when missions were not requested"));
    check(!res.skipped.filter(QStringLiteral("Missions")).isEmpty(),
          QStringLiteral("skipped list says why missions are absent"));

    // ------------------------------------------------- byte for byte fidelity
    out << Qt::endl << "fidelity against the template" << Qt::endl;
    // Everything outside Missions has to come out as the template file with the
    // token swapped and nothing else touched: same bytes, so same line endings,
    // same encoding, same trailing newline or lack of one. config.cpp and the
    // gproj are excluded because they also take the dialog's answers.
    const QStringList edited = { QStringLiteral("%1/Scripts/config.cpp").arg(prefix),
                                 QStringLiteral("%1/Workbench/dayz.gproj").arg(prefix) };
    int compared = 0;
    QStringList drifted;
    QStringList missing;
    for (const QString &rel : relativeFiles(templateRoot)) {
        if (rel.startsWith(QLatin1String("Missions/"))) continue;
        QString destRel = rel;
        destRel.replace(QLatin1String("ModTemplate"), prefix);
        if (edited.contains(destRel)) continue;
        const QString dest = modRoot + QLatin1Char('/') + destRel;
        if (!QFile::exists(dest)) {
            missing << destRel;
            continue;
        }
        QByteArray expected = readFile(templateRoot + QLatin1Char('/') + rel);
        if (rewritten(QFileInfo(rel).suffix()))
            expected.replace(token, prefix.toUtf8());
        if (readFile(dest) != expected) drifted << destRel;
        compared++;
    }
    out << "       compared " << compared << " files byte for byte" << Qt::endl;
    check(compared > 5, QStringLiteral("something was actually compared"));
    check(missing.isEmpty(),
          QStringLiteral("every non mission template file landed (%1)").arg(missing.join(", ")));
    check(drifted.isEmpty(),
          QStringLiteral("no file drifted from template bytes plus the token swap (%1)")
              .arg(drifted.join(", ")));

    // The two edited files still have to keep their line endings.
    for (const QString &rel : edited) {
        QString sourceRel = rel;
        sourceRel.replace(prefix, QLatin1String("ModTemplate"));
        const QByteArray was = readFile(templateRoot + QLatin1Char('/') + sourceRel);
        const QByteArray now = readFile(modRoot + QLatin1Char('/') + rel);
        check(describeEndings(was) == describeEndings(now),
              QStringLiteral("%1 line endings unchanged (%2 -> %3)")
                  .arg(rel, describeEndings(was), describeEndings(now)));
    }

    // --------------------------------------------- refusing a non-empty target
    out << Qt::endl << "refusing an occupied folder" << Qt::endl;
    const QStringList before = signature(modRoot);
    ModTemplateOptions again = opts;
    again.displayName = QStringLiteral("Something Else");
    const ModTemplateResult clash = scaffoldMod(sandbox, again);
    check(!clash.ok, QStringLiteral("second scaffold refused (%1)").arg(clash.error));
    check(clash.created.isEmpty(), QStringLiteral("refusal reports nothing created"));
    const QStringList after = signature(modRoot);
    check(before == after,
          QStringLiteral("existing folder untouched (%1 files before, %2 after)")
              .arg(before.size())
              .arg(after.size()));
    check(QString::fromUtf8(readFile(configPath))
              .contains(QStringLiteral("name=\"SUDO Link\"")),
          QStringLiteral("existing config.cpp not rewritten by the refusal"));

    // An empty folder is not an obstacle: a user who made the folder in
    // Explorer first and then browsed to it should not be turned away.
    {
        ModTemplateOptions pre;
        pre.prefix = QStringLiteral("SUDO_Pre");
        pre.displayName = QStringLiteral("SUDO Pre");
        pre.workDrive = drive;
        QDir().mkpath(sandbox + QStringLiteral("/SUDO_Pre"));
        const ModTemplateResult r = scaffoldMod(sandbox, pre);
        check(r.ok, QStringLiteral("scaffolds into an existing empty folder (%1)").arg(r.error));
        check(QFile::exists(
                  sandbox + QStringLiteral("/SUDO_Pre/SUDO_Pre/Scripts/config.cpp")),
              QStringLiteral("empty folder scaffold produced a config.cpp"));
    }

    // ------------------------------------------------------- one map requested
    out << Qt::endl << "scaffold with one mission" << Qt::endl;
    ModTemplateOptions withMap;
    withMap.prefix = QStringLiteral("SUDO_Chern");
    withMap.displayName = QStringLiteral("SUDO Chern");
    withMap.author = QStringLiteral("SudoArkMan");
    withMap.includeMissions = true;
    withMap.maps = { QStringLiteral("ChernarusPlus") };
    withMap.workDrive = drive;
    const ModTemplateResult mapped = scaffoldMod(sandbox, withMap);
    check(mapped.ok, QStringLiteral("scaffolded (%1)").arg(mapped.error));
    if (mapped.ok) {
        const QString root = mapped.modRoot;
        out << "       created " << mapped.created.size() << " files, skipped "
            << mapped.skipped.size() << " entries" << Qt::endl;
        check(QFileInfo(QStringLiteral("%1/Missions/SUDO_Chern.ChernarusPlus").arg(root))
                  .isDir(),
              QStringLiteral("ChernarusPlus mission copied"));
        check(QFile::exists(
                  QStringLiteral("%1/Missions/SUDO_Chern.ChernarusPlus/init.c").arg(root)),
              QStringLiteral("mission init.c copied"));
        check(QFile::exists(QStringLiteral(
                  "%1/Missions/SUDO_Chern.ChernarusPlus/db/types.xml").arg(root)),
              QStringLiteral("mission db copied"));
        check(!QFileInfo(QStringLiteral("%1/Missions/SUDO_Chern.Enoch").arg(root)).exists(),
              QStringLiteral("Enoch left behind"));
        check(!QFileInfo(QStringLiteral("%1/Missions/SUDO_Chern.sakhal").arg(root)).exists(),
              QStringLiteral("sakhal left behind"));
        check(!mapped.skipped.filter(QStringLiteral("Enoch")).isEmpty(),
              QStringLiteral("skipped list names Enoch"));

        QStringList hits;
        for (const QString &rel : relativeFiles(root)) {
            if (rel.contains(QLatin1String("ModTemplate"), Qt::CaseInsensitive)
                || readFile(root + QLatin1Char('/') + rel).contains(token))
                hits << rel;
        }
        check(hits.isEmpty(),
              QStringLiteral("token gone from the mission tree too (%1)").arg(hits.join(", ")));
    }

    // ------------------------------------------- every map, plus the extra blobs
    out << Qt::endl << "scaffold with every mission" << Qt::endl;
    // A stand in for a full template: one mission folder holding two of the
    // three oversized blobs, so the missing one has to turn up in `skipped`.
    const QString extra = sandbox + QStringLiteral("/full-template");
    QDir().mkpath(extra + QStringLiteral("/Missions/Vanilla.ChernarusPlus"));
    for (const QString &blob : { QStringLiteral("areaflags.map"),
                                 QStringLiteral("mapgroupcluster.xml"),
                                 QStringLiteral("mapgroupclusterpos.xml") }) {
        QFile f(extra + QStringLiteral("/Missions/Vanilla.ChernarusPlus/") + blob);
        if (f.open(QIODevice::WriteOnly)) f.write("<!-- stand in -->\r\n");
    }

    ModTemplateOptions all;
    all.prefix = QStringLiteral("SUDO_All");
    all.displayName = QStringLiteral("SUDO All");
    all.includeMissions = true;
    all.extraMissionSource = extra;
    all.workDrive = drive;
    const ModTemplateResult everything = scaffoldMod(sandbox, all);
    check(everything.ok, QStringLiteral("scaffolded (%1)").arg(everything.error));
    if (everything.ok) {
        const QString root = everything.modRoot;
        out << "       created " << everything.created.size() << " files, skipped "
            << everything.skipped.size() << " entries" << Qt::endl;
        for (const QString &map : { QStringLiteral("ChernarusPlus"), QStringLiteral("Enoch"),
                                    QStringLiteral("sakhal") }) {
            check(QFileInfo(QStringLiteral("%1/Missions/SUDO_All.%2").arg(root, map)).isDir(),
                  QStringLiteral("%1 mission copied when no map list was given").arg(map));
        }
        check(QFile::exists(QStringLiteral(
                  "%1/Missions/SUDO_All.ChernarusPlus/areaflags.map").arg(root)),
              QStringLiteral("areaflags.map pulled from the extra source"));
        check(QFile::exists(QStringLiteral(
                  "%1/Missions/SUDO_All.ChernarusPlus/mapgroupclusterpos.xml").arg(root)),
              QStringLiteral("mapgroupcluster*.xml glob pulled from the extra source"));
        check(!everything.skipped.filter(QStringLiteral("mapgroupproto.xml")).isEmpty(),
              QStringLiteral("missing mapgroupproto.xml reported in skipped"));
        check(!everything.skipped.filter(QStringLiteral("SUDO_All.Enoch: no")).isEmpty(),
              QStringLiteral("Enoch has no matching source folder, reported in skipped"));

        QStringList hits;
        for (const QString &rel : relativeFiles(root)) {
            if (rel.contains(QLatin1String("ModTemplate"), Qt::CaseInsensitive)
                || readFile(root + QLatin1Char('/') + rel).contains(token))
                hits << rel;
        }
        check(hits.isEmpty(),
              QStringLiteral("token gone from the full tree (%1)").arg(hits.join(", ")));
        out << "       full tree holds " << relativeFiles(root).size() << " files"
            << Qt::endl;
    }

    // ============================================================ work drive
    //
    // Every case P:\<Name> can be in when a mod is created, and what each one
    // is allowed to do about it. All of it against the stand in drive.

    out << Qt::endl << "work drive: a new mod lands linked" << Qt::endl;
    const QString modFolder = res.modFolder;
    const QString link = workDriveLinkFor(modFolder, drive);
    check(!modFolder.isEmpty() && QFileInfo(modFolder).isDir(),
          QStringLiteral("the result names the mod folder"));
    check(res.workDrive.ok,
          QStringLiteral("scaffolding linked it, with no second button press (%1)")
              .arg(res.workDrive.error));
    checkState(res.workDrive.link, WorkDriveState::Linked,
               QStringLiteral("state after scaffolding"));
    check(QFileInfo(link).isJunction(), QStringLiteral("the link is a junction"));
    check(QDir::cleanPath(QFileInfo(link).junctionTarget())
              == QDir::cleanPath(modFolder),
          QStringLiteral("it points at the mod folder, not at the project root"));
    // The one that matters to AddonBuilder: the config has to be reachable
    // through the link at the depth the PBO prefix expects.
    check(QFile::exists(link + QStringLiteral("/Scripts/config.cpp")),
          QStringLiteral("config.cpp resolves through the link"));
    check(!res.workDrive.command.isEmpty(),
          QStringLiteral("the mklink line is kept for the log"));
    check(res.workDrive.movedTo.isEmpty(),
          QStringLiteral("nothing was moved to make room"));
    for (const ModTemplateResult *r : { &res, &mapped, &everything }) {
        if (!r->ok) continue;
        check(r->workDrive.ok,
              QStringLiteral("%1 linked too (%2)")
                  .arg(QFileInfo(r->modFolder).fileName(), r->workDrive.error));
    }

    // The stand in drive is the point of the whole section. If any of these
    // fail, this test has been writing to the user's real work drive.
    out << Qt::endl << "work drive: the real P: was never touched" << Qt::endl;
    for (const QString &name : { QStringLiteral("SUDO_Link"), QStringLiteral("SUDO_Pre"),
                                 QStringLiteral("SUDO_Chern"), QStringLiteral("SUDO_All"),
                                 QStringLiteral("SUDO_Dup"), QStringLiteral("SUDO_Own"),
                                 QStringLiteral("SUDO_Whole") }) {
        check(!QFileInfo::exists(workDriveRoot() + name),
              QStringLiteral("%1%2 was not created")
                  .arg(QDir::toNativeSeparators(workDriveRoot()), name));
    }

    out << Qt::endl << "work drive: already correct" << Qt::endl;
    {
        const WorkDriveAction repeat = linkModFolder(link, modFolder);
        check(repeat.ok, QStringLiteral("linking again is a success"));
        checkState(repeat.link, WorkDriveState::Linked, QStringLiteral("state"));
        check(repeat.command.isEmpty(),
              QStringLiteral("nothing was run the second time"));
        check(repeat.link.fix().isEmpty(),
              QStringLiteral("a correct link asks nothing of the user"));
    }

    out << Qt::endl << "work drive: a junction pointing somewhere else" << Qt::endl;
    {
        const QString elsewhere = QDir(sandbox).filePath(QStringLiteral("elsewhere"));
        QDir().mkpath(elsewhere);
        const QString taken = QDir(drive).filePath(QStringLiteral("SUDO_Taken"));
        check(linkModFolder(taken, elsewhere).ok,
              QStringLiteral("a junction to another folder set up for the case"));

        const WorkDriveLink seen = inspectWorkDriveLink(taken, modFolder);
        checkState(seen, WorkDriveState::LinkedElsewhere, QStringLiteral("state"));
        check(QDir::cleanPath(seen.pointsAt) == QDir::cleanPath(elsewhere),
              QStringLiteral("it says where the junction points"));
        check(seen.message().contains(QDir::toNativeSeparators(elsewhere)),
              QStringLiteral("the message names that folder"));

        const WorkDriveAction refused = linkModFolder(taken, modFolder);
        check(!refused.ok, QStringLiteral("linking over it refused"));
        check(refused.command.isEmpty(), QStringLiteral("nothing was run"));
        const WorkDriveAction refusedMove = moveAsideAndLinkModFolder(taken, modFolder);
        check(!refusedMove.ok && refusedMove.movedTo.isEmpty(),
              QStringLiteral("moving it aside refused as well"));
        check(QFileInfo(taken).isJunction()
                  && QDir::cleanPath(QFileInfo(taken).junctionTarget())
                         == QDir::cleanPath(elsewhere),
              QStringLiteral("the other junction still points where it did"));
    }

    // ---------------------------------------------------------------- a copy
    //
    // The user's own case: P:\<Name> is a real folder holding a byte for byte
    // copy of the project. Nothing in it is unique, so it can be renamed out of
    // the way, and renaming is the most that is ever done to it.
    out << Qt::endl << "work drive: a real folder that is a copy" << Qt::endl;
    ModTemplateOptions dupOpts;
    dupOpts.prefix = QStringLiteral("SUDO_Dup");
    dupOpts.displayName = QStringLiteral("SUDO Dup");
    dupOpts.author = QStringLiteral("SudoArkMan");
    dupOpts.linkWorkDrive = false;
    const ModTemplateResult dup = scaffoldMod(sandbox, dupOpts);
    check(dup.ok, QStringLiteral("a second mod scaffolded (%1)").arg(dup.error));
    check(!dup.workDrive.attempted(),
          QStringLiteral("linking turned off means nothing was tried"));
    if (dup.ok) {
        const QString dupLink = workDriveLinkFor(dup.modFolder, drive);
        check(copyTree(dup.modFolder, dupLink),
              QStringLiteral("a copy of it placed at the link"));
        const int copied = relativeFiles(dup.modFolder).size();
        const QStringList wasThere = signature(dupLink);

        const WorkDriveLink seen = inspectWorkDriveLink(dupLink, dup.modFolder);
        checkState(seen, WorkDriveState::FolderIsCopy, QStringLiteral("state"));
        check(seen.uniqueTotal == 0, QStringLiteral("nothing in it is unique"));
        check(seen.files == copied,
              QStringLiteral("all %1 files were looked at (%2)").arg(copied).arg(seen.files));
        check(seen.compared == copied,
              QStringLiteral("all %1 read byte for byte, not trusted on size (%2)")
                  .arg(copied).arg(seen.compared));
        check(seen.canMoveAside(),
              QStringLiteral("a copy is the one case that can be cleared"));

        // Linking on its own never moves anything. The move is a separate call
        // because it is a separate decision, and the user makes it.
        const WorkDriveAction refused = linkModFolder(dupLink, dup.modFolder);
        check(!refused.ok && refused.command.isEmpty(),
              QStringLiteral("linking alone will not move a folder"));
        check(signature(dupLink) == wasThere,
              QStringLiteral("the folder is exactly as it was"));

        const WorkDriveAction moved = moveAsideAndLinkModFolder(dupLink, dup.modFolder);
        check(moved.ok, QStringLiteral("moved aside and linked (%1)").arg(moved.error));
        check(!moved.movedTo.isEmpty(), QStringLiteral("it says where the folder went"));
        check(QFileInfo(moved.movedTo).isDir(),
              QStringLiteral("the folder is still on disk under its new name"));
        check(signature(moved.movedTo) == wasThere,
              QStringLiteral("every one of its %1 files survived the move").arg(copied));
        check(QFileInfo(dupLink).isJunction(),
              QStringLiteral("the link is a junction now"));
        check(QFile::exists(dupLink + QStringLiteral("/Scripts/config.cpp")),
              QStringLiteral("config.cpp resolves through it"));
    }

    // --------------------------------------------- a copy of the whole project
    //
    // What was actually on the user's disk: P:\TimerTest held a copy of the
    // project, not of the mod folder the junction has to point at. Against the
    // mod folder alone that reads as a folder full of somebody's work, which is
    // the reading that turns a one click fix into a dead end.
    out << Qt::endl << "work drive: a real folder that copies the whole project"
        << Qt::endl;
    ModTemplateOptions wholeOpts;
    wholeOpts.prefix = QStringLiteral("SUDO_Whole");
    wholeOpts.displayName = QStringLiteral("SUDO Whole");
    wholeOpts.linkWorkDrive = false;
    const ModTemplateResult whole = scaffoldMod(sandbox, wholeOpts);
    check(whole.ok, QStringLiteral("a fourth mod scaffolded (%1)").arg(whole.error));
    if (whole.ok) {
        const QString wholeLink = workDriveLinkFor(whole.modFolder, drive);
        check(copyTree(whole.modRoot, wholeLink),
              QStringLiteral("the whole project copied to the link"));
        const QStringList wasThere = signature(wholeLink);

        checkState(inspectWorkDriveLink(wholeLink, whole.modFolder),
                   WorkDriveState::FolderHasOwn,
                   QStringLiteral("against the mod folder alone"));

        const WorkDriveLink seen =
            inspectWorkDriveLink(wholeLink, whole.modFolder, whole.modRoot);
        checkState(seen, WorkDriveState::FolderIsCopy,
                   QStringLiteral("with the project root as well"));
        check(QDir::cleanPath(seen.copyOf) == QDir::cleanPath(whole.modRoot),
              QStringLiteral("it says which folder it is a copy of"));
        check(seen.message().contains(QDir::toNativeSeparators(whole.modRoot)),
              QStringLiteral("and the message names that folder"));

        const WorkDriveAction moved =
            moveAsideAndLinkModFolder(wholeLink, whole.modFolder, whole.modRoot);
        check(moved.ok, QStringLiteral("moved aside and linked (%1)").arg(moved.error));
        check(signature(moved.movedTo) == wasThere,
              QStringLiteral("the copy survived the move whole"));
        check(QFile::exists(wholeLink + QStringLiteral("/Scripts/config.cpp")),
              QStringLiteral("the link points at the mod folder, not the project root"));
    }

    // ------------------------------------------------- a folder of their own
    out << Qt::endl << "work drive: a real folder with content of its own" << Qt::endl;
    ModTemplateOptions ownOpts;
    ownOpts.prefix = QStringLiteral("SUDO_Own");
    ownOpts.displayName = QStringLiteral("SUDO Own");
    ownOpts.linkWorkDrive = false;
    const ModTemplateResult own = scaffoldMod(sandbox, ownOpts);
    check(own.ok, QStringLiteral("a third mod scaffolded (%1)").arg(own.error));
    if (own.ok) {
        const QString ownLink = workDriveLinkFor(own.modFolder, drive);
        check(copyTree(own.modFolder, ownLink), QStringLiteral("copied to the link"));

        // One file that is not in the mod folder at all.
        check(writeFile(ownLink + QStringLiteral("/notes.txt"),
                        QByteArrayLiteral("a day of work\r\n")),
              QStringLiteral("a file of their own added"));
        // And one that is there, at the same size, with different bytes. Sizes
        // alone would call this a copy, which is how a folder full of work gets
        // renamed out from under somebody.
        const QString configRel = QStringLiteral("Scripts/config.cpp");
        QByteArray body = readFile(ownLink + QLatin1Char('/') + configRel);
        check(!body.isEmpty(), QStringLiteral("the copied config.cpp was readable"));
        body[0] = body.at(0) == 'x' ? 'y' : 'x';
        check(writeFile(ownLink + QLatin1Char('/') + configRel, body),
              QStringLiteral("one byte changed, same length"));

        const QStringList wasThere = signature(ownLink);
        const WorkDriveLink seen = inspectWorkDriveLink(ownLink, own.modFolder);
        checkState(seen, WorkDriveState::FolderHasOwn, QStringLiteral("state"));
        check(seen.uniqueTotal == 2,
              QStringLiteral("both are unique, the extra and the edited one (%1)")
                  .arg(seen.uniqueTotal));
        check(seen.unique.contains(QStringLiteral("notes.txt")),
              QStringLiteral("the extra file is named"));
        check(seen.unique.contains(configRel),
              QStringLiteral("the one that differs only in its bytes is named too"));
        check(!seen.canMoveAside(),
              QStringLiteral("a folder with anything of its own cannot be moved"));

        const WorkDriveAction refused = moveAsideAndLinkModFolder(ownLink, own.modFolder);
        check(!refused.ok && refused.movedTo.isEmpty(),
              QStringLiteral("the move refused and said why"));
        check(refused.error.contains(QStringLiteral("notes.txt")),
              QStringLiteral("the refusal names what it found"));
        check(signature(ownLink) == wasThere,
              QStringLiteral("the folder is exactly as it was"));
        check(!QFileInfo(ownLink).isJunction(),
              QStringLiteral("and it is still a real folder"));
    }

    // ------------------------------------- the project created on the drive
    //
    // What was really on the user's disk. P:\TimerTest carries TimerTest.sdzn
    // and P:\SUDO_Test_3 carries a SUDO_Test_3 folder inside it, so both are
    // projects that were scaffolded onto the work drive root rather than copies
    // some tool made. The link P:\<Name> would need is then the project folder
    // itself, and every other row of this table would read that folder as a
    // perfect copy of the project and offer to rename it. It is the one case
    // where the folder in the way is the work.
    out << Qt::endl << "work drive: the project is the link path" << Qt::endl;
    {
        ModTemplateOptions onDrive;
        onDrive.prefix = QStringLiteral("SUDO_OnDrive");
        onDrive.displayName = QStringLiteral("SUDO OnDrive");
        onDrive.workDrive = drive;
        // Created on the drive itself, which is what the dialog now refuses and
        // what every project made before it did.
        const ModTemplateResult r = scaffoldMod(drive, onDrive);
        check(r.ok, QStringLiteral("the mod is written (%1)").arg(r.error));
        if (r.ok) {
            const QStringList wasThere = signature(r.modRoot);
            check(QDir::cleanPath(r.modRoot)
                      == QDir::cleanPath(workDriveLinkFor(r.modFolder, drive)),
                  QStringLiteral("the project folder is the link path"));
            check(!r.workDrive.ok, QStringLiteral("so it is not linked"));
            checkState(r.workDrive.link, WorkDriveState::Overlapping,
                       QStringLiteral("state"));
            check(r.workDrive.command.isEmpty(), QStringLiteral("nothing was run"));
            check(r.workDrive.movedTo.isEmpty(), QStringLiteral("nothing was moved"));
            check(!r.workDrive.link.canMoveAside(),
                  QStringLiteral("and it can never be offered as a move"));
            check(!r.workDrive.link.fix().isEmpty(),
                  QStringLiteral("the fix says to keep the project off the drive"));

            // Asked for directly, which is what a caller that ignores the state
            // would do. It still refuses, because the guard is in the rule and
            // not in the caller.
            const WorkDriveAction forced =
                moveAsideAndLinkModFolder(r.modRoot, r.modFolder, r.modRoot);
            check(!forced.ok && forced.movedTo.isEmpty(),
                  QStringLiteral("moving it aside refuses even when asked outright"));
            check(signature(r.modRoot) == wasThere,
                  QStringLiteral("the project is exactly as it was written"));
            check(QFile::exists(r.modFolder + QStringLiteral("/Scripts/config.cpp")),
                  QStringLiteral("and its config.cpp is still where it belongs"));
        }

        // The other two ways two paths can overlap.
        checkState(inspectWorkDriveLink(modFolder, modFolder),
                   WorkDriveState::Overlapping, QStringLiteral("a link to itself"));
        // Not Scripts: P:\scripts is the vanilla tree, so that name is answered
        // by the reserved rule one line earlier and would prove nothing here.
        checkState(inspectWorkDriveLink(modFolder + QStringLiteral("/Workbench"),
                                        modFolder),
                   WorkDriveState::Overlapping,
                   QStringLiteral("a link inside the mod folder"));
    }

    // ------------------------------------------------------- reserved names
    out << Qt::endl << "work drive: names the drive already owns" << Qt::endl;
    for (const QString &name : { QStringLiteral("DZ"), QStringLiteral("Mods"),
                                 QStringLiteral("Core"), QStringLiteral("scripts") }) {
        check(isReservedWorkDriveName(name),
              QStringLiteral("%1 is reserved").arg(name));
        // Called on its own line: the reason is written by the call, and
        // reading it in the same expression is not ordered against it.
        QString reason;
        const bool valid = isValidModPrefix(name, &reason);
        check(!valid, QStringLiteral("%1 refused as a prefix (%2)").arg(name, reason));

        ModTemplateOptions bad;
        bad.prefix = name;
        bad.workDrive = drive;
        const ModTemplateResult r = scaffoldMod(sandbox, bad);
        check(!r.ok, QStringLiteral("%1 refused before anything was written").arg(name));
        check(!QFileInfo::exists(QDir(sandbox).filePath(name)),
              QStringLiteral("no %1 folder created").arg(name));

        checkState(inspectWorkDriveLink(QDir(drive).filePath(name), modFolder),
                   WorkDriveState::NameReserved, QStringLiteral("%1 at the link").arg(name));
    }
    check(!isReservedWorkDriveName(QStringLiteral("SUDO_Link")),
          QStringLiteral("an ordinary prefix is not reserved"));

    // -------------------------------------------------------- drive not there
    out << Qt::endl << "work drive: not mounted" << Qt::endl;
    {
        const QString absent = QDir(sandbox).filePath(QStringLiteral("no-such-drive"));
        ModTemplateOptions offline;
        offline.prefix = QStringLiteral("SUDO_Offline");
        offline.displayName = QStringLiteral("SUDO Offline");
        offline.workDrive = absent;
        const ModTemplateResult r = scaffoldMod(sandbox, offline);
        check(r.ok, QStringLiteral("the mod is written anyway (%1)").arg(r.error));
        check(QFile::exists(r.modFolder + QStringLiteral("/Scripts/config.cpp")),
              QStringLiteral("and it is complete"));
        check(!r.workDrive.ok, QStringLiteral("but it is not linked"));
        checkState(r.workDrive.link, WorkDriveState::DriveMissing, QStringLiteral("state"));
        check(r.workDrive.command.isEmpty(), QStringLiteral("nothing was run"));
        check(!r.workDrive.link.fix().isEmpty(),
              QStringLiteral("the fix is spelled out (%1)").arg(r.workDrive.link.fix()));
        check(!QFileInfo::exists(absent),
              QStringLiteral("the missing drive folder was not created either"));

        // A drive letter is the case with a one line answer, so it gives one.
        QString letter;
        for (const QString &candidate : { QStringLiteral("Y:/"), QStringLiteral("X:/"),
                                          QStringLiteral("W:/"), QStringLiteral("V:/") }) {
            if (!QFileInfo(candidate).isDir()) { letter = candidate; break; }
        }
        if (letter.isEmpty()) {
            out << "       every candidate letter is mounted, subst wording skipped"
                << Qt::endl;
        } else {
            const WorkDriveLink seen =
                inspectWorkDriveLink(letter + QStringLiteral("SUDO_Offline"), modFolder);
            checkState(seen, WorkDriveState::DriveMissing,
                       QStringLiteral("an unmounted letter"));
            check(seen.fix().contains(QStringLiteral("subst ") + letter.left(2)),
                  QStringLiteral("the fix carries the subst line (%1)").arg(seen.fix()));
        }
    }

    // ---------------------------------------------------- the remaining two
    out << Qt::endl << "work drive: a file, and no mod folder" << Qt::endl;
    {
        const QString filePath = QDir(drive).filePath(QStringLiteral("SUDO_File"));
        check(writeFile(filePath, QByteArrayLiteral("not a folder")),
              QStringLiteral("a file placed at a link path"));
        checkState(inspectWorkDriveLink(filePath, modFolder), WorkDriveState::RealFile,
                   QStringLiteral("state"));
        const WorkDriveAction refused = linkModFolder(filePath, modFolder);
        check(!refused.ok && refused.command.isEmpty(),
              QStringLiteral("linking over a file refused, and ran nothing"));
        check(QFileInfo(filePath).isFile() && readFile(filePath) == "not a folder",
              QStringLiteral("the file is untouched"));

        checkState(inspectWorkDriveLink(QDir(drive).filePath(QStringLiteral("SUDO_Ghost")),
                                        QDir(sandbox).filePath(QStringLiteral("nothing"))),
                   WorkDriveState::NoModFolder, QStringLiteral("no mod folder to link"));
    }

    // ------------------------------------------- a project that arrives later
    //
    // Creating a mod is not the only way to end up with no link. A clone, a
    // folder that moved, or a project made before any of this existed all reach
    // the same place, and the answer has to be the same one.
    out << Qt::endl << "work drive: a project opened later" << Qt::endl;
    {
        checkState(inspectModFolder(modFolder, drive), WorkDriveState::Linked,
                   QStringLiteral("one that is already linked"));

        const QString later =
            QDir(sandbox).filePath(QStringLiteral("cloned/SUDO_Later"));
        check(writeFile(later + QStringLiteral("/Workbench/dayz.gproj"),
                        QByteArrayLiteral("ID \"SUDO_Later\"\r\n")),
              QStringLiteral("a mod folder that was never scaffolded here"));
        const WorkDriveLink seen = inspectModFolder(later, drive);
        checkState(seen, WorkDriveState::NotLinked, QStringLiteral("state"));
        check(seen.link == QDir::cleanPath(QDir(drive).filePath(
                  QStringLiteral("SUDO_Later"))),
              QStringLiteral("the link it needs is named"));
        check(!seen.fix().isEmpty(), QStringLiteral("and what to do about it"));

        const WorkDriveAction made = linkModFolder(seen.link, later);
        check(made.ok, QStringLiteral("linking it works the same way (%1)").arg(made.error));
        checkState(inspectModFolder(later, drive), WorkDriveState::Linked,
                   QStringLiteral("state afterwards"));
    }

    out << Qt::endl
        << (failures == 0 ? QStringLiteral("ALL TEMPLATE TESTS PASSED")
                          : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
