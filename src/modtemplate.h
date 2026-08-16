// Scaffolds a new mod from the bundled template.
//
// A DayZ mod is a folder layout before it is any code: config.cpp with the
// four script modules registered, a Workbench project, a stringtable, and a
// mission per map. Getting that wrong costs an afternoon, so the app lays it
// down rather than asking the user to remember it.
//
// The template lives in resources/mod-template with "ModTemplate" as the token
// that gets replaced by the mod's prefix, the same convention its own Init.ps1
// uses, so a folder scaffolded here is identical to one scaffolded by hand.
#pragma once

#include "workdrive.h"

#include <QString>
#include <QStringList>

struct ModTemplateOptions {
    QString prefix;      // "SUDO_Link", replaces the ModTemplate token
    QString displayName; // shown in the launcher
    QString author;
    // Missions are the bulk of the template and most mods do not ship one.
    bool includeMissions = false;
    QStringList maps;    // "ChernarusPlus", "Enoch", "sakhal"
    // The vanilla economy blobs (areaflags.map, mapgroupcluster) are not
    // bundled; point this at a full template or a mission folder to copy them.
    QString extraMissionSource;

    // A mod folder that is not junctioned to P:\<Name> cannot be built, so the
    // link is part of creating the mod rather than a button to find later. On
    // by default for that reason; a caller that only wants files turns it off.
    bool linkWorkDrive = true;
    // Which drive. Empty means the real work drive, which is the only one the
    // tools resolve against. A test points this at a folder of its own so
    // nothing under the real P: is touched.
    QString workDrive;
};

struct ModTemplateResult {
    bool ok = false;
    QString error;
    QString modRoot;      // the folder that was created
    QString modFolder;    // <root>/<prefix>, the folder P:\<prefix> points at
    QString scriptsRoot;  // <root>/<prefix>/Scripts, where generated .c files go
    QStringList created;  // every file written, for the report
    QStringList skipped;

    // What happened to P:\<prefix>. Never fatal: a mod with no link is still a
    // mod, and the drive is commonly unmounted on a fresh boot. It is reported
    // rather than swallowed, because the link changes what AddonBuilder packs
    // and a silent junction is nearly as bad as none.
    //
    // A folder already sitting at that path is never moved here. Whether it can
    // be moved is workDrive.link.canMoveAside(), and asking is the caller's job.
    WorkDriveAction workDrive;
};

// True when the bundled template is present and readable.
bool modTemplateAvailable(QString *pathOut = nullptr);

// Creates the mod folder under `parentDir`, then junctions it to the work
// drive. Refuses to touch a non-empty target: overwriting somebody's mod is not
// a recoverable mistake, and the same rule holds on the work drive, where a
// folder already at P:\<prefix> is reported and left exactly as it was.
ModTemplateResult scaffoldMod(const QString &parentDir,
                              const ModTemplateOptions &options);

// Prefix rules DayZ actually enforces, checked before anything is written.
bool isValidModPrefix(const QString &prefix, QString *reason = nullptr);
