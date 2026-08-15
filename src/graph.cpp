#include "graph.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

const Pin *NodeDef::pin(const QString &id, PinDir dir) const
{
    for (const Pin &p : pins)
        if (p.id == id && p.dir == dir) return &p;
    return nullptr;
}

QString nodeKindName(NodeKind k)
{
    switch (k) {
    case NodeKind::Call: return QStringLiteral("call");
    case NodeKind::Event: return QStringLiteral("event");
    case NodeKind::Builtin: return QStringLiteral("builtin");
    case NodeKind::VarGet: return QStringLiteral("varGet");
    case NodeKind::VarSet: return QStringLiteral("varSet");
    case NodeKind::Comment: return QStringLiteral("comment");
    }
    return QStringLiteral("call");
}

NodeKind nodeKindFromName(const QString &name)
{
    if (name == "event") return NodeKind::Event;
    if (name == "builtin") return NodeKind::Builtin;
    if (name == "varGet") return NodeKind::VarGet;
    if (name == "varSet") return NodeKind::VarSet;
    if (name == "comment") return NodeKind::Comment;
    return NodeKind::Call;
}

GraphNode *Graph::node(const QString &id)
{
    for (GraphNode &n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const GraphNode *Graph::node(const QString &id) const
{
    for (const GraphNode &n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const GraphVariable *Graph::variable(const QString &name) const
{
    for (const GraphVariable &v : variables)
        if (v.name == name) return &v;
    return nullptr;
}

QString nextId(const QString &prefix)
{
    // The counter alone keeps ids apart inside one run, but it restarts at zero
    // every launch, so a fresh id has to avoid the ids already in the file that
    // was just opened. With a two-character salt that produced three-character
    // ids of exactly the shape hand-authored projects already use, and
    // Graph::node() returns the first match, so a collision would silently
    // attach a new node to an existing one. Four characters puts the odds at
    // roughly one in 1.7 million per id and makes the result longer than the
    // short ids that are already out there.
    static quint32 counter = 0;
    counter += 1;
    const quint32 salt = QRandomGenerator::global()->bounded(1679616u); // 36^4
    return prefix + QString::number(counter, 36)
           + QString::number(salt, 36).rightJustified(4, QLatin1Char('0'));
}

QString uniqueId(const Graph &g, const QString &prefix)
{
    QSet<QString> taken;
    for (const GraphNode &n : g.nodes) taken.insert(n.id);
    for (const GraphEdge &e : g.edges) taken.insert(e.id);
    for (const GraphVariable &v : g.variables) taken.insert(v.id);
    for (const GraphFunction &f : g.functions) taken.insert(f.id);

    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString id = nextId(prefix);
        if (!taken.contains(id)) return id;
    }
    // 64 misses means something is badly wrong with the generator rather than
    // with this graph; a suffix nothing else can be using ends the loop.
    return nextId(prefix) + QStringLiteral("x");
}

QString variableIdOf(const QString &ref)
{
    if (ref.startsWith(QLatin1String("var.get."))
        || ref.startsWith(QLatin1String("var.set.")))
        return ref.mid(8);
    return ref;
}

const GraphVariable *variableForRef(const Graph &g, const QString &ref)
{
    const QString id = variableIdOf(ref);
    for (const GraphVariable &v : g.variables)
        if (!v.id.isEmpty() && v.id == id) return &v;
    // Older files reference a variable by name; accept that, but only after
    // every id has had its chance, so an id never loses to someone's name.
    for (const GraphVariable &v : g.variables)
        if (!v.name.isEmpty() && v.name == id) return &v;
    return nullptr;
}

bool canConnect(const Pin &a, const Pin &b)
{
    if (a.dir == b.dir) return false;
    const Pin &from = a.dir == PinDir::Out ? a : b;
    const Pin &to = a.dir == PinDir::Out ? b : a;
    const bool fe = from.type.kind == PinKind::Exec;
    const bool te = to.type.kind == PinKind::Exec;
    if (fe != te) return false;
    if (fe) return true;
    if (from.type.isArray != to.type.isArray) return false;
    if (from.type.kind == PinKind::Any || to.type.kind == PinKind::Any) return true;
    if (from.type.kind != to.type.kind) {
        const auto numeric = [](PinKind k) {
            return k == PinKind::Int || k == PinKind::Float;
        };
        return numeric(from.type.kind) && numeric(to.type.kind);
    }
    // object-to-object class compatibility is checked against the catalogue
    // by the caller, which knows the inheritance chain
    return true;
}

void connectPins(Graph &g, const EdgeEnd &from, const EdgeEnd &to, bool isExec)
{
    if (from.node == to.node) return;
    QVector<GraphEdge> kept;
    kept.reserve(g.edges.size());
    for (const GraphEdge &e : g.edges) {
        if (isExec && e.from == from) continue;
        if (!isExec && e.to == to) continue;
        kept.append(e);
    }
    g.edges = kept;
    g.edges.append({nextId(QStringLiteral("e")), from, to});
}

void disconnectEdge(Graph &g, const QString &edgeId)
{
    for (int i = 0; i < g.edges.size(); ++i) {
        if (g.edges[i].id == edgeId) {
            g.edges.removeAt(i);
            return;
        }
    }
}

void removeNode(Graph &g, const QString &nodeId)
{
    for (int i = g.nodes.size() - 1; i >= 0; --i)
        if (g.nodes[i].id == nodeId) g.nodes.removeAt(i);
    for (int i = g.edges.size() - 1; i >= 0; --i)
        if (g.edges[i].from.node == nodeId || g.edges[i].to.node == nodeId)
            g.edges.removeAt(i);
}

const GraphEdge *edgeInto(const Graph &g, const QString &nodeId, const QString &pin)
{
    for (const GraphEdge &e : g.edges)
        if (e.to.node == nodeId && e.to.pin == pin) return &e;
    return nullptr;
}

const GraphEdge *edgeFrom(const Graph &g, const QString &nodeId, const QString &pin)
{
    for (const GraphEdge &e : g.edges)
        if (e.from.node == nodeId && e.from.pin == pin) return &e;
    return nullptr;
}

QVector<const GraphNode *> execChain(const Graph &g, const QString &startNode,
                                     const QString &startPin)
{
    QVector<const GraphNode *> out;
    QSet<QString> seen;
    QString curNode = startNode;
    QString curPin = startPin;
    for (;;) {
        const GraphEdge *e = edgeFrom(g, curNode, curPin);
        if (!e) break;
        const GraphNode *next = g.node(e->to.node);
        if (!next || seen.contains(next->id)) break;
        seen.insert(next->id);
        out.append(next);
        curNode = next->id;
        curPin = QStringLiteral("exec");
    }
    return out;
}

// ------------------------------------------------------------- JSON I/O

namespace {

QMap<QString, QString> stringMap(const QJsonObject &obj)
{
    QMap<QString, QString> out;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonValue v = it.value();
        if (v.isString()) {
            out.insert(it.key(), v.toString());
        } else if (v.isBool()) {
            out.insert(it.key(), v.toBool() ? QStringLiteral("true")
                                            : QStringLiteral("false"));
        } else if (v.isDouble()) {
            // Default QString::number rounds at 6 significant digits, which
            // silently rewrites 1234567 as 1.23457e+06 on the way back out.
            const double d = v.toDouble();
            const qint64 i = v.toInteger();
            out.insert(it.key(), double(i) == d ? QString::number(i)
                                                : QString::number(d, 'g', 17));
        } else if (!v.isNull() && !v.isUndefined()) {
            // An array or object here is not something a pin literal can hold;
            // keep the JSON text rather than flattening it to "0".
            out.insert(it.key(), QString::fromUtf8(
                                     QJsonDocument::fromVariant(v.toVariant())
                                         .toJson(QJsonDocument::Compact))
                                     .trimmed());
        }
    }
    return out;
}

QJsonObject toJson(const QMap<QString, QString> &map)
{
    QJsonObject out;
    for (auto it = map.begin(); it != map.end(); ++it)
        out.insert(it.key(), it.value());
    return out;
}

// Everything in `obj` except the listed keys, preserved verbatim on save so
// a project touched by a newer build never loses fields.
QJsonObject extrasOf(const QJsonObject &obj, const QStringList &known)
{
    QJsonObject out;
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (!known.contains(it.key())) out.insert(it.key(), it.value());
    return out;
}

void mergeExtras(QJsonObject &target, const QJsonObject &extra)
{
    for (auto it = extra.begin(); it != extra.end(); ++it)
        target.insert(it.key(), it.value());
}

} // namespace

Graph graphFromJson(const QJsonObject &obj)
{
    Graph g;
    g.className = obj.value("className").toString(g.className);
    g.baseClass = obj.value("baseClass").toString();
    g.modded = obj.value("modded").toBool();
    g.module = obj.value("module").toString(g.module);

    for (const QJsonValue &v : obj.value("nodes").toArray()) {
        const QJsonObject o = v.toObject();
        GraphNode n;
        n.id = o.value("id").toString();
        n.kind = nodeKindFromName(o.value("kind").toString());
        n.ref = o.value("ref").toString();
        n.x = o.value("x").toDouble();
        n.y = o.value("y").toDouble();
        n.inputs = stringMap(o.value("inputs").toObject());
        n.opts = stringMap(o.value("opts").toObject());
        n.extra = extrasOf(o, {"id", "kind", "ref", "x", "y", "inputs", "opts"});
        g.nodes.append(n);
    }

    for (const QJsonValue &v : obj.value("edges").toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonObject from = o.value("from").toObject();
        const QJsonObject to = o.value("to").toObject();
        GraphEdge e;
        e.id = o.value("id").toString();
        e.from = {from.value("node").toString(), from.value("pin").toString()};
        e.to = {to.value("node").toString(), to.value("pin").toString()};
        e.extra = extrasOf(o, {"id", "from", "to"});
        g.edges.append(e);
    }

    for (const QJsonValue &v : obj.value("variables").toArray()) {
        const QJsonObject o = v.toObject();
        GraphVariable var;
        var.id = o.value("id").toString();
        var.name = o.value("name").toString();
        var.type = o.value("type").toString();
        var.def = o.value("default").toString();
        var.sync = o.value("sync").toBool();
        var.persist = o.value("persist").toBool();
        var.isStatic = o.value("static").toBool();
        var.isConst = o.value("const").toBool();
        var.isPrivate = o.value("private").toBool();
        var.isProtected = o.value("protected").toBool();
        var.isRef = o.value("ref").toBool();
        var.hasRef = o.contains("ref");
        var.extra = extrasOf(o, {"id", "name", "type", "default", "sync", "persist",
                                 "static", "const", "private", "protected", "ref"});
        g.variables.append(var);
    }

    for (const QJsonValue &v : obj.value("functions").toArray()) {
        const QJsonObject o = v.toObject();
        GraphFunction fn;
        fn.id = o.value("id").toString();
        fn.name = o.value("name").toString();
        fn.returns = o.value("returns").toString();
        for (const QJsonValue &pv : o.value("params").toArray()) {
            const QJsonObject po = pv.toObject();
            fn.params.append({po.value("name").toString(), po.value("type").toString()});
        }
        fn.isStatic = o.value("static").toBool();
        fn.isPrivate = o.value("private").toBool();
        fn.isProtected = o.value("protected").toBool();
        fn.isOverride = o.value("override").toBool();
        fn.rawBody = o.value("rawBody").toString();
        fn.hasRawBody = o.contains("rawBody");
        fn.extra = extrasOf(o, {"id", "name", "returns", "params", "static",
                                "private", "protected", "override", "rawBody"});
        g.functions.append(fn);
    }

    g.extra = extrasOf(obj, {"className", "baseClass", "modded", "module",
                             "nodes", "edges", "variables", "functions"});
    return g;
}

QJsonObject graphToJson(const Graph &g)
{
    QJsonObject obj;
    obj.insert("className", g.className);
    obj.insert("baseClass", g.baseClass);
    obj.insert("modded", g.modded);
    obj.insert("module", g.module);

    QJsonArray nodes;
    for (const GraphNode &n : g.nodes) {
        QJsonObject o;
        o.insert("id", n.id);
        o.insert("kind", nodeKindName(n.kind));
        o.insert("ref", n.ref);
        o.insert("x", qRound(n.x));
        o.insert("y", qRound(n.y));
        o.insert("inputs", toJson(n.inputs));
        if (!n.opts.isEmpty()) o.insert("opts", toJson(n.opts));
        mergeExtras(o, n.extra);
        nodes.append(o);
    }
    obj.insert("nodes", nodes);

    QJsonArray edges;
    for (const GraphEdge &e : g.edges) {
        QJsonObject o;
        o.insert("id", e.id);
        o.insert("from", QJsonObject{{"node", e.from.node}, {"pin", e.from.pin}});
        o.insert("to", QJsonObject{{"node", e.to.node}, {"pin", e.to.pin}});
        mergeExtras(o, e.extra);
        edges.append(o);
    }
    obj.insert("edges", edges);

    QJsonArray vars;
    for (const GraphVariable &v : g.variables) {
        QJsonObject o;
        o.insert("id", v.id);
        o.insert("name", v.name);
        o.insert("type", v.type);
        o.insert("default", v.def);
        o.insert("sync", v.sync);
        o.insert("persist", v.persist);
        if (v.isStatic) o.insert("static", true);
        if (v.isConst) o.insert("const", true);
        if (v.isPrivate) o.insert("private", true);
        if (v.isProtected) o.insert("protected", true);
        if (v.hasRef) o.insert("ref", v.isRef);
        else if (v.isRef) o.insert("ref", true);
        mergeExtras(o, v.extra);
        vars.append(o);
    }
    obj.insert("variables", vars);

    QJsonArray fns;
    for (const GraphFunction &f : g.functions) {
        QJsonObject o;
        o.insert("id", f.id);
        o.insert("name", f.name);
        o.insert("returns", f.returns);
        QJsonArray params;
        for (const GraphParam &p : f.params)
            params.append(QJsonObject{{"name", p.name}, {"type", p.type}});
        o.insert("params", params);
        if (f.isStatic) o.insert("static", true);
        if (f.isPrivate) o.insert("private", true);
        if (f.isProtected) o.insert("protected", true);
        if (f.isOverride) o.insert("override", true);
        if (f.hasRawBody || !f.rawBody.isEmpty()) o.insert("rawBody", f.rawBody);
        mergeExtras(o, f.extra);
        fns.append(o);
    }
    obj.insert("functions", fns);

    mergeExtras(obj, g.extra);
    return obj;
}
