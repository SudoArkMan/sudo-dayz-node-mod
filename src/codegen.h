// Graph -> Enforce Script.
//
// Event nodes become overridden methods. Each event's exec chain is walked to
// statements; data pins resolve to expressions, inlining pure nodes and
// referencing temporaries for impure calls that produce values.
//
// Regions between USER_BEGIN/USER_END markers survive regeneration, so
// hand-written helpers can live alongside generated code.
#pragma once

#include "builtins.h"
#include "catalog.h"
#include "graph.h"
#include "project.h"

#include <QString>
#include <QStringList>

extern const QString USER_BEGIN;
extern const QString USER_END;

struct GenResult {
    QString code;
    QStringList warnings;
    // Which node produced each line of `code`, indexed the same way as
    // code.split('\n'). Empty where a line belongs to no single node (the
    // class header, a member declaration, the user region). The live code view
    // uses this to jump between a line and the node behind it.
    //
    // lineOwners.size() == code.split('\n').size(), trailing newline included:
    // `code` ends with one, so the split ends with an empty element, and that
    // element gets an entry here too.
    QVector<QString> lineOwners;

    // Node that produced a given line, or an empty string.
    QString ownerOfLine(int line) const
    {
        return line >= 0 && line < lineOwners.size() ? lineOwners.at(line) : QString();
    }
};

// Generates the .c file body for one script. `project` is passed so calls
// into other scripts in the same project resolve; `previous` is the existing
// file contents when regenerating, so user regions are carried over.
GenResult generateEnforce(const Graph &graph, const Catalog &cat,
                          const Builtins &builtins, const Project &project,
                          const QString &previous = QString());

// One .c file out of the classes that belong in it.
//
// generateEnforce answers for a single class. A file can hold several, and it
// can hold a preamble: an enum, a global function, a #define, anything the
// graph has no room for. The lead-in and the blank line between two classes are
// this function's own lines, so it is the only thing that can put the file's
// ending on them. Everything upstream works in bare newlines.
//
// `eol` is the ending the file was read with, from `Graph::eol`. An empty
// string means the source mixed the two, which has no answer that reproduces
// it, so the text is left on bare newlines.
QString assembleScriptFile(const QStringList &classes, const QString &preamble,
                           const QString &eol);
