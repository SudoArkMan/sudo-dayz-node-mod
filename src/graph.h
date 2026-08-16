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

// ------------------------------------------------------------ author's layout
//
// A graph has no field for the blank lines, the comments and the indentation a
// method was written with, and most of the bodies this tool refuses to convert
// are refused for exactly that reason. All of it lives in GraphNode::opts,
// which already survives a save and a load, and which removeNode already takes
// with the node it belongs to. A side table would orphan: delete a node and its
// row stays behind, ready to reappear above someone else's statement.
//
// Keys, on the node that owns the line or the block:
//   fmt.base        what sits in front of a statement at the top of the body
//   fmt.unit        what one more level of nesting adds
//   fmt.eol         the line ending the body was written with
//   trivia.before   the blank lines and comments above this node's first line
//   trivia.trailing what follows that line, on the same line
//   trivia.end      the same, above the brace that closes this node's block
//   trivia.endElse  and above the brace that closes its second block
namespace nodefmt {

QString keyBase();
QString keyUnit();
// 385 of 512 extracted mod scripts and every vanilla file are written with
// CRLF, and the generator invents headers, braces and blank lines that have no
// source line to read an ending from. Without this the body comes back with two
// kinds of ending in it. Only "\n" and "\r\n" are line endings; anything else
// stored here is ignored, and absent means "\n".
//
// Two limits, both measured rather than reasoned about, because the comment
// above reads as a wider promise than the code keeps:
//
//   Only "convert to nodes" ever writes this, off the bytes a raw node holds.
//   The importer cannot: importEnforceText removes every carriage return from
//   the file before anything else reads it, so a body arrives there with LF
//   endings whatever the file on disk uses. Over 400 vanilla files, 399 of them
//   CRLF, and 1491 extracted mod scripts, 1015 of them CRLF, no node the
//   importer built carried this key.
//
//   It covers a method body and nothing else. The class header, the members,
//   the signature, the braces around the method and the preserved region are
//   written with "\n" either way, so a file with a converted CRLF block in it
//   holds both endings.
QString keyEol();
QString keyBefore();
QString keyTrailing();
QString keyEnd();
QString keyEndElse();

// The ending a run of text is written with: "\r\n" when the carriage returns
// win outright, "\n" otherwise.
//
// Real mod files do mix the two, and no single answer reproduces a mixed body.
// There is nothing to gain from a cleverer guess: every line that came out of
// the source keeps whatever ending it came with, only the lines the generator
// invents take this one, and the caller compares what it regenerated against
// what it read before accepting any of it. A wrong answer therefore costs a
// conversion and never a byte of the author's file.
QString eolOf(const QString &text);

// Whole lines, stored with a trailing newline so one blank line is "\n" rather
// than an empty string, which a map cannot tell from a key that is not there.
QStringList lines(const QString &stored);
QString store(const QStringList &lines);

// True when `text` holds nothing but whitespace and comments. Anything else is
// refused: the graph must not become a second place to hide script, or the node
// count stops meaning what it says.
bool isCommentaryOnly(const QString &text);
// The same invariant, stated once for every key above, so the side that reads a
// .sdzn and the side that writes a .c cannot drift apart on what they accept.
// A key that is not one of the formatting keys answers true: this speaks only
// for its own. Checked where a .sdzn is read and again where a .c is written,
// because a .sdzn is a file anyone can hand you and the mod browser makes
// opening other people's work ordinary; a trivia.before holding
// `GetGame().RequestExit(0);` would otherwise reach the user's mod as code.
bool isValidValue(const QString &key, const QString &value);
// Whitespace and nothing else. An indent field carrying more than that would
// generate a file that does not compile.
bool isIndentText(const QString &text);
// A line with nothing on it, which is a wider question than whether it is an
// indent: three quarters of the installed mods are written with CRLF endings,
// so splitting their bodies on the newline leaves a carriage return standing on
// every blank line. Indenting that would put whitespace at the end of a line
// nobody typed any into.
bool isBlankLine(const QString &line);

} // namespace nodefmt

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
