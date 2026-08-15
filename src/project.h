// .sdzn project file: a set of scripts organised into module folders.
//
// One project maps to one mod's script tree: every script becomes a .c file
// under 3_Game / 4_World / 5_Mission when the project is exported.
#pragma once

#include "graph.h"

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

struct Project {
    QString name = QStringLiteral("Untitled");
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

    ScriptEntry *script(const QString &id);
    const ScriptEntry *script(const QString &id) const;
    ScriptEntry *active();
};

// Both return false and set `error` on failure; the JSON shape matches the
// Electron build exactly, so projects move between the two.
bool loadProject(const QString &path, Project &out, QString *error = nullptr);
bool saveProject(const Project &project, const QString &path, QString *error = nullptr);

Project newProject();
