// Enforce Script file to graph.
//
// This is a visual scripting tool, so opening a .c should give you the nodes,
// not a text box. The parser and the lowering already turn statements into
// nodes; what is missing is the declaration layer above them, which is what
// this reads: the class header, its members, and each method signature.
//
// Nothing is ever lost. A method body that cannot be lowered keeps its text on
// the function as `rawBody`, and a statement that cannot be lowered stays a
// Raw node, so the file regenerates as it was even where the graph could not
// model it.
#pragma once

#include "graph.h"
#include "project.h"

class Catalog;
class Builtins;

struct ImportedScript {
    QString className;
    QString baseClass;
    bool modded = false;
    Graph graph;
    // Statements that became nodes, against the total, for the report.
    int statementsLowered = 0;
    int statementsTotal = 0;
};

struct ImportResult {
    bool ok = false;
    QString error;
    // A file may declare more than one class; each becomes its own script.
    QVector<ImportedScript> scripts;
    QStringList notes;
    // Text outside any class (a global function, an enum, a #define). Kept so
    // exporting the file back does not drop it.
    QString preamble;

    int totalLowered() const;
    int totalStatements() const;
};

// Reads one .c file. Never throws; a file it cannot make sense of comes back
// with ok = false and the reason, leaving the caller to open it as text.
ImportResult importEnforceFile(const QString &path, const Catalog &cat,
                               const Builtins &builtins, const Project &project);

// Same, for text already in hand.
ImportResult importEnforceText(const QString &text, const Catalog &cat,
                               const Builtins &builtins, const Project &project);

// The module folder a path sits in ("3_Game", "4_World", "5_Mission"), or an
// empty string. Used to place an imported script in the project tree.
QString moduleForPath(const QString &path);
