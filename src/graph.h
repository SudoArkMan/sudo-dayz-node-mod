// Graph model with Blueprint semantics.
//
// Two pin families:
//   exec: white, one outgoing connection per pin, defines statement order
//   data: typed, an input takes one edge, an output may fan out
//
// Nodes store only a catalogue key and their literal inputs; pin shapes are
// resolved from the catalogue at render time, so a project file stays small
// and survives a catalogue rebuild after a DayZ update. Matches the .sdzn
// format of the Electron build byte-for-byte in spirit: unknown JSON fields
// are preserved on load and written back on save.
#pragma once

#include "pins.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

struct NodeDef {
    QString key;
    QString title;
    QString subtitle;    // owning class / file, shown under the title
    QString category;
    QColor accent;
    QVector<Pin> pins;
    QString doc;
    QString loc;
    bool pure = false;   // Get/Is/Has style nodes rendered without exec pins
    bool native = false; // engine-implemented; cannot be overridden
    bool valid = false;

    const Pin *pin(const QString &id, PinDir dir) const;
};

enum class NodeKind { Call, Event, Builtin, VarGet, VarSet, Comment };

QString nodeKindName(NodeKind k);
NodeKind nodeKindFromName(const QString &name);

struct GraphNode {
    QString id;
    NodeKind kind = NodeKind::Call;
    // Catalogue key ("m123", "g7", "en12", "co5") or builtin id ("bi.branch")
    QString ref;
    double x = 0;
    double y = 0;
    QMap<QString, QString> inputs; // literals for unconnected inputs, by pin id
    QMap<QString, QString> opts;   // free-form settings (comment text, var name)
    QJsonObject extra;             // unknown .sdzn fields, preserved on save
};

struct EdgeEnd {
    QString node;
    QString pin;
    bool operator==(const EdgeEnd &o) const { return node == o.node && pin == o.pin; }
};

struct GraphEdge {
    QString id;
    EdgeEnd from;
    EdgeEnd to;
    QJsonObject extra;
};

struct GraphVariable {
    QString id;
    QString name;
    QString type;      // Enforce type name
    QString def;       // default value literal ("default" in JSON)
    bool sync = false;    // RegisterNetSyncVariable + SetSynchDirty
    bool persist = false; // OnStoreSave / OnStoreLoad
    bool isStatic = false;
    bool isConst = false;
    bool isPrivate = false;
    bool isProtected = false;
    bool isRef = false;
    // `ref` is tri-state: absent means "infer from the type", present means the
    // author decided. An explicit false must beat the inference, so the flag
    // alone cannot carry it.
    bool hasRef = false;
    QJsonObject extra;
};

struct GraphParam {
    QString name;
    QString type;
};

// A method the script declares; its body is whatever chains off its entry node.
struct GraphFunction {
    QString id;
    QString name;
    QString returns;
    QVector<GraphParam> params;
    bool isStatic = false;
    bool isPrivate = false;
    bool isProtected = false;
    bool isOverride = false;
    QString rawBody; // kept verbatim by the importer
    // An empty rawBody is a real body (an empty override), not a missing one,
    // and the importer writes exactly that for `void OnX() {}`.
    bool hasRawBody = false;
    QJsonObject extra;
};

struct Graph {
    QString className = QStringLiteral("MyItem");
    QString baseClass = QStringLiteral("ItemBase");
    bool modded = false;
    QString module = QStringLiteral("4_World"); // 3_Game | 4_World | 5_Mission
    QVector<GraphNode> nodes;
    QVector<GraphEdge> edges;
    QVector<GraphVariable> variables;
    QVector<GraphFunction> functions;
    QJsonObject extra;

    GraphNode *node(const QString &id);
    const GraphNode *node(const QString &id) const;
    const GraphVariable *variable(const QString &name) const;
};

QString nextId(const QString &prefix = QStringLiteral("n"));
// An id nothing in `g` is already using. Worth preferring whenever nodes are
// inserted into a graph that came off disk, because the counter behind nextId
// restarts every launch and knows nothing about ids the file already holds.
QString uniqueId(const Graph &g, const QString &prefix = QStringLiteral("n"));

// Variable node refs are written either bare ("v0") or prefixed
// ("var.get.v0"). Every module must strip the prefix and compare the id
// EXACTLY: matching on a suffix binds the wrong member whenever one id ends
// with another ("count" vs "hitcount").
QString variableIdOf(const QString &ref);
// The variable a varGet/varSet node refers to, or null when it was deleted.
const GraphVariable *variableForRef(const Graph &g, const QString &ref);

// Connection rules:
//   exec: an exec OUTPUT drives at most one edge, an input takes several
//   data: a data INPUT takes at most one edge, outputs fan out freely
bool canConnect(const Pin &a, const Pin &b);

// Adds the edge, displacing whichever existing edge the rules require.
void connectPins(Graph &g, const EdgeEnd &from, const EdgeEnd &to, bool isExec);
void disconnectEdge(Graph &g, const QString &edgeId);
void removeNode(Graph &g, const QString &nodeId);

const GraphEdge *edgeInto(const Graph &g, const QString &nodeId, const QString &pin);
const GraphEdge *edgeFrom(const Graph &g, const QString &nodeId, const QString &pin);

// Walk an exec chain from a node's exec output, cutting cycles.
QVector<const GraphNode *> execChain(const Graph &g, const QString &startNode,
                                     const QString &startPin = QStringLiteral("exec"));

// .sdzn JSON round-trip for one graph object.
Graph graphFromJson(const QJsonObject &obj);
QJsonObject graphToJson(const Graph &g);
