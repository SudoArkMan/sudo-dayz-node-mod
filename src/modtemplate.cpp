// Scaffolds a mod folder from the bundled template.
//
// One rule shapes the whole file: the mod folder comes out complete or nothing
// is written at all. Every check that can fail runs before the first byte
// lands, and an I/O failure part way through unwinds what was already copied.
// A half made mod folder is worse than no mod folder, because the user has to
// work out which half is missing.
#include "modtemplate.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace {

// The placeholder the template stands on, named the same way its own Init.ps1
// names it. Changing this breaks every scaffolded folder's parity with a
// hand run of Init.ps1.
const QLatin1String kToken("ModTemplate");

// Files whose contents get the token swapped. Init.ps1 covers .cpp, .gproj and
// .cfg only; the token also reaches the mission configs, the launch scripts and
// the readme, and one stale "ModTemplate" left behind surfaces much later as a
// Workbench load error nobody connects back to scaffolding. Anything not listed
// is copied byte for byte, which is what keeps binaries intact.
bool isRewritable(const QString &suffix)
{
    static const QSet<QString> exts = {
        QStringLiteral("c"),     QStringLiteral("cpp"),  QStringLiteral("h"),
        QStringLiteral("hpp"),   QStringLiteral("gproj"), QStringLiteral("cfg"),
        QStringLiteral("xml"),   QStringLiteral("json"), QStringLiteral("csv"),
        QStringLiteral("bat"),   QStringLiteral("ps1"),  QStringLiteral("md"),
        QStringLiteral("lst"),   QStringLiteral("txt"),  QStringLiteral("layout"),
        QStringLiteral("styles"),
    };
    return exts.contains(suffix.toLower());
}

QString applyToken(const QString &path, const QString &prefix)
{
    QString out = path;
    out.replace(kToken, prefix);
    return out;
}

// Byte level, so CRLF, tabs and any byte order mark survive exactly as the
// template wrote them. A UTF-16 file would not match the ASCII token and is
// left alone rather than mangled.
QByteArray rewriteToken(const QByteArray &in, const QByteArray &prefix)
{
    QByteArray out = in;
    out.replace(QByteArray(kToken.data(), int(kToken.size())), prefix);
    return out;
}

bool readAll(const QString &path, QByteArray &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, f.errorString());
        return false;
    }
    out = f.readAll();
    return true;
}

bool writeAll(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    if (f.write(data) != qint64(data.size()) || !f.flush()) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

// QFile::copy carries the source permissions across. Template files ship read
// only when the app is installed under Program Files, and a mod folder the
// user cannot edit is no use to anyone.
void makeWritable(const QString &path)
{
    const QFileDevice::Permissions p = QFile::permissions(path);
    if (p & QFileDevice::WriteOwner) return;
    QFile::setPermissions(path, p | QFileDevice::WriteOwner | QFileDevice::WriteUser);
}

bool ensureDir(const QString &path, QString *error)
{
    if (QDir().mkpath(path)) return true;
    if (error) *error = QStringLiteral("Cannot create %1.").arg(path);
    return false;
}

bool copyFile(const QString &source, const QString &dest, QString *error)
{
    if (QFile::exists(dest)) QFile::remove(dest);
    if (!QFile::copy(source, dest)) {
        if (error) *error = QStringLiteral("Cannot copy %1 to %2.").arg(source, dest);
        return false;
    }
    makeWritable(dest);
    return true;
}

// config.cpp has no escape sequence for a double quote inside a string, so a
// quote in the display name becomes an apostrophe rather than a config the
// engine refuses to parse.
QString configSafe(const QString &value)
{
    QString out = value.simplified();
    out.replace(QLatin1Char('"'), QLatin1Char('\''));
    return out;
}

// Fills one of the empty `field="";` slots the template ships. Searching from
// `from` keeps the edit inside the CfgMods block.
void setConfigField(QString &text, int from, const QString &field, const QString &value)
{
    const QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*\"[^\"]*\"").arg(field));
    const QRegularExpressionMatch m = re.match(text, from);
    if (!m.hasMatch()) return;
    text.replace(m.capturedStart(), m.capturedLength(),
                 QStringLiteral("%1=\"%2\"").arg(field, configSafe(value)));
}

