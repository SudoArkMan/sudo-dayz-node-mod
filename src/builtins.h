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

    // The choices behind the opts the Inspector has to edit. Without a list to
    // offer, `op`, `type` and `category` are only reachable by hand-editing the
    // .sdzn. Every node that takes one still works untouched, so the picker is
    // polish rather than a thing a graph cannot be built without.
    static const QStringList &binaryOperators(); // bi.op, opts["op"]
    static const QStringList &literalTypes();    // bi.literal, opts["type"]
    // Timing nodes, opts["category"]. Keys, not the constants they emit.
    static const QStringList &callCategories();
    // What one of those keys means, for a picker and for the inspector text.
    static QString callCategoryLabel(const QString &key);
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

// Arrays. Make Array is the one node in the group that declares an array
// instead of working on one, and the only builtin whose pin list is decided by
// the user rather than by its definition.
extern const QString MakeArray;
extern const QString ArrayCount;
extern const QString ArrayGet;
extern const QString ArrayInsert;
extern const QString ArrayInsertAt;
extern const QString ArrayRemove;
extern const QString ArrayClear;
extern const QString ArrayFind;
extern const QString ArraySort;
extern const QString ArrayForIndex;
// Written before these existed and still the node for `arr[i] = v`, so it is
// named here rather than respelled in every module that has to recognise it.
extern const QString SetElement;

// How many element pins a Make Array carries, read off opts["count"] and
// clamped to the def's own limits. An untouched node answers the default, so a
// node placed from the palette already has somewhere to type.
int listCount(const GraphNode &node, const PinList &list);
// The pin id of element `index`, "el0" and up. One spelling, because the
// canvas, the generator and the analyser all have to name the same pin. Two
// letters rather than one: "exec" also begins with an "e", so a prefix test
// written against "e" would count an exec pin as an element.
QString listPinId(const PinList &list, int index);

// The element type of an array node, as far as the node itself can say: what
// the author set in Details, or nothing. Deliberately does NOT guess from the
// wires, because the canvas resolves a def without the graph in hand and a
// subtitle claiming a type the generator then disagreed with would be worse
// than one that stays quiet. The generator does the wire-based inference, and
// its rule is written out at arrayElementFor in codegen.cpp.
QString declaredElementType(const GraphNode &node);

// Timing. These four are the only nodes that write more than statements: Set
// Timer declares a `ref` member and a whole callback method, Call Later writes
// a callback method. Everything about that is derived from one name, so the
// member, the call, the string the engine dispatches on and the method it
// dispatches to cannot disagree with each other.
extern const QString SetTimer;
extern const QString StopTimer;
extern const QString CallLater;
extern const QString CancelCallLater;

// The identifier a timing node works from. Read off opts["name"], reduced to
// something that can be an Enforce identifier, and falling back to the node's
// own id so an untouched node still generates a unique member and method rather
// than colliding with the next one.
QString timingName(const GraphNode &node);
// `m_Reload` and `ReloadElapsed` for a timer named Reload. A deferred call has
// no member and its callback is the name itself, which is what puts a readable
// `CallLater(RefreshHud, 250, false)` at the call site.
QString timerMember(const QString &name);
QString timerCallback(const QString &name);
// The CALL_CATEGORY_* constant a timing node's opts["category"] names.
QString callCategoryConstant(const GraphNode &node);

// Cast To / New Object target class. Two spellings reached the file format:
// "cls" is what both builds write, "class" is what some older projects carry.
// Every reader has to accept both or the node shows one type on the canvas and
// generates another.
QString castClass(const GraphNode &node);
} // namespace bi
