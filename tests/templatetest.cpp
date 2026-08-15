// Headless check of the mod scaffolder: a folder made here has to be
// indistinguishable from one made by running the template's own Init.ps1.
//
// The load bearing assertion is that the "ModTemplate" token is gone from
// every file name and every file body. A token left in one config is not a
// visible failure, it is a Workbench load error days later.
#include "modtemplate.h"

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
    opts.author = QStringLiteral("Dillan Stephenson");
    opts.includeMissions = false;

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
    check(config.contains(QStringLiteral("author=\"Dillan Stephenson\"")),
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
    withMap.author = QStringLiteral("Dillan Stephenson");
    withMap.includeMissions = true;
    withMap.maps = { QStringLiteral("ChernarusPlus") };
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

    out << Qt::endl
        << (failures == 0 ? QStringLiteral("ALL TEMPLATE TESTS PASSED")
                          : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
