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
//   deps         calls into another mod: addon not required, optional use left
//                unguarded, a dependency's own requirements not met
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
#include "moddeps.h"

#include <QString>
#include <QStringList>
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

// What the dependency rules are checked against: the mods this project is
// written on top of, and what its own config.cpp already asks the engine to
// load. Default constructed means "no dependencies", and under it all three
// rules report nothing, so a caller that predates them sees no change.
//
// Community Online Tools is the worked example throughout, because it is the
// dependency most DayZ mods carry: addon JM_COT_Scripts, which in turn needs
// JM_CF_Scripts (Community Framework), and which defines JM_COT_LOADED so a mod
// can compile with or without it.
struct DependencyContext {
    QVector<ModDependency> deps;   // Project::dependencies, verbatim
    // requiredAddons as this mod's own config.cpp lists them, unquoted. Analysis
    // opens no files: it re-runs on every graph edit, so the caller reads the
    // config once through readAddonFacts() in moddeps.h, which parses it with
    // src/config/configtree.h, and hands its `requires` list over. Entries that
    // still carry their quotes are tolerated either way.
    QStringList declaredAddons;
    QString patchClass;   // CfgPatches class those came from, "MyMod_Scripts"
    QString configPath;   // named in the hint, so a finding points at a file
    // False means config.cpp was never read, which is not the same as an empty
    // requiredAddons. Both rules that would blame the config stay quiet under
    // it, so a project with no mod folder reports nothing rather than
    // everything.
    bool configRead = false;
};

// `scriptId` is this graph's id in the owning project. It is what tells a
// `fn.call.<scriptId>.<fnId>` node aimed at another script apart from one aimed
// at this graph's own function; without it the analyser cannot know which of
// the two it is looking at and treats every call whose function id is declared
// here as local. Callers that have the id should pass it.
//
// `deps` defaults to empty, so a caller that knows nothing about dependencies
// gets exactly the findings it got before this existed.
AnalysisResult analyzeGraph(const Graph &graph, const Catalog &cat,
                            const Builtins &builtins,
                            const QString &scriptId = QString(),
                            const DependencyContext &deps = DependencyContext());
