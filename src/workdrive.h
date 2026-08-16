// P:\<Name>, and what is allowed to happen to it.
//
// Binarize, AddonBuilder and Workbench resolve every path through the work
// drive, so a mod folder with no P:\<Name> junction cannot be built at all.
// The template's own SetupWorkdrive.bat makes those links, keyed on the child
// that carries Workbench\dayz.gproj. This is that rule in code, so a mod
// scaffolded by the app lands linked rather than leaving the user to discover
// months later that a button they never pressed was the missing step.
//
// Two halves, and the split is the safety property. inspect reads the disk and
// decides; it writes nothing and can be called as often as anybody likes. The
// two act functions each do the one thing their name says. Nothing here
// deletes: a junction is a real change to somebody's disk and the folder in the
// way may be the only copy of a day's work. The one function that moves a
// folder refuses unless it has compared that folder against the mod folder and
// found nothing in it that the mod folder does not already have.
//
// This sits in the Core tier because the scaffolder does. testrun.cpp's
// makeJunction is the same operation one tier up, where Qt Widgets is already
// linked; it should forward here rather than keep a second copy of the rules.
#pragma once

#include <QString>
#include <QStringList>

// What is at P:\<Name> right now. Every value has one message and one next
// step, which is the whole decision table.
enum class WorkDriveState {
    NoModFolder,     // the mod folder is not there, so there is nothing to link
    NameReserved,    // P:\DZ and friends: taking one shadows the game's own data
    DriveMissing,    // the work drive is not mounted
    // The link path and the mod folder are the same folder, or one is inside
    // the other. A project created on the work drive root lands here: P:\<Name>
    // is then the project folder itself and cannot also be a link to the mod
    // folder inside it. It is the one state where doing nothing is the only
    // safe move, because the folder in the way is the work.
    Overlapping,
    Linked,          // a junction already pointing at this mod folder
    LinkedElsewhere, // a junction pointing at some other folder
    NotLinked,       // nothing at that path, so the link can be made
    FolderIsCopy,    // a real folder holding nothing the mod folder lacks
    FolderHasOwn,    // a real folder carrying files of its own
    FolderUnchecked, // a real folder too large to compare, so it is left alone
    RealFile,        // a file, not a folder
};

struct WorkDriveLink {
    WorkDriveState state = WorkDriveState::NoModFolder;
    QString link;    // "P:\SUDO_Link"
    QString target;  // the mod folder it should point at
    QString pointsAt; // where an existing junction points, LinkedElsewhere only

    // The comparison behind FolderIsCopy and FolderHasOwn. `unique` names what
    // the folder holds that nothing of the project's does, capped so a wrong
    // folder cannot print a thousand lines; `files` is how many were looked at,
    // `matched` how many were found at the same size, and `compared` how many
    // of those were read byte for byte rather than trusted on their size.
    QStringList unique;
    int uniqueTotal = 0;
    int files = 0;
    int matched = 0;
    int compared = 0;
    // Which folder it turned out to be a copy of. The junction points at the
    // mod folder, but a folder already in the way is just as often a copy of
    // the whole project, and neither one loses anything by being renamed.
    QString copyOf;

    bool linked() const { return state == WorkDriveState::Linked; }
    // Nothing is at risk, so the app may act without asking.
    bool canLink() const { return state == WorkDriveState::NotLinked; }
    // A rename would clear the way, and the user has to agree to it first.
    bool canMoveAside() const { return state == WorkDriveState::FolderIsCopy; }

    // One sentence saying what is there. Sentence case, no full stop after a
    // trailing path, because a path that ends in one reads as part of the path.
    QString message() const;
    // The next thing to do about it, empty when nothing is needed.
    QString fix() const;
};

// What an attempt did. `command` and `output` are kept whether or not it
// worked: a link that failed with nothing on screen is the thing that costs an
// evening.
struct WorkDriveAction {
    bool ok = false;
    WorkDriveLink link;  // re-read after the attempt, so it is what is true now
    QString movedTo;     // where a real folder was renamed to, when one was
    QString command;     // the mklink line that ran
    QString output;      // what it printed
    QString error;       // why it did not work

    // False when nothing was tried, which is how a caller that turned linking
    // off is told apart from one whose link failed.
    bool attempted() const { return !link.link.isEmpty(); }
};

// The letter the tools fix. Binarize resolves against it and the template's own
// scripts hardcode it, so this is not a preference.
QString workDriveRoot();

// Names the work drive already uses for the game's unpacked data and for the
// tools' own folders. A mod taking one of these does not fail loudly, it
// shadows vanilla data, so it is refused before anything is written.
bool isReservedWorkDriveName(const QString &name);

// P:\<folder name>. `drive` empty means the real work drive; a test passes a
// folder of its own so nothing under the real P: is touched.
QString workDriveLinkFor(const QString &modFolder, const QString &drive = QString());

// Reads the disk and decides. Writes nothing.
//
// `alsoKnown` is a second folder whose content would be just as safe to rename:
// pass the project root, because a folder sitting at P:\<Name> is as often a
// copy of the whole project as of the mod folder inside it, and calling that
// one "content of its own" turns a one click fix into a dead end.
WorkDriveLink inspectWorkDriveLink(const QString &link, const QString &target,
                                   const QString &alsoKnown = QString());

// The same for a mod folder that already exists: what an open, a clone, or a
// folder that moved all arrive at. A project made before any of this existed
// lands here too.
WorkDriveLink inspectModFolder(const QString &modFolder,
                               const QString &drive = QString(),
                               const QString &projectRoot = QString());

// Makes the junction. Does nothing at all unless the state is NotLinked, and
// reports the state it refused on. An existing junction already pointing at the
// mod folder is a success that runs nothing.
WorkDriveAction linkModFolder(const QString &link, const QString &target,
                              const QString &alsoKnown = QString());

// Renames the folder in the way, then links. Refuses unless inspect says
// FolderIsCopy, so a folder with anything of its own in it can never be moved
// by this. Nothing is deleted either way: the folder is renamed and the new
// name comes back in `movedTo` so the user can find it.
WorkDriveAction moveAsideAndLinkModFolder(const QString &link, const QString &target,
                                          const QString &alsoKnown = QString());

// The name a move would use, so a dialog can say where the folder is going
// before the user agrees to it.
QString asideNameFor(const QString &link);
