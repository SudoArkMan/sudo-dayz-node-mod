// Graph analysis: the Errors/Warnings count in the status bar and the
// squiggles on the canvas.
//
// Three rule families:
//   correctness  dangling required inputs, type mismatches, cycles, and the
//                hand-written Enforce inside Raw nodes: brace and paren
//                balance, unterminated strings and comments, names that
//                resolve to nothing
//   dayz         engine traps: modding a proto native, client/server misuse,
//                sync vars never marked dirty, super skipped on an event,
//                module load order, mutating a container mid-iteration
//   dead         unreachable nodes, orphaned pure chains, unused variables
//
// Raw nodes used to be exempt from all of it, which meant a missing brace
// shipped as a broken .c file with nothing on screen to say so. They are read
// through the Enforce tokeniser now (src/enforce/lexer.h). What it cannot see
// it stays quiet about: one graph is handed in, never the project, so a name
// sitting in a type or static-class position may belong to a sibling script
// and is never called wrong.
#pragma once

#include "builtins.h"
#include "catalog.h"
#include "graph.h"

#include <QString>
#include <QVector>

enum class Severity { Error, Warning, Info };

struct Diagnostic {
    Severity severity = Severity::Warning;
    QString message;
    QString nodeId;   // empty when the finding is about the graph as a whole
    QString pinId;
    QString rule;     // stable id, e.g. "dayz.native-override"
    QString hint;     // what to do about it
};

struct AnalysisResult {
    QVector<Diagnostic> diagnostics;
    int errors = 0;
    int warnings = 0;

    QVector<Diagnostic> forNode(const QString &nodeId) const;
    Severity worstFor(const QString &nodeId) const;
    bool hasIssue(const QString &nodeId) const;
};

// `scriptId` is this graph's id in the owning project. It is what tells a
// `fn.call.<scriptId>.<fnId>` node aimed at another script apart from one aimed
// at this graph's own function; without it the analyser cannot know which of
// the two it is looking at and treats every call whose function id is declared
// here as local. Callers that have the id should pass it.
AnalysisResult analyzeGraph(const Graph &graph, const Catalog &cat,
                            const Builtins &builtins,
                            const QString &scriptId = QString());
