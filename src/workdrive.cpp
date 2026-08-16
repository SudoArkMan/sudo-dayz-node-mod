#include "workdrive.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace {

// Everything DayZ Tools unpacks to the root of the work drive, plus the two
// folders the tools and this app write there. A mod called "scripts" would sit
// on top of the vanilla script tree and binarize would resolve against the mod
// instead of the game, which is a failure that looks like the game changing
// under you.
const char *const kReserved[] = {
    "DZ", "Core", "Mods", "temp", "bin", "graphics", "gui", "languagecore",
    "scripts", "system", "Buldozer",
};

// The walk behind "this is a copy of your project" has to end. A mod folder is
// a few thousand files; past this the answer is that it was not checked, which
// is a different thing from it being safe to move.
constexpr int kFileCeiling = 50000;

// Sizes alone would let a folder of same-length placeholders pass as a copy, so
// the files are read as well. Both bounds exist so a mission folder carrying a
// 200 MB areaflags.map cannot turn a dialog into a stall; anything past them
// falls back to matching on size and says how many were really read.
constexpr qint64 kCompareFileCeiling = 8ll * 1024 * 1024;
constexpr qint64 kCompareBudget = 128ll * 1024 * 1024;

// Long enough for cmd.exe to start on a cold cache, short enough that a wedged
// console does not hold the dialog open.
constexpr int kTimeoutMs = 15000;

// How many unique names go in the message. The rest are counted, because a
// folder that is nothing like the mod prints hundreds and the point of the
// line is that the folder is not ours to touch.
constexpr int kNamesShown = 5;

QString clean(const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

QString rootOf(const QString &link)
{
    return clean(QFileInfo(link).absolutePath());
}

// Two paths naming the same folder, or one holding the other. Junctioning
// either way round is nonsense, and treating the outer one as a folder in the
// way would offer to rename the project the user is standing in.
bool overlaps(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a.compare(b, Qt::CaseInsensitive) == 0) return true;
    return a.startsWith(b + QLatin1Char('/'), Qt::CaseInsensitive)
           || b.startsWith(a + QLatin1Char('/'), Qt::CaseInsensitive);
}

QString countOf(int n, const QString &noun)
{
    return QStringLiteral("%1 %2%3").arg(n).arg(noun, n == 1 ? QString()
                                                             : QStringLiteral("s"));
}

bool sameBytes(const QString &a, const QString &b)
{
    QFile fa(a);
    QFile fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly)) return false;
    for (;;) {
        const QByteArray ba = fa.read(64 * 1024);
        const QByteArray bb = fb.read(64 * 1024);
        if (ba != bb) return false;
        if (ba.isEmpty()) return true;
    }
}

struct FolderDiff {
    int files = 0;
    int matched = 0;
    int compared = 0;
    int uniqueTotal = 0;
    QStringList unique;
    bool complete = true;
};

// Every file under `folder` that `against` does not also have at the same size
// and, where the budget reaches, the same bytes. Hidden and system entries
// count: a .git or a desktop.ini in there is still something of the user's that
// no scaffold ever wrote.
FolderDiff compareFolders(const QString &folder, const QString &against)
{
    FolderDiff diff;
    const QDir from(folder);
    const QDir to(against);
    qint64 budget = kCompareBudget;

    QDirIterator it(folder,
                    QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (++diff.files > kFileCeiling) {
            diff.complete = false;
            return diff;
        }
        const QString rel = from.relativeFilePath(path);
        const QFileInfo mine(path);
        const QFileInfo theirs(to.filePath(rel));

        bool same = theirs.isFile() && theirs.size() == mine.size();
        if (same && mine.size() <= kCompareFileCeiling && budget > 0) {
            same = sameBytes(path, theirs.absoluteFilePath());
            budget -= mine.size();
            if (same) diff.compared++;
        }
        if (same) {
            diff.matched++;
            continue;
        }
        diff.uniqueTotal++;
        if (diff.unique.size() < kNamesShown) diff.unique << rel;
    }
    return diff;
}

