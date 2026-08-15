// Nodes that are not API calls: flow control, operators, literals, variable
// access, casting, and the raw-code escape hatch.
//
// These carry the parts of a program the vanilla API cannot express (an `if` is
// not a method), plus a way out when the graph is the wrong tool for a
// particular line. Builtin refs are "bi.*" ids; some are parameterised by the
// node's opts (Begin mode, cast target class, variable name).
#pragma once

#include "graph.h"

#include <QHash>
#include <QString>
#include <QVector>

// One lifecycle choice. Enforce has no single "begin play". There are four
// moments that all get used, and picking the wrong one is a classic source of
// "my code runs but nothing happens", so Begin makes the choice explicit.
struct LifecycleSig {
    QString method;      // method to override, empty for the constructor
    QString ret;
    QVector<GraphParam> params;
    bool ctor = false;
    QString label;
    QString note;
};

class Catalog;

class Builtins {
public:
    Builtins();

    // All builtin defs, for the palette.
    QVector<NodeDef> all() const;
    // Base def for a builtin id; invalid def when the id is unknown.
    NodeDef def(const QString &id) const;
    // Def with per-node options applied (Begin mode, cast class, var type).
    NodeDef defForNode(const GraphNode &node, const Catalog &cat) const;
    bool contains(const QString &id) const { return m_defs.contains(id); }

    // Variable get/set nodes are shaped by the variable's declared type.
    NodeDef variableDef(const GraphVariable &var, bool setter,
                        const Catalog &cat) const;

    const QHash<QString, LifecycleSig> &beginModes() const { return m_beginModes; }
    LifecycleSig beginMode(const QString &key) const;
    // Begin mode keys in the order they are meant to be shown. A QHash has no
    // order of its own, so a combo built from beginModes() reshuffles between
    // runs; these four read as a timeline, earliest moment first.
    QStringList beginModeOrder() const { return m_beginOrder; }

    // The choices behind the two opts the Inspector has to edit. Without a list
    // to offer, `op` and `type` are only reachable by hand-editing the .sdzn.
    static const QStringList &binaryOperators(); // bi.op, opts["op"]
    static const QStringList &literalTypes();    // bi.literal, opts["type"]
    // True when the operator yields a bool whatever it is given, which is what
    // decides the shape of the Operator node's pins.
    static bool operatorYieldsBool(const QString &op);

    // Categories in palette display order.
    QStringList categories() const;

private:
    QHash<QString, NodeDef> m_defs;
    QStringList m_order;
    QHash<QString, LifecycleSig> m_beginModes;
    QStringList m_beginOrder;

    void add(const NodeDef &def);
    void addBeginMode(const QString &key, const LifecycleSig &sig);
};

// Builtin ids used by name elsewhere (codegen, canvas shortcuts).
namespace bi {
extern const QString Begin;
extern const QString End;
extern const QString Branch;
extern const QString Sequence;
extern const QString ForLoop;
extern const QString ForEach;
extern const QString While;
extern const QString Return;
extern const QString Comment;
extern const QString Raw;
extern const QString Cast;
extern const QString Literal;
extern const QString Print;

// Cast To / New Object target class. Two spellings reached the file format:
// "cls" is what both builds write, "class" is what some older projects carry.
// Every reader has to accept both or the node shows one type on the canvas and
// generates another.
QString castClass(const GraphNode &node);
} // namespace bi