// Workbench keys the project on ID, so it has to be the prefix whatever the
// template happened to say.
void setGprojField(QString &text, const QString &field, const QString &value)
{
    const QRegularExpression re(QStringLiteral("^([ \\t]*%1[ \\t]+)\"[^\"]*\"").arg(field),
                                QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return;
    text.replace(m.capturedStart(), m.capturedLength(),
                 m.captured(1) + QLatin1Char('"') + value + QLatin1Char('"'));
}

// "ModTemplate.ChernarusPlus" gives back "ChernarusPlus". Empty for any other
// folder under Missions, such as the shared Dev overrides.
QString missionMap(const QString &folder)
{
    const QString lead = QString(kToken) + QLatin1Char('.');
    if (!folder.startsWith(lead)) return QString();
    return folder.mid(lead.size());
}

bool containsCI(const QStringList &list, const QString &value)
{
    for (const QString &s : list)
        if (s.compare(value, Qt::CaseInsensitive) == 0) return true;
    return false;
}

// Where the oversized vanilla blobs live in whatever the user pointed at. The
// path may be a full template, a Missions folder or one mission folder, and
// the mission there is rarely named after this mod.
QString findMissionSource(const QString &root, const QString &map)
{
    const QFileInfo rootInfo(root);
    if (root.isEmpty() || !rootInfo.isDir()) return QString();

    const QString suffix = QLatin1Char('.') + map;
    if (rootInfo.fileName().endsWith(suffix, Qt::CaseInsensitive))
        return rootInfo.absoluteFilePath();

    const QStringList places = {
        rootInfo.absoluteFilePath(),
        rootInfo.absoluteFilePath() + QStringLiteral("/Missions"),
    };
    for (const QString &place : places) {
        QDir d(place);
        if (!d.exists()) continue;
        for (const QString &name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (name.endsWith(suffix, Qt::CaseInsensitive)
                || name.compare(map, Qt::CaseInsensitive) == 0)
                return d.absoluteFilePath(name);
        }
    }

    // A mission folder pointed at directly under some other name.
    if (QFile::exists(rootInfo.absoluteFilePath() + QStringLiteral("/areaflags.map")))
        return rootInfo.absoluteFilePath();
    return QString();
}

struct PlanItem {
    QString source;    // absolute path in the template
    QString relative;  // path below the mod root, token already applied
    bool rewrite = false;
};

} // namespace

bool modTemplateAvailable(QString *pathOut)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/resources/mod-template",
        appDir + "/../resources/mod-template",
        appDir + "/../../resources/mod-template",
        appDir + "/../../../resources/mod-template",
        appDir + "/../../../../resources/mod-template",
    };
    for (const QString &c : candidates) {
        if (!QFileInfo(c).isDir()) continue;
        if (pathOut) *pathOut = QDir::cleanPath(c);
        return true;
    }
    return false;
}

bool isValidModPrefix(const QString &prefix, QString *reason)
{
    if (prefix.isEmpty()) {
        if (reason) *reason = QStringLiteral("Enter a prefix.");
        return false;
    }
    // The prefix becomes a class name, a folder name and a PBO name, so it has
    // to survive all three.
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    if (!ok.match(prefix).hasMatch()) {
        if (reason)
            *reason = QStringLiteral("Use letters, digits and underscores, starting "
                                     "with a letter.");
        return false;
    }
    if (prefix.compare(QStringLiteral("ModTemplate"), Qt::CaseInsensitive) == 0) {
        if (reason) *reason = QStringLiteral("Pick a name of your own.");
        return false;
    }
    // The prefix is also the name of the junction on the work drive, so a
    // prefix that collides with the game's unpacked data would shadow it.
    // Refused here rather than at link time, because a mod that can never be
    // linked is a mod that can never be built.
    if (isReservedWorkDriveName(prefix)) {
        if (reason)
            *reason = QStringLiteral("The work drive already uses %1 for the game's "
                                     "own data. Pick another name.").arg(prefix);
        return false;
    }
    return true;
}