void runMkLink(const QString &link, const QString &target, WorkDriveAction *action)
{
    // mklink is a cmd builtin, so there is no executable to call directly.
    const QStringList args = { QStringLiteral("/c"), QStringLiteral("mklink"),
                               QStringLiteral("/J"), native(link), native(target) };
    action->command = QStringLiteral("cmd.exe /c mklink /J \"%1\" \"%2\"")
                          .arg(native(link), native(target));

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(QStringLiteral("cmd.exe"), args);
    if (!proc.waitForStarted(kTimeoutMs)) {
        action->error = QStringLiteral("Could not start cmd.exe: %1")
                            .arg(proc.errorString());
        return;
    }
    if (!proc.waitForFinished(kTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        action->output = QString::fromLocal8Bit(proc.readAll()).trimmed();
        action->error = QStringLiteral("mklink did not finish within %1 seconds.")
                            .arg(kTimeoutMs / 1000);
        return;
    }
    action->output = QString::fromLocal8Bit(proc.readAll()).trimmed();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        action->error = QStringLiteral("mklink exited with code %1.")
                            .arg(proc.exitCode());
}

} // namespace

// ------------------------------------------------------------------- the rules

QString workDriveRoot()
{
    return QStringLiteral("P:/");
}

bool isReservedWorkDriveName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    for (const char *reserved : kReserved)
        if (trimmed.compare(QLatin1String(reserved), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QString workDriveLinkFor(const QString &modFolder, const QString &drive)
{
    const QString name = QFileInfo(clean(modFolder)).fileName();
    if (name.isEmpty()) return QString();
    const QString root = drive.trimmed().isEmpty() ? workDriveRoot() : clean(drive);
    return clean(QDir(root).filePath(name));
}

// ---------------------------------------------------------------- the decision

WorkDriveLink inspectWorkDriveLink(const QString &link, const QString &target,
                                   const QString &alsoKnown)
{
    WorkDriveLink out;
    out.link = clean(link);
    out.target = clean(target);
    out.copyOf = out.target;

    // The name is wrong whatever the disk says, so it is answered first.
    if (isReservedWorkDriveName(QFileInfo(out.link).fileName())) {
        out.state = WorkDriveState::NameReserved;
        return out;
    }
    if (out.target.isEmpty() || !QFileInfo(out.target).isDir()) {
        out.state = WorkDriveState::NoModFolder;
        return out;
    }
    if (out.link.isEmpty() || !QFileInfo(rootOf(out.link)).isDir()) {
        out.state = WorkDriveState::DriveMissing;
        return out;
    }
    // Decided from the two paths alone, before anything on disk is read. A
    // project created on the work drive root reaches this, and every branch
    // below it would be wrong about it: the folder at the link path is the
    // project, so comparing it against the project finds a perfect copy and
    // offers to rename the user's work out from under them.
    if (overlaps(out.link, out.target)) {
        out.state = WorkDriveState::Overlapping;
        return out;
    }

    const QFileInfo linkInfo(out.link);
    if (linkInfo.isJunction()) {
        out.pointsAt = clean(linkInfo.junctionTarget());
        out.state = out.pointsAt.compare(out.target, Qt::CaseInsensitive) == 0
                        ? WorkDriveState::Linked
                        : WorkDriveState::LinkedElsewhere;
        return out;
    }
    if (!linkInfo.exists()) {
        out.state = WorkDriveState::NotLinked;
        return out;
    }
    if (!linkInfo.isDir()) {
        out.state = WorkDriveState::RealFile;
        return out;
    }

    FolderDiff diff = compareFolders(out.link, out.target);
    // Something copied the whole project to P:\<Name> rather than linking it,
    // and against the mod folder alone that reads as a folder full of unique
    // work. Checking the project root too is what keeps the answer honest, and
    // the answer decides whether a rename is offered.
    const QString second = clean(alsoKnown);
    if (diff.complete && diff.uniqueTotal > 0 && !second.isEmpty()
        && second.compare(out.target, Qt::CaseInsensitive) != 0
        && !overlaps(second, out.link) && QFileInfo(second).isDir()) {
        const FolderDiff other = compareFolders(out.link, second);
        if (other.complete && other.uniqueTotal == 0) {
            diff = other;
            out.copyOf = second;
        }
    }

    out.files = diff.files;
    out.matched = diff.matched;
    out.compared = diff.compared;
    out.uniqueTotal = diff.uniqueTotal;
    out.unique = diff.unique;
    if (!diff.complete)
        out.state = WorkDriveState::FolderUnchecked;
    else if (diff.uniqueTotal == 0)
        out.state = WorkDriveState::FolderIsCopy;
    else
        out.state = WorkDriveState::FolderHasOwn;
    return out;
}

WorkDriveLink inspectModFolder(const QString &modFolder, const QString &drive,
                               const QString &projectRoot)
{
    return inspectWorkDriveLink(workDriveLinkFor(modFolder, drive), modFolder,
                                projectRoot);
}

// ------------------------------------------------------------------ in words

QString WorkDriveLink::message() const
{
    const QString l = native(link);
    const QString t = native(target);
    switch (state) {
    case WorkDriveState::NoModFolder:
        return QStringLiteral("%1 is not a folder, so there is nothing to link.").arg(t);
    case WorkDriveState::NameReserved:
        return QStringLiteral("%1 is one of the work drive's own folders, so a mod "
                              "cannot take that name.").arg(l);
    case WorkDriveState::DriveMissing:
        return QStringLiteral("%1 is not mounted, so the mod was not linked to the "
                              "work drive.").arg(native(rootOf(link)));
    case WorkDriveState::Overlapping:
        if (link.compare(target, Qt::CaseInsensitive) == 0)
            return QStringLiteral("%1 is the mod folder itself, so it cannot also be "
                                  "a link to it.").arg(l);
        if (target.startsWith(link + QLatin1Char('/'), Qt::CaseInsensitive))
            return QStringLiteral("%1 is the folder the mod was created in and %2 is "
                                  "inside it, so it cannot also be a link to it.")
                .arg(l, t);
        return QStringLiteral("%1 is inside %2, so it cannot be a link to it.")
            .arg(l, t);
    case WorkDriveState::Linked:
        return QStringLiteral("%1 points at %2").arg(l, t);
    case WorkDriveState::LinkedElsewhere:
        return QStringLiteral("%1 already points at %2, not at %3.")
            .arg(l, native(pointsAt), t);
    case WorkDriveState::NotLinked:
        return QStringLiteral("%1 is not linked yet.").arg(l);
    case WorkDriveState::FolderIsCopy:
        // Files only, because that is all the walk counts. A folder holding
        // nothing but empty folders reaches here too, and calling that one
        // empty is a claim the user would agree to a rename on.
        if (files == 0)
            return QStringLiteral("%1 is a real folder with no files in it.").arg(l);
        return QStringLiteral("%1 is a real folder holding %2, and every one of them "
                              "is already in %3 at the same size, %4 of them read "
                              "byte for byte. Nothing in it is unique.")
            .arg(l, countOf(files, QStringLiteral("file")), native(copyOf))
            .arg(compared);
    case WorkDriveState::FolderHasOwn:
        return QStringLiteral("%1 is a real folder and %2 in it %3 not in %4: %5.")
            .arg(l, countOf(uniqueTotal, QStringLiteral("file")),
                 uniqueTotal == 1 ? QStringLiteral("is") : QStringLiteral("are"), t,
                 uniqueTotal > unique.size()
                     ? QStringLiteral("%1 and %2 more")
                           .arg(unique.join(QStringLiteral(", ")))
                           .arg(uniqueTotal - unique.size())
                     : unique.join(QStringLiteral(", ")));
    case WorkDriveState::FolderUnchecked:
        return QStringLiteral("%1 is a real folder with more than %2 files in it, too "
                              "many to check against %3.")
            .arg(l).arg(kFileCeiling).arg(t);
    case WorkDriveState::RealFile:
        return QStringLiteral("%1 is a real file.").arg(l);
    }
    return QString();
}

QString WorkDriveLink::fix() const
{
    switch (state) {
    case WorkDriveState::NoModFolder:
    case WorkDriveState::Linked:
        return QString();
    case WorkDriveState::NameReserved:
        return QStringLiteral("Rename the mod folder to something the work drive does "
                              "not already use.");
    case WorkDriveState::DriveMissing: {
        const QString root = rootOf(link);
        // A drive root is the case that has a one line answer. Anything else is
        // a folder somebody chose, so the answer is to make it.
        if (root.size() <= 3 && root.size() >= 2 && root.at(1) == QLatin1Char(':'))
            return QStringLiteral("Mount it from DayZ Tools, or run "
                                  "subst %1 <your work drive folder>, then set up the "
                                  "work drive.").arg(root.left(2));
        return QStringLiteral("Create %1, then set up the work drive.").arg(native(root));
    }
    case WorkDriveState::Overlapping:
        // The folder in the way is the project, so the answer is never to move
        // anything. It is to keep the project off the work drive root, which is
        // the one place its own name is already spoken for.
        return QStringLiteral("Keep the project in a folder outside the work drive "
                              "and let the link point at it from there. A project "
                              "created on the work drive root takes the name its own "
                              "link needs, and nothing here will rename it.");
    case WorkDriveState::LinkedElsewhere:
        return QStringLiteral("Remove that junction yourself if it is stale. Nothing "
                              "here deletes a link it did not make.");
    case WorkDriveState::NotLinked:
        return QStringLiteral("Set up the work drive.");
    case WorkDriveState::FolderIsCopy:
        return QStringLiteral("Move it aside and link, or move it yourself.");
    case WorkDriveState::FolderHasOwn:
    case WorkDriveState::FolderUnchecked:
    case WorkDriveState::RealFile:
        return QStringLiteral("Move or rename it yourself, then set up the work drive.");
    }
    return QString();
}

// -------------------------------------------------------------------- the acts

QString asideNameFor(const QString &link)
{
    const QString base =
        clean(link) + QStringLiteral(".aside-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString name = base;
    for (int n = 2; QFileInfo::exists(name); ++n)
        name = QStringLiteral("%1-%2").arg(base).arg(n);
    return name;
}

WorkDriveAction linkModFolder(const QString &link, const QString &target,
                              const QString &alsoKnown)
{
    WorkDriveAction action;
    action.link = inspectWorkDriveLink(link, target, alsoKnown);
    if (action.link.linked()) {
        action.ok = true;
        return action;
    }
    if (!action.link.canLink()) {
        action.error = action.link.message();
        return action;
    }

    runMkLink(action.link.link, action.link.target, &action);
    // Believed only after re-reading: mklink reports success on a path that
    // ends up as something other than a junction often enough to matter.
    action.link = inspectWorkDriveLink(link, target, alsoKnown);
    action.ok = action.link.linked();
    if (!action.ok && action.error.isEmpty())
        action.error = QStringLiteral("mklink reported success but %1 is not a "
                                      "junction.").arg(native(clean(link)));
    return action;
}

WorkDriveAction moveAsideAndLinkModFolder(const QString &link, const QString &target,
                                          const QString &alsoKnown)
{
    WorkDriveAction action;
    action.link = inspectWorkDriveLink(link, target, alsoKnown);
    if (action.link.linked()) {
        action.ok = true;
        return action;
    }
    if (!action.link.canMoveAside()) {
        // The refusal names what was found rather than saying no, because the
        // whole reason this function exists is that the folder in the way may
        // be somebody's only copy.
        action.error = QStringLiteral("%1 was not moved. %2")
                           .arg(native(action.link.link), action.link.message());
        return action;
    }

    const QString aside = asideNameFor(action.link.link);
    if (!QDir().rename(action.link.link, aside)) {
        action.error = QStringLiteral("Could not rename %1 to %2, so nothing was "
                                      "changed.")
                           .arg(native(action.link.link), native(aside));
        return action;
    }
    action.movedTo = aside;

    runMkLink(action.link.link, action.link.target, &action);
    action.link = inspectWorkDriveLink(link, target, alsoKnown);
    action.ok = action.link.linked();
    if (!action.ok && action.error.isEmpty())
        action.error = QStringLiteral("mklink reported success but %1 is not a "
                                      "junction.").arg(native(clean(link)));
    return action;
}
