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
};

struct ModTemplateResult {
    bool ok = false;
    QString error;
    QString modRoot;      // the folder that was created
    QString scriptsRoot;  // <root>/<prefix>/Scripts, where generated .c files go
    QStringList created;  // every file written, for the report
    QStringList skipped;
};

// True when the bundled template is present and readable.
bool modTemplateAvailable(QString *pathOut = nullptr);

// Creates the mod folder under `parentDir`. Refuses to touch a non-empty
// target: overwriting somebody's mod is not a recoverable mistake.
ModTemplateResult scaffoldMod(const QString &parentDir,
                              const ModTemplateOptions &options);

// Prefix rules DayZ actually enforces, checked before anything is written.
bool isValidModPrefix(const QString &prefix, QString *reason = nullptr);
