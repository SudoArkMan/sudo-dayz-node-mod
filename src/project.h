// .sdzn project file: a set of scripts organised into module folders.
//
// One project maps to one mod's script tree: every script becomes a .c file
// under 3_Game / 4_World / 5_Mission when the project is exported.
#pragma once

#include "graph.h"
#include "moddeps.h"

#include <QString>
#include <QVector>

struct ScriptEntry {
    QString id;
    QString name;      // class/file name
    QString folder;    // "4_World/SUDO_Link"
    Graph graph;
    // The .c this script was imported from, when it came from one. Saving and
    // exporting write back here instead of asking for a folder, so a graph
    // opened out of the mod folder returns to the file it was read from. Held
    // absolute and written to the .sdzn relative to the project file, the way
    // modRoot is, so moving the folder does not strand the link.
    QString sourcePath;
    // What sat outside the class in that file: enums, globals, a #define. The
    // generator writes the class and nothing else, so this is kept here and put
    // back in front of it, or the first regenerate deletes it.
    QString preamble;
    QJsonObject extra;
};

// What shape of .sdzn this build writes.
//
//   1  the shape the Electron build wrote, and every build up to the one that
//      taught a node to carry the indentation, blank lines and comments a
//      method was written with
//   2  fmt.base, fmt.unit, fmt.eol and the trivia.* keys on nodes
//
// A file with no version field is version 1: nothing before this wrote one.
// That direction is safe: a v1 file has no layout keys, and every body
// regenerates the way the build that wrote it regenerated them. The other
// direction is the one that costs an author their formatting, and it is why the
// field exists at all: a v2 file opened by a v1 build comes back with tab
// indentation and no comments, and until there is a number in the file nothing
// can tell the user that is about to happen.
constexpr int kProjectFormatVersion = 2;

struct Project {
    QString name = QStringLiteral("Untitled");
    // The version the file claimed, kept so saving never quietly relabels a
    // file written by a newer build as one of ours.
    int formatVersion = kProjectFormatVersion;
    QStringList folders;
    QVector<ScriptEntry> scripts;
    QString activeId;
    QJsonObject extra;
    QString path;      // file it was loaded from, empty for new projects
    // Mod folder this project belongs to, when it was scaffolded from the
    // template. Export writes into <modRoot>/<prefix>/Scripts and the Mod
    // Explorer roots itself here. Saved in the .sdzn as a relative path so a
    // project folder can be moved or shared without breaking.
    QString modRoot;
    QString modPrefix;
    // Mods this one is written against: Community Framework, Community Online
    // Tools, or anything else the user pointed the tool at. Each one's
    // scriptRoot is saved relative to the .sdzn the way modRoot is, so a
    // project folder can be moved without stranding the link, and a project
    // opened on a machine that does not have the mod installed still knows
    // what it depends on.
    QVector<ModDependency> dependencies;

    ScriptEntry *script(const QString &id);
    const ScriptEntry *script(const QString &id) const;
    ScriptEntry *active();
    // The dependency an addon id names, or null. Enough on its own to draw a
    // node's badge: ModIndex::dependencyIdOf reads the id out of the node's
    // catalogue key, and this turns it into a name and a colour.
    const ModDependency *dependency(const QString &id) const;
};

// Both return false and set `error` on failure; the JSON shape matches the
// Electron build exactly, so projects move between the two.
bool loadProject(const QString &path, Project &out, QString *error = nullptr);
bool saveProject(const Project &project, const QString &path, QString *error = nullptr);

Project newProject();