ModTemplateResult scaffoldMod(const QString &parentDir, const ModTemplateOptions &options)
{
    ModTemplateResult result;

    QString reason;
    if (!isValidModPrefix(options.prefix, &reason)) {
        result.error = reason;
        return result;
    }
    if (parentDir.trimmed().isEmpty()) {
        result.error = QStringLiteral("Choose a folder to create the mod in.");
        return result;
    }

    QString templateRoot;
    if (!modTemplateAvailable(&templateRoot)) {
        result.error = QStringLiteral("The bundled mod template is missing. Expected it "
                                      "at resources/mod-template next to the app.");
        return result;
    }

    const QString prefix = options.prefix;
    const QByteArray prefixBytes = prefix.toUtf8();
    const QString modRoot = QDir::cleanPath(QDir(parentDir).absoluteFilePath(prefix));

    const QFileInfo targetInfo(modRoot);
    if (targetInfo.exists() && !targetInfo.isDir()) {
        result.error = QStringLiteral("%1 already exists and is a file.").arg(modRoot);
        return result;
    }
    const bool rootExisted = targetInfo.isDir();
    if (rootExisted
        && !QDir(modRoot).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                  | QDir::System)) {
        result.error = QStringLiteral("%1 already exists and is not empty. Pick another "
                                      "name or another folder.").arg(modRoot);
        return result;
    }
    // Scaffolding into the template would rewrite the template itself, and
    // every mod made after it would carry this prefix.
    if (modRoot.compare(templateRoot, Qt::CaseInsensitive) == 0
        || modRoot.startsWith(templateRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
        result.error = QStringLiteral("That folder is inside the bundled template.");
        return result;
    }

    // Which maps come along. Missions are the bulk of the template, so the
    // default is none of them; an empty map list with missions turned on reads
    // as everything the template ships, which is what the checkbox alone says.
    QStringList missionFolders;
    for (const QString &name :
         QDir(templateRoot + QStringLiteral("/Missions"))
             .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString map = missionMap(name);
        if (!map.isEmpty()) missionFolders << map;
    }
    QStringList wantedMaps = options.maps;
    if (options.includeMissions && wantedMaps.isEmpty()) wantedMaps = missionFolders;

    QStringList skipped;
    if (!options.includeMissions) {
        skipped << QStringLiteral("Missions (not requested)");
    } else {
        for (const QString &map : wantedMaps) {
            if (!containsCI(missionFolders, map))
                skipped << QStringLiteral("Missions/%1.%2 (not in the template)")
                               .arg(prefix, map);
        }
    }

    // Everything is planned before anything is written, so a bad path or an
    // unreadable template file is caught while the target folder is still empty.
    const QDir templateDir(templateRoot);
    QVector<PlanItem> plan;
    QStringList copiedMaps;
    QDirIterator it(templateRoot,
                    QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString rel = templateDir.relativeFilePath(source);
        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        if (parts.first() == QLatin1String("Missions")) {
            if (!options.includeMissions) continue;
            const QString map = missionMap(parts.value(1));
            if (!map.isEmpty()) {
                if (!containsCI(wantedMaps, map)) {
                    const QString note = QStringLiteral("Missions/%1.%2 (map not selected)")
                                             .arg(prefix, map);
                    if (!skipped.contains(note)) skipped << note;
                    continue;
                }
                if (!containsCI(copiedMaps, map)) copiedMaps << map;
            }
        }

        PlanItem item;
        item.source = source;
        item.relative = applyToken(rel, prefix);
        item.rewrite = isRewritable(QFileInfo(rel).suffix());
        plan.push_back(item);
    }
    if (plan.isEmpty()) {
        result.error = QStringLiteral("The bundled mod template at %1 is empty.")
                           .arg(templateRoot);
        return result;
    }
    std::sort(plan.begin(), plan.end(), [](const PlanItem &a, const PlanItem &b) {
        return a.relative < b.relative;
    });

    QString err;
    if (!ensureDir(modRoot, &err)) {
        result.error = err;
        return result;
    }

    // From here on the folder exists, so every exit takes it back out again.
    auto fail = [&](const QString &message) -> ModTemplateResult {
        QDir(modRoot).removeRecursively();
        if (rootExisted) QDir().mkpath(modRoot);
        ModTemplateResult failed;
        failed.error = message;
        return failed;
    };

    QStringList created;
    for (const PlanItem &item : plan) {
        const QString dest = modRoot + QLatin1Char('/') + item.relative;
        if (!ensureDir(QFileInfo(dest).absolutePath(), &err)) return fail(err);
        if (item.rewrite) {
            QByteArray bytes;
            if (!readAll(item.source, bytes, &err)) return fail(err);
            if (!writeAll(dest, rewriteToken(bytes, prefixBytes), &err)) return fail(err);
        } else if (!copyFile(item.source, dest, &err)) {
            return fail(err);
        }
        created << item.relative;
    }

    // The folders Init.ps1 creates empty. Git drops an empty folder on clone,
    // so each gets a .gitkeep and the mod survives being shared.
    const QStringList keepDirs = {
        QStringLiteral("%1/Scripts/1_Core/%1").arg(prefix),
        QStringLiteral("%1/Scripts/3_Game/%1").arg(prefix),
        QStringLiteral("%1/Scripts/4_World/%1").arg(prefix),
        QStringLiteral("%1/Scripts/5_Mission/%1").arg(prefix),
        QStringLiteral("Addons"),
        QStringLiteral("Missions/Global"),
        QStringLiteral("Profiles/Dev"),
        QStringLiteral("Profiles/Global"),
    };
    for (const QString &dir : keepDirs) {
        const QString full = modRoot + QLatin1Char('/') + dir;
        if (!ensureDir(full, &err)) return fail(err);
        const QString keep = dir + QStringLiteral("/.gitkeep");
        if (!writeAll(modRoot + QLatin1Char('/') + keep, QByteArray(), &err))
            return fail(err);
        created << keep;
    }

    // What the dialog collected. The template ships these empty, and a mod with
    // no name shows up in the launcher as a blank row.
    const QString configRel = QStringLiteral("%1/Scripts/config.cpp").arg(prefix);
    const QString configPath = modRoot + QLatin1Char('/') + configRel;
    if (QFile::exists(configPath)) {
        QByteArray bytes;
        if (!readAll(configPath, bytes, &err)) return fail(err);
        QString text = QString::fromUtf8(bytes);
        const int mods = text.indexOf(QLatin1String("class CfgMods"));
        if (mods >= 0) {
            const QString display = options.displayName.trimmed().isEmpty()
                                        ? prefix
                                        : options.displayName;
            setConfigField(text, mods, QStringLiteral("name"), display);
            setConfigField(text, mods, QStringLiteral("author"), options.author);
        } else {
            skipped << QStringLiteral("%1 (no CfgMods block to fill in)").arg(configRel);
        }

        // The template hardcodes `class MT_Scripts` in CfgPatches and its own
        // Init.ps1 leaves it alone, so every mod built from it declares the
        // same patch class and two of them collide at load. The token pass
        // cannot catch this one because the name does not contain the token.
        static const QRegularExpression patchClass(
            QStringLiteral("(class\\s+CfgPatches\\s*\\{\\s*class\\s+)MT_Scripts\\b"));
        const QRegularExpressionMatch patch = patchClass.match(text);
        if (patch.hasMatch()) {
            text.replace(patch.capturedStart(), patch.capturedLength(),
                         patch.captured(1) + prefix + QStringLiteral("_Scripts"));
        }
        if (!writeAll(configPath, text.toUtf8(), &err)) return fail(err);
    } else {
        skipped << QStringLiteral("%1 (not in the template)").arg(configRel);
    }

    const QString gprojRel = QStringLiteral("%1/Workbench/dayz.gproj").arg(prefix);
    const QString gprojPath = modRoot + QLatin1Char('/') + gprojRel;
    if (QFile::exists(gprojPath)) {
        QByteArray bytes;
        if (!readAll(gprojPath, bytes, &err)) return fail(err);
        QString text = QString::fromUtf8(bytes);
        setGprojField(text, QStringLiteral("ID"), prefix);
        setGprojField(text, QStringLiteral("TITLE"), prefix);
        if (!writeAll(gprojPath, text.toUtf8(), &err)) return fail(err);
    } else {
        skipped << QStringLiteral("%1 (not in the template)").arg(gprojRel);
    }

    // The three vanilla economy blobs are 232 MB between them and are not
    // bundled, so they are pulled from a full template only when asked for.
    if (!options.extraMissionSource.trimmed().isEmpty()) {
        for (const QString &map : copiedMaps) {
            const QString destRel = QStringLiteral("Missions/%1.%2").arg(prefix, map);
            const QString source = findMissionSource(options.extraMissionSource, map);
            if (source.isEmpty()) {
                skipped << QStringLiteral("%1: no %2 mission under %3")
                               .arg(destRel, map, options.extraMissionSource);
                continue;
            }
            const QDir sourceDir(source);
            QStringList names;
            for (const QString &exact : { QStringLiteral("areaflags.map"),
                                          QStringLiteral("mapgroupproto.xml") }) {
                if (sourceDir.exists(exact)) names << exact;
                else skipped << QStringLiteral("%1/%2 (not in %3)")
                                    .arg(destRel, exact, source);
            }
            const QStringList clusters =
                sourceDir.entryList({ QStringLiteral("mapgroupcluster*.xml") }, QDir::Files);
            if (clusters.isEmpty())
                skipped << QStringLiteral("%1/mapgroupcluster*.xml (not in %2)")
                               .arg(destRel, source);
            names << clusters;

            for (const QString &name : names) {
                const QString rel = destRel + QLatin1Char('/') + name;
                const QString dest = modRoot + QLatin1Char('/') + rel;
                if (!ensureDir(QFileInfo(dest).absolutePath(), &err)) return fail(err);
                if (!copyFile(sourceDir.absoluteFilePath(name), dest, &err)) return fail(err);
                created << rel;
            }
        }
    }

    created.sort();
    result.ok = true;
    result.modRoot = modRoot;
    result.modFolder = QStringLiteral("%1/%2").arg(modRoot, prefix);
    result.scriptsRoot = QStringLiteral("%1/%2/Scripts").arg(modRoot, prefix);
    result.created = created;
    result.skipped = skipped;

    // The junction goes on the folder holding Workbench\dayz.gproj, which is
    // the same folder SetupWorkdrive.bat keys on and the same one the test
    // dock's checklist compares against.
    //
    // Deliberately after result.ok. A drive that is not mounted, or a folder
    // already sitting at P:\<prefix>, is a warning about the work drive and not
    // a reason to unwind a mod folder that came out complete.
    if (options.linkWorkDrive) {
        result.workDrive =
            linkModFolder(workDriveLinkFor(result.modFolder, options.workDrive),
                          result.modFolder, modRoot);
    }
    return result;
}
