// Enforce syntax tree to node graph.
//
// This is what makes the tool a visual scripting tool rather than a code
// editor with boxes: `if (!m_RestApi)` becomes a Branch fed by a Not fed by a
// Get node, and the user can rewire it without touching text.
//
// A statement that cannot be lowered falls back to a Raw node on its own,
// never taking the rest of the body down with it.
#pragma once

#include "ast.h"
#include "graph.h"

class Catalog;
class Builtins;
class Project;

struct LowerOptions {
    // Class the statements live on, so an unqualified call resolves against
    // the right ancestry.
    QString selfClass;
    // Names in scope before the block runs: event parameters, function
    // parameters, loop variables from an enclosing scope.
    QHash<QString, QString> knownLocals;  // name -> declared type
    // Where to start placing nodes.
    double originX = 0;
    double originY = 0;
    // The body these statements were parsed out of. Statements carry offsets
    // into it, so the blank lines and comments between them can be read back
    // and hung on the nodes. Empty means the caller has no text to offer, and
    // nothing is kept.
    QString sourceText;
};

struct LowerResult {
    QVector<GraphNode> nodes;
    QVector<GraphEdge> edges;
    // New locals that had to become real graph variables because they are
    // written more than once. Single-assignment locals are wired directly
    // instead, which is what keeps the graph readable.
    QVector<GraphVariable> variables;
    // First node of the exec chain, and the last, so a caller can splice the
    // result into an existing chain.
    QString entryNode;
    QString exitNode;

    // Blank lines and comments the author left between the last statement and
    // the end of the body. The caller owns the closing brace, so it is the one
    // that has to put them back.
    QString endTrivia;

    QStringList notes;     // what fell back to Raw, and why
    int statementsLowered = 0;
    int statementsRaw = 0;

    bool isEmpty() const { return nodes.isEmpty(); }
};

// Lowers parsed statements into nodes. `cat` resolves method calls against the
// vanilla API; `project` lets a call into another script in the same project
// resolve to a fn.call node.
LowerResult lowerToNodes(const std::vector<StmtPtr> &statements, const Catalog &cat,
                         const Builtins &builtins, const Graph &graph,
                         const Project &project, const LowerOptions &opts);

// Convenience: parse and lower one block of text in a single call. Used by the
// "convert to nodes" action on a Raw node.
LowerResult lowerEnforceCode(const QString &code, const Catalog &cat,
                             const Builtins &builtins, const Graph &graph,
                             const Project &project, const LowerOptions &opts);

// Replaces a Raw node with the nodes its code lowers to, splicing the new
// chain into the exec wires the raw node was carrying. Returns false and
// leaves the graph untouched when nothing could be lowered.
bool explodeRawNode(Graph &graph, const QString &nodeId, const Catalog &cat,
                    const Builtins &builtins, const Project &project,
                    QStringList *notes = nullptr);
