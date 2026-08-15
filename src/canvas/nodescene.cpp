#include "nodescene.h"

#include "enforce/ast.h"
#include "enforce/lower.h"
#include "nodeitem.h"
#include "noteitem.h"
#include "theme.h"
#include "wireitem.h"

#include <QSet>

#include <algorithm>
#include <utility>

namespace {

QStringList selectedIdsOf(const QGraphicsScene *scene)
{
    QStringList ids;
    const QList<QGraphicsItem *> sel = scene->selectedItems();
    for (QGraphicsItem *it : sel) {
        if (auto *n = dynamic_cast<NodeItem *>(it)) ids << n->nodeId();
        else if (auto *note = dynamic_cast<NoteItem *>(it)) ids << note->nodeId();
    }
    return ids;
}

bool sameIds(const QStringList &a, const QStringList &b)
{
    return QSet<QString>(a.begin(), a.end()) == QSet<QString>(b.begin(), b.end());
}

// Pin shapes come from the item's def, which is the same def painting used, so
// a hit test can never disagree with what is on screen.
const Pin *pinOf(const NodeScene *scene, const PinRef &ref)
{
    if (!ref.valid || ref.nodeId.isEmpty()) return nullptr;
    const NodeItem *item = scene->itemForNode(ref.nodeId);
    if (!item) return nullptr;
    return item->def().pin(ref.pinId, ref.dir);
}

// One selected item, as the layout operations see it: what it is called, where
// it sits, and the rect the user can actually see. NodeItem inflates its
// boundingRect for painting, so the drawn body is asked for by name; aligning
// to the halo would leave the visible edges one margin apart.
struct Placed {
    QString id;
    QPointF pos;
    QRectF rect;
};

// How well a dropped wire lands on `p`, 0 when it does not fit at all. An exact
// type match beats a numeric conversion, which beats landing on a wildcard, so
// a float dropped on a node with both a float and an int input takes the float.
int fitRank(const Catalog &cat, const PinType &src, PinDir srcDir, const Pin &p)
{
    if (!pinWouldFit(cat, src, srcDir, p)) return 0;
    if (src.kind != p.type.kind) return 1;
    if (src.kind == PinKind::Object || src.kind == PinKind::Enum)
        return src.cls == p.type.cls ? 3 : 2;
    return 3;
}

// The Enforce type a promoted variable is declared with. Object and enum pins
// carry their own name; everything else maps back to the primitive it parsed
// from. Class is the fallback because a member cannot be declared `auto`.
QString enforceTypeOf(const PinType &t)
{
    QString base;
    switch (t.kind) {
    case PinKind::Bool:     base = QStringLiteral("bool"); break;
    case PinKind::Int:      base = QStringLiteral("int"); break;
    case PinKind::Float:    base = QStringLiteral("float"); break;
    case PinKind::String:   base = QStringLiteral("string"); break;
    case PinKind::Vector:   base = QStringLiteral("vector"); break;
    case PinKind::Typename: base = QStringLiteral("typename"); break;
    case PinKind::Object:
    case PinKind::Enum:     base = t.cls; break;
    default: break;
    }
    if (base.isEmpty() || base == QLatin1String("auto"))
        base = QStringLiteral("Class");
    return t.isArray ? QStringLiteral("array<%1>").arg(base) : base;
}

// Everything an Enforce identifier is allowed to carry, and nothing else.
QString identifierPart(const QString &text)
{
    QString out;
    for (const QChar c : text)
        if (c.isLetterOrNumber() || c == QLatin1Char('_')) out += c;
    while (!out.isEmpty() && out.at(0).isDigit()) out.remove(0, 1);
    return out;
}

// m_Health from a pin called health. Return pins have no label of their own, so
// they borrow the node's title when that is one word; anything else would read
// as a sentence with the spaces taken out.
QString promotedNameFor(const Pin &pin, const NodeDef &def)
{
    QString base = identifierPart(pin.label);
    if (base.isEmpty() && pin.id == QLatin1String("ret")
        && !def.title.contains(QLatin1Char(' ')))
        base = identifierPart(def.title);
    if (base.isEmpty()) base = identifierPart(pin.id);
    if (base.isEmpty()) base = QStringLiteral("Value");
    base[0] = base.at(0).toUpper();
    return QStringLiteral("m_") + base;
}

QString uniqueVariableName(const Graph &g, const QString &base)
{
    const auto taken = [&g](const QString &candidate) {
        for (const GraphVariable &v : g.variables)
            if (v.name == candidate) return true;
        return false;
    };
    QString name = base;
    int suffix = 2;
    while (taken(name)) name = base + QString::number(suffix++);
    return name;
}

QVector<Placed> placedSelection(const QGraphicsScene *scene)
{
    QVector<Placed> out;
    const QList<QGraphicsItem *> sel = scene->selectedItems();
    for (QGraphicsItem *it : sel) {
        if (auto *n = dynamic_cast<NodeItem *>(it))
            out.append({n->nodeId(), n->pos(), n->mapRectToScene(n->bodyRect())});
        else if (auto *note = dynamic_cast<NoteItem *>(it))
            out.append({note->nodeId(), note->pos(),
                        note->mapRectToScene(note->boundingRect().adjusted(2, 2, -2, -2))});
    }
    return out;
}

} // namespace

bool pinWouldFit(const Catalog &cat, const PinType &type, PinDir dir,
                 const Pin &candidate)
{
    Pin probe;
    probe.dir = dir;
    probe.type = type;
    if (!canConnect(probe, candidate)) return false;

    // canConnect leaves object pins to the caller, because only the catalogue
    // knows the inheritance chain. The value flows out of `out`, so its class
    // has to be assignable to the input's class, not the other way round.
    const PinType &out = dir == PinDir::Out ? type : candidate.type;
    const PinType &in = dir == PinDir::Out ? candidate.type : type;
    if (out.kind == PinKind::Object && in.kind == PinKind::Object
        && !out.cls.isEmpty() && !in.cls.isEmpty())
        return cat.isA(out.cls, in.cls);
    return true;
}

NodeScene::NodeScene(Document *doc, QObject *parent)
    : QGraphicsScene(parent), m_doc(doc)
{
    setBackgroundBrush(theme::canvasBg());
    // Nodes move constantly while dragging, which is exactly the case the BSP
    // index is bad at.
    setItemIndexMethod(QGraphicsScene::NoIndex);
    setSceneRect(QRectF(-1500, -1000, 3000, 2000));

    connect(this, &QGraphicsScene::selectionChanged, this, &NodeScene::onSelectionChanged);

    if (m_doc) {
        // Direct, so the canvas is correct the instant an edit lands even if
        // nothing pumps the event loop before the next paint. Edits committed
        // from inside an item's own handler are safe because rebuild() unhooks
        // items immediately but defers their destruction.
        connect(m_doc, &Document::graphChanged, this, &NodeScene::rebuild);
        connect(m_doc, &Document::activeScriptChanged, this, &NodeScene::rebuild);
        connect(m_doc, &Document::projectChanged, this, &NodeScene::rebuild);
        connect(m_doc, &Document::selectionChanged, this,
                [this] { applyDocumentSelection(); });
    }

    rebuild();
}

void NodeScene::rebuild()
{
    for (WireItem *w : std::as_const(m_wires)) {
        removeItem(w);
        delete w;
    }
    m_wires.clear();
    // Items are unhooked immediately but destroyed later: a rebuild can be
    // triggered from inside an item's own event handler.
    for (NodeItem *n : std::as_const(m_nodes)) {
        removeItem(n);
        n->deleteLater();
    }
    m_nodes.clear();
    for (NoteItem *n : std::as_const(m_notes)) {
        removeItem(n);
        n->deleteLater();
    }
    m_notes.clear();

    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    if (!g) return;

    for (const GraphNode &n : g->nodes) {
        if (n.kind == NodeKind::Comment || n.ref == bi::Comment) {
            auto *item = new NoteItem(m_doc, n.id);
            addItem(item);
            item->refresh();
            m_notes.append(item);
        } else {
            auto *item = new NodeItem(m_doc, n.id);
            addItem(item);
            item->refresh();
            m_nodes.insert(n.id, item);
        }
    }

    rebuildWires();
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
        it.value()->setDiagnostics(m_analysis.forNode(it.key()));
    applyDocumentSelection();

    // The rect only ever grows, so panning never jumps when a node is added
    // near the edge of the current one.
    const QRectF bounds = graphBounds();
    if (!bounds.isNull()) {
        const QRectF want = bounds.adjusted(-1200, -900, 1200, 900);
        if (!sceneRect().contains(want)) setSceneRect(sceneRect().united(want));
    }
}

void NodeScene::refreshVisuals()
{
    // Items hold the live positions during a drag; re-reading the graph now
    // would snap everything back to where the drag started.
    if (!m_movingNodes) {
        for (NodeItem *n : std::as_const(m_nodes)) n->refresh();
        for (NoteItem *n : std::as_const(m_notes)) n->refresh();
    }
    rebuildWires();
}

void NodeScene::setAnalysis(const AnalysisResult &result)
{
    m_analysis = result;
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
        it.value()->setDiagnostics(result.forNode(it.key()));
}

NodeItem *NodeScene::itemForNode(const QString &id) const
{
    return m_nodes.value(id, nullptr);
}

void NodeScene::rebuildWires()
{
    for (WireItem *w : std::as_const(m_wires)) {
        removeItem(w);
        delete w;
    }
    m_wires.clear();

    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    if (!g) return;

    for (const GraphEdge &e : g->edges) {
        NodeItem *from = m_nodes.value(e.from.node, nullptr);
        NodeItem *to = m_nodes.value(e.to.node, nullptr);
        if (!from || !to) continue;
        auto *wire = new WireItem();
        wire->setEdgeId(e.id);
        // The wire takes the source pin's colour, so a chain reads as one
        // value travelling rather than two ends meeting.
        const Pin *src = from->def().pin(e.from.pin, PinDir::Out);
        wire->setPinType(src ? src->type : PinType{});
        wire->setEndpoints(from->pinScenePos(e.from.pin, PinDir::Out),
                           to->pinScenePos(e.to.pin, PinDir::In));
        addItem(wire);
        m_wires.append(wire);
    }
}

void NodeScene::updateWiresFor(const QString &nodeId)
{
    if (m_wires.isEmpty()) return;
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    if (!g) return;

    for (const GraphEdge &e : g->edges) {
        if (e.from.node != nodeId && e.to.node != nodeId) continue;
        NodeItem *from = m_nodes.value(e.from.node, nullptr);
        NodeItem *to = m_nodes.value(e.to.node, nullptr);
        if (!from || !to) continue;
        for (WireItem *w : std::as_const(m_wires)) {
            if (w->edgeId() != e.id) continue;
            w->setEndpoints(from->pinScenePos(e.from.pin, PinDir::Out),
                            to->pinScenePos(e.to.pin, PinDir::In));
            break;
        }
    }
}

// ------------------------------------------------------------ node editing

bool NodeScene::prepareNode(const QString &key, const QPointF &scenePos,
                            GraphNode *node, NodeDef *def)
{
    if (!m_doc || key.isEmpty() || !node || !def) return false;
    const Graph *g = m_doc->activeGraph();
    if (!g) {
        emit statusMessage(QStringLiteral("Open a script before adding nodes."));
        return false;
    }

    NodeKind kind = NodeKind::Call;

    if (key.startsWith(QLatin1String("var.get.")) || key.startsWith(QLatin1String("var.set."))) {
        const bool setter = key.startsWith(QLatin1String("var.set."));
        // Exact id match, then name: matching loosely binds the wrong member
        // whenever one variable's id ends with another's.
        const GraphVariable *var = variableForRef(*g, key);
        if (!var) {
            emit statusMessage(QStringLiteral("That variable no longer exists."));
            return false;
        }
        *def = m_doc->builtins().variableDef(*var, setter, m_doc->catalog());
        kind = setter ? NodeKind::VarSet : NodeKind::VarGet;
    } else {
        *def = m_doc->defForKey(key);
        if (key.startsWith(QLatin1String("bi.")))
            kind = key == bi::Comment ? NodeKind::Comment : NodeKind::Builtin;
        else if (def->category == QLatin1String("Events"))
            kind = NodeKind::Event;
    }

    if (!def->valid) {
        emit statusMessage(QStringLiteral("Nothing in the catalogue matches %1.").arg(key));
        return false;
    }

    node->id = nextId(QStringLiteral("n"));
    node->kind = kind;
    node->ref = key;
    node->x = qRound(scenePos.x());
    node->y = qRound(scenePos.y());
    // Seeding the declared defaults keeps generated code compiling even if the
    // user never touches the node again.
    for (const Pin &p : def->pins)
        if (p.dir == PinDir::In && p.hasDef) node->inputs.insert(p.id, p.def);
    return true;
}

void NodeScene::addNodeAt(const QString &key, const QPointF &scenePos)
{
    GraphNode node;
    NodeDef def;
    if (!prepareNode(key, scenePos, &node, &def)) return;

    m_doc->beginEdit(QStringLiteral("Add %1").arg(def.title));
    Graph *g = m_doc->activeGraph();
    if (!g) {
        m_doc->commitEdit();
        return;
    }
    g->nodes.append(node);
    m_doc->commitEdit();
    m_doc->setSelection({node.id});
    emit statusMessage(QStringLiteral("Added %1").arg(def.title));
}

QString NodeScene::addNodeConnectedTo(const QString &key, const QPointF &scenePos,
                                      const PinRef &from)
{
    if (!m_doc) return QString();
    const Pin *source = pinOf(this, from);

    // A node fed by the drag sits a body-width to the left of where the wire was
    // let go, so its output pin lands near the cursor rather than a whole node
    // past it and the wire runs forwards.
    QPointF at = scenePos;
    if (source && from.dir == PinDir::In) at.rx() -= theme::node::width;

    GraphNode node;
    NodeDef def;
    if (!prepareNode(key, at, &node, &def)) return QString();

    // The node the wire came from can have been deleted while the menu was
    // open. Placing it unconnected is still what the user asked for.
    const Pin *best = nullptr;
    if (source) {
        int bestRank = 0;
        for (const Pin &p : def.pins) {
            const int rank = fitRank(m_doc->catalog(), source->type, source->dir, p);
            if (rank > bestRank) {
                bestRank = rank;
                best = &p;
            }
        }
    }

    m_doc->beginEdit(best ? QStringLiteral("Add %1 and connect").arg(def.title)
                          : QStringLiteral("Add %1").arg(def.title));
    Graph *g = m_doc->activeGraph();
    if (!g) {
        m_doc->commitEdit();
        return QString();
    }
    g->nodes.append(node);
    if (best) {
        const EdgeEnd sourceEnd{from.nodeId, from.pinId};
        const EdgeEnd newEnd{node.id, best->id};
        const bool isExec = source->type.kind == PinKind::Exec;
        if (from.dir == PinDir::Out) connectPins(*g, sourceEnd, newEnd, isExec);
        else connectPins(*g, newEnd, sourceEnd, isExec);
    }
    m_doc->commitEdit();
    // Selected, not just placed: the inspector follows the selection, so the
    // node the user just asked for is the one they can carry on editing.
    m_doc->setSelection({node.id});

    emit statusMessage(best
                           ? QStringLiteral("Added %1 and connected it").arg(def.title)
                           : QStringLiteral("Added %1; nothing on it takes that pin")
                                 .arg(def.title));
    return node.id;
}

QString NodeScene::promoteToVariable(const PinRef &from, const QPointF &scenePos)
{
    if (!m_doc) return QString();
    const Pin *source = pinOf(this, from);
    const NodeItem *sourceItem = itemForNode(from.nodeId);
    if (!source || !sourceItem) {
        emit statusMessage(QStringLiteral("That pin is no longer on the canvas."));
        return QString();
    }
    if (source->type.kind == PinKind::Exec) {
        emit statusMessage(QStringLiteral("Exec pins carry no value to store."));
        return QString();
    }
    const Graph *read = m_doc->activeGraph();
    if (!read) {
        emit statusMessage(QStringLiteral("Open a script before adding nodes."));
        return QString();
    }

    GraphVariable var;
    var.id = nextId(QStringLiteral("v"));
    var.name = uniqueVariableName(*read, promotedNameFor(*source, sourceItem->def()));
    var.type = enforceTypeOf(source->type);

    // A drag off an input wants something to read, a drag off an output wants
    // somewhere to put the value.
    const bool setter = from.dir == PinDir::Out;
    const NodeDef def = m_doc->builtins().variableDef(var, setter, m_doc->catalog());
    const QString valuePin = setter ? QStringLiteral("v") : QStringLiteral("ret");
    const Pin *target = def.pin(valuePin, setter ? PinDir::In : PinDir::Out);
    if (!target) return QString();

    QPointF at = scenePos;
    if (!setter) at.rx() -= theme::node::width;

    GraphNode node;
    node.id = nextId(QStringLiteral("n"));
    node.kind = setter ? NodeKind::VarSet : NodeKind::VarGet;
    node.ref = QStringLiteral("var.%1.%2")
                   .arg(setter ? QStringLiteral("set") : QStringLiteral("get"), var.id);
    node.x = qRound(at.x());
    node.y = qRound(at.y());

    // The member and the node that uses it are one thought, so they are one
    // undo step: nothing here leaves a variable behind with no node on it.
    m_doc->beginEdit(QStringLiteral("Promote to variable"));
    Graph *g = m_doc->activeGraph();
    if (!g) {
        m_doc->commitEdit();
        return QString();
    }
    g->variables.append(var);
    g->nodes.append(node);
    if (setter) connectPins(*g, {from.nodeId, from.pinId}, {node.id, target->id}, false);
    else connectPins(*g, {node.id, target->id}, {from.nodeId, from.pinId}, false);
    m_doc->commitEdit();
    m_doc->setSelection({node.id});

    emit statusMessage(QStringLiteral("Promoted to %1 (%2). Rename it in the "
                                      "Variable Manager.")
                           .arg(var.name, var.type));
    return var.id;
}

void NodeScene::deleteSelectedNodes()
{
    if (!m_doc || !m_doc->activeGraph()) return;
    const QStringList ids = selectedIdsOf(this);
    if (ids.isEmpty()) return;

    m_doc->beginEdit(ids.size() > 1 ? QStringLiteral("Delete nodes")
                                    : QStringLiteral("Delete node"));
    Graph *g = m_doc->activeGraph();
    if (!g) {
        m_doc->commitEdit();
        return;
    }
    for (const QString &id : ids) removeNode(*g, id);
    m_doc->setSelection({});
    m_doc->commitEdit();
    emit statusMessage(ids.size() > 1
                           ? QStringLiteral("Deleted %1 nodes").arg(ids.size())
                           : QStringLiteral("Deleted 1 node"));
}

void NodeScene::duplicateSelection()
{
    if (!m_doc || !m_doc->activeGraph()) return;
    const QStringList ids = selectedIdsOf(this);
    if (ids.isEmpty()) return;

    m_doc->beginEdit(QStringLiteral("Duplicate"));
    Graph *g = m_doc->activeGraph();
    if (!g) {
        m_doc->commitEdit();
        return;
    }

    QHash<QString, QString> copiedAs;
    QVector<GraphNode> copies;
    for (const QString &id : ids) {
        const GraphNode *src = g->node(id);
        if (!src) continue;
        GraphNode copy = *src; // taken before any append; the vector reallocates
        copy.id = nextId(QStringLiteral("n"));
        copy.x += 20;
        copy.y += 20;
        copiedAs.insert(id, copy.id);
        copies.append(copy);
    }
    for (const GraphNode &c : copies) g->nodes.append(c);

    // Wires wholly inside the selection come along; ones crossing its edge do
    // not, because there is no sensible answer for where they should land.
    QVector<GraphEdge> added;
    for (const GraphEdge &e : g->edges) {
        if (!copiedAs.contains(e.from.node) || !copiedAs.contains(e.to.node)) continue;
        added.append({nextId(QStringLiteral("e")),
                      {copiedAs.value(e.from.node), e.from.pin},
                      {copiedAs.value(e.to.node), e.to.pin}});
    }
    g->edges += added;

    QStringList newIds;
    for (const GraphNode &c : copies) newIds << c.id;
    m_doc->setSelection(newIds);
    m_doc->commitEdit();
    emit statusMessage(QStringLiteral("Duplicated %1 nodes").arg(copies.size()));
}

// ------------------------------------------------------------ raw to nodes

namespace {

// "1 statement" or "4 statements". The count is the whole message, so it must
// not read as a template nobody finished.
QString statementsPhrase(int n)
{
    return n == 1 ? QStringLiteral("1 statement")
                  : QStringLiteral("%1 statements").arg(n);
}

QSet<QString> nodeIdsOf(const Graph &g)
{
    QSet<QString> ids;
    for (const GraphNode &n : g.nodes) ids.insert(n.id);
    return ids;
}

// Statements the lowering had to leave as raw code. Each one lands on a raw
// node of its own, so counting the new raw nodes counts the statements exactly.
int newRawNodes(const Graph &g, const QSet<QString> &before)
{
    int count = 0;
    for (const GraphNode &n : g.nodes)
        if (n.ref == bi::Raw && !before.contains(n.id)) ++count;
    return count;
}

} // namespace

bool NodeScene::isRawCodeNode(const QString &nodeId) const
{
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *node = g ? g->node(nodeId) : nullptr;
    return node && node->ref == bi::Raw;
}

QString NodeScene::variableName(const QString &variableId) const
{
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    if (!g) return QString();
    const GraphVariable *var = variableForRef(*g, variableId);
    return var ? var->name : QString();
}

bool NodeScene::convertRawToNodes(const QString &nodeId, QStringList *notes)
{
    if (!m_doc) return false;
    Graph *g = m_doc->activeGraph();
    if (!g) {
        emit statusMessage(QStringLiteral("Open a script before converting code."));
        return false;
    }
    const GraphNode *node = g->node(nodeId);
    if (!node || node->ref != bi::Raw) {
        emit statusMessage(QStringLiteral("That node does not hold raw code."));
        return false;
    }

    // Counted before the node goes away. The lowering reports what it could not
    // do; the total is what turns that into "6 of 8" rather than a bare list of
    // complaints.
    const int total =
        parseEnforceBody(node->opts.value(QStringLiteral("code"))).statementCount;
    if (total == 0) {
        emit statusMessage(QStringLiteral("That node has no code to convert."));
        return false;
    }

    // Lowered into a copy: an attempt that converts nothing must not leave an
    // undo step that restores a graph identical to the one it replaced.
    Graph working = *g;
    const QSet<QString> before = nodeIdsOf(working);
    QStringList local;
    const bool ok = explodeRawNode(working, nodeId, m_doc->catalog(),
                                   m_doc->builtins(), m_doc->project(), &local);
    if (notes) *notes += local;

    if (!ok) {
        emit statusMessage(QStringLiteral("Nothing could be converted: %1 %2 not "
                                          "representable yet.")
                               .arg(statementsPhrase(total),
                                    total == 1 ? QStringLiteral("is")
                                               : QStringLiteral("are")));
        return false;
    }

    const int stayedRaw = newRawNodes(working, before);
    const int lowered = qMax(0, total - stayedRaw);

    m_doc->beginEdit(QStringLiteral("Convert to nodes"));
    Graph *live = m_doc->activeGraph();
    if (live) *live = working;
    // The raw node is gone; leaving its id selected would leave the inspector
    // pointing at nothing.
    m_doc->setSelection({});
    m_doc->commitEdit();

    emit statusMessage(
        stayedRaw == 0
            ? QStringLiteral("Converted %1 to nodes").arg(statementsPhrase(lowered))
            : QStringLiteral("Converted %1 to nodes; %2 stayed as raw code")
                  .arg(statementsPhrase(lowered)).arg(stayedRaw));
    return true;
}

int NodeScene::convertAllRaw(QStringList *notes)
{
    if (!m_doc) return 0;
    Graph *g = m_doc->activeGraph();
    if (!g) {
        emit statusMessage(QStringLiteral("Open a script before converting code."));
        return 0;
    }

    QStringList targets;
    for (const GraphNode &n : g->nodes)
        if (n.ref == bi::Raw) targets << n.id;
    if (targets.isEmpty()) {
        emit statusMessage(QStringLiteral("This script has no raw code left."));
        return 0;
    }

    Graph working = *g;
    QStringList local;
    int converted = 0;
    // The targets are listed before the sweep starts. A conversion can leave a
    // new raw node behind for the statement it could not model, and feeding
    // those back in would never terminate.
    for (const QString &id : std::as_const(targets)) {
        if (!working.node(id)) continue;
        if (explodeRawNode(working, id, m_doc->catalog(), m_doc->builtins(),
                           m_doc->project(), &local))
            ++converted;
    }
    if (notes) *notes += local;

    if (converted == 0) {
        emit statusMessage(
            targets.size() == 1
                ? QStringLiteral("Nothing could be converted: that code is not "
                                 "representable yet.")
                : QStringLiteral("Nothing could be converted: none of the %1 raw "
                                 "nodes are representable yet.")
                      .arg(targets.size()));
        return 0;
    }

    // One edit for the whole sweep, so a script-wide conversion is one Ctrl+Z.
    m_doc->beginEdit(QStringLiteral("Convert all raw code"));
    Graph *live = m_doc->activeGraph();
    if (live) *live = working;
    m_doc->setSelection({});
    m_doc->commitEdit();

    const int left = targets.size() - converted;
    emit statusMessage(
        left == 0 ? QStringLiteral("Converted all %1 raw nodes").arg(converted)
                  : QStringLiteral("Converted %1 of %2 raw nodes; %3 are not "
                                   "representable yet")
                        .arg(converted).arg(targets.size()).arg(left));
    return converted;
}

// ------------------------------------------------------------ layout

void NodeScene::alignSelection(AlignEdge edge)
{
    if (!m_doc || !m_doc->activeGraph()) return;
    const QVector<Placed> picks = placedSelection(this);
    if (picks.size() < 2) {
        emit statusMessage(QStringLiteral("Select two or more nodes to align them."));
        return;
    }

    QRectF box = picks.first().rect;
    for (const Placed &p : picks) box = box.united(p.rect);

    // Centre alignment uses the bounding box's centre rather than the mean of
    // the items', so aligning twice does not creep.
    const auto offsetFor = [&](const QRectF &r) -> QPointF {
        switch (edge) {
        case AlignEdge::Left:    return QPointF(box.left() - r.left(), 0);
        case AlignEdge::Right:   return QPointF(box.right() - r.right(), 0);
        case AlignEdge::Top:     return QPointF(0, box.top() - r.top());
        case AlignEdge::Bottom:  return QPointF(0, box.bottom() - r.bottom());
        case AlignEdge::CentreX: return QPointF(box.center().x() - r.center().x(), 0);
        case AlignEdge::CentreY: return QPointF(0, box.center().y() - r.center().y());
        }
        return QPointF();
    };

    m_doc->beginEdit(QStringLiteral("Align nodes"));
    Graph *g = m_doc->activeGraph();
    if (g) {
        for (const Placed &p : picks) {
            const QPointF to = p.pos + offsetFor(p.rect);
            if (GraphNode *n = g->node(p.id)) {
                n->x = qRound(to.x());
                n->y = qRound(to.y());
            }
        }
    }
    m_doc->commitEdit();
    emit statusMessage(QStringLiteral("Aligned %1 nodes").arg(picks.size()));
}

void NodeScene::distributeSelection(Qt::Orientation orientation)
{
    if (!m_doc || !m_doc->activeGraph()) return;
    QVector<Placed> picks = placedSelection(this);
    if (picks.size() < 3) {
        emit statusMessage(
            QStringLiteral("Select three or more nodes to space them evenly."));
        return;
    }

    const bool horizontal = orientation == Qt::Horizontal;
    std::sort(picks.begin(), picks.end(), [horizontal](const Placed &a, const Placed &b) {
        return horizontal ? a.rect.center().x() < b.rect.center().x()
                          : a.rect.center().y() < b.rect.center().y();
    });

    // The two outermost stay put and the span between them is divided by the
    // gaps, not by the centres, so nodes of different heights read as evenly
    // spaced rather than evenly stepped.
    const double start = horizontal ? picks.first().rect.left() : picks.first().rect.top();
    const double end = horizontal ? picks.last().rect.right() : picks.last().rect.bottom();
    double occupied = 0;
    for (const Placed &p : picks)
        occupied += horizontal ? p.rect.width() : p.rect.height();
    const double gap = (end - start - occupied) / (picks.size() - 1);

    m_doc->beginEdit(QStringLiteral("Distribute nodes"));
    Graph *g = m_doc->activeGraph();
    if (g) {
        double cursor = start;
        for (const Placed &p : picks) {
            const double extent = horizontal ? p.rect.width() : p.rect.height();
            const double delta = cursor - (horizontal ? p.rect.left() : p.rect.top());
            if (GraphNode *n = g->node(p.id)) {
                if (horizontal) {
                    n->x = qRound(p.pos.x() + delta);
                    n->y = qRound(p.pos.y());
                } else {
                    n->x = qRound(p.pos.x());
                    n->y = qRound(p.pos.y() + delta);
                }
            }
            cursor += extent + gap;
        }
    }
    m_doc->commitEdit();
    emit statusMessage(QStringLiteral("Spaced %1 nodes evenly").arg(picks.size()));
}

void NodeScene::straightenWires()
{
    if (!m_doc || !m_doc->activeGraph()) return;
    const Graph *g = m_doc->activeGraph();

    QSet<QString> chosen;
    const QList<QGraphicsItem *> sel = selectedItems();
    for (QGraphicsItem *it : sel)
        if (auto *n = dynamic_cast<NodeItem *>(it)) chosen.insert(n->nodeId());
    if (chosen.isEmpty()) {
        emit statusMessage(
            QStringLiteral("Select the nodes whose wires should run flat."));
        return;
    }

    // Every offset is measured against the positions as they are now, so the
    // result does not depend on which node happens to be handled first.
    const auto isExecPin = [this](const QString &nodeId, const QString &pinId, PinDir dir) {
        const NodeItem *item = itemForNode(nodeId);
        const Pin *pin = item ? item->def().pin(pinId, dir) : nullptr;
        return pin && pin->type.kind == PinKind::Exec;
    };

    QHash<QString, double> shift;
    for (const QString &id : chosen) {
        const NodeItem *item = itemForNode(id);
        if (!item) continue;

        // Inputs first: a node is read as hanging off what feeds it, and an
        // exec wire is the spine of the graph, so it outranks a data wire.
        int bestRank = 0;
        double bestDelta = 0;
        for (const GraphEdge &e : g->edges) {
            const bool incoming = e.to.node == id;
            const bool outgoing = e.from.node == id;
            if (!incoming && !outgoing) continue;
            const QString otherId = incoming ? e.from.node : e.to.node;
            if (otherId == id) continue;
            const NodeItem *other = itemForNode(otherId);
            if (!other) continue;

            const bool exec = isExecPin(id, incoming ? e.to.pin : e.from.pin,
                                        incoming ? PinDir::In : PinDir::Out);
            // Anchoring to a node that is about to move too says nothing, so
            // partners inside the selection rank below partners outside it.
            const bool anchored = !chosen.contains(otherId);
            const int rank = (anchored ? 8 : 0) + (incoming ? 4 : 0) + (exec ? 2 : 0) + 1;
            if (rank <= bestRank) continue;

            const double mine = item->pinScenePos(incoming ? e.to.pin : e.from.pin,
                                                  incoming ? PinDir::In : PinDir::Out).y();
            const double theirs = other->pinScenePos(incoming ? e.from.pin : e.to.pin,
                                                     incoming ? PinDir::Out : PinDir::In).y();
            bestRank = rank;
            bestDelta = theirs - mine;
        }
        if (bestRank > 0 && qAbs(bestDelta) >= 0.5) shift.insert(id, bestDelta);
    }

    if (shift.isEmpty()) {
        emit statusMessage(QStringLiteral("Those wires already run flat."));
        return;
    }

    m_doc->beginEdit(QStringLiteral("Straighten wires"));
    Graph *live = m_doc->activeGraph();
    if (live) {
        for (auto it = shift.cbegin(); it != shift.cend(); ++it) {
            const NodeItem *item = itemForNode(it.key());
            if (!item) continue;
            if (GraphNode *n = live->node(it.key())) {
                n->x = qRound(item->x());
                n->y = qRound(item->y() + it.value());
            }
        }
    }
    m_doc->commitEdit();
    emit statusMessage(shift.size() > 1
                           ? QStringLiteral("Straightened %1 nodes").arg(shift.size())
                           : QStringLiteral("Straightened 1 node"));
}

// ------------------------------------------------------------ wire dragging

void NodeScene::beginWireDrag(const PinRef &from, const QPointF &scenePos)
{
    if (m_dragging) finishWireDrag(PinRef{});
    if (!m_doc || !from.valid) return;
    Graph *g = m_doc->activeGraph();
    const Pin *pin = pinOf(this, from);
    if (!g || !pin) return;

    PinRef source = from;
    const bool isExec = pin->type.kind == PinKind::Exec;

    // A data input holds one wire and an exec output drives one wire, so
    // grabbing a full pin picks the existing wire up rather than refusing a
    // second one. The far end becomes the thing the cursor is now holding.
    const GraphEdge *held = nullptr;
    if (!isExec && from.dir == PinDir::In)
        held = edgeInto(*g, from.nodeId, from.pinId);
    else if (isExec && from.dir == PinDir::Out)
        held = edgeFrom(*g, from.nodeId, from.pinId);

    if (held) {
        const PinRef other = from.dir == PinDir::In
            ? PinRef{held->from.node, held->from.pin, PinDir::Out, true}
            : PinRef{held->to.node, held->to.pin, PinDir::In, true};
        const QString edgeId = held->id;
        m_doc->beginEdit(QStringLiteral("Disconnect"));
        g = m_doc->activeGraph();
        if (g) disconnectEdge(*g, edgeId);
        m_doc->commitEdit();
        source = other;
    }

    m_dragFrom = source;
    m_dragging = true;

    const Pin *sourcePin = pinOf(this, source);
    m_dragWire = new WireItem();
    m_dragWire->setPreview(true);
    m_dragWire->setPinType(sourcePin ? sourcePin->type : pin->type);
    addItem(m_dragWire);
    updateWireDrag(scenePos);
}

void NodeScene::updateWireDrag(const QPointF &scenePos)
{
    if (!m_dragging || !m_dragWire) return;
    const NodeItem *anchor = itemForNode(m_dragFrom.nodeId);
    if (!anchor) return;
    const QPointF fixed = anchor->pinScenePos(m_dragFrom.pinId, m_dragFrom.dir);
    if (m_dragFrom.dir == PinDir::Out) m_dragWire->setEndpoints(fixed, scenePos);
    else m_dragWire->setEndpoints(scenePos, fixed);
}

// Takes the preview wire down and clears the drag state, returning the pin the
// gesture started from. Every way a drag can end goes through here, so none of
// them can leave a preview wire behind on the canvas.
PinRef NodeScene::endWireDrag()
{
    if (m_dragWire) {
        removeItem(m_dragWire);
        delete m_dragWire;
        m_dragWire = nullptr;
    }
    if (!m_dragging) return PinRef{};
    m_dragging = false;
    const PinRef from = m_dragFrom;
    m_dragFrom = PinRef{};
    return from;
}

void NodeScene::finishWireDragOnEmpty(const QPointF &scenePos)
{
    const PinRef from = endWireDrag();
    // Nothing is written to the graph here. The window answers by offering the
    // nodes that would fit, and a menu the user walks away from has to leave
    // the graph exactly as it found it.
    if (from.valid && pinOf(this, from)) emit wireDroppedOnEmpty(from, scenePos);
}

void NodeScene::finishWireDrag(const PinRef &to)
{
    const PinRef from = endWireDrag();
    if (!from.valid) return;
    if (!to.valid) return; // cancelled

    if (!wouldConnect(from, to)) {
        const Pin *a = pinOf(this, from);
        const Pin *b = pinOf(this, to);
        if (from.nodeId == to.nodeId)
            emit statusMessage(QStringLiteral("A node cannot wire to itself."));
        else if (a && b && a->type.kind == PinKind::Object
                 && b->type.kind == PinKind::Object)
            emit statusMessage(QStringLiteral(
                "Those classes are not compatible. Put a Cast node between them."));
        else
            emit statusMessage(QStringLiteral("Those pins cannot be connected."));
        return;
    }

    const Pin *a = pinOf(this, from);
    if (!a) return;
    const PinRef &out = a->dir == PinDir::Out ? from : to;
    const PinRef &in = a->dir == PinDir::Out ? to : from;
    const bool isExec = a->type.kind == PinKind::Exec;

    m_doc->beginEdit(QStringLiteral("Connect"));
    Graph *g = m_doc->activeGraph();
    if (g) connectPins(*g, {out.nodeId, out.pinId}, {in.nodeId, in.pinId}, isExec);
    m_doc->commitEdit();
}

bool NodeScene::wouldConnect(const PinRef &a, const PinRef &b) const
{
    if (!a.valid || !b.valid || !m_doc) return false;
    if (a.nodeId == b.nodeId) return false;

    const Pin *pa = pinOf(this, a);
    const Pin *pb = pinOf(this, b);
    if (!pa || !pb) return false;
    return pinWouldFit(m_doc->catalog(), pa->type, pa->dir, *pb);
}

PinType NodeScene::typeOfPin(const PinRef &ref) const
{
    const Pin *pin = pinOf(this, ref);
    return pin ? pin->type : PinType{};
}

// ------------------------------------------------------------ moving, state

void NodeScene::beginNodeMove()
{
    m_movingNodes = true;
    m_moveDirty = false;
}

void NodeScene::nodeMoved(const QString &nodeId)
{
    if (m_movingNodes) m_moveDirty = true;
    updateWiresFor(nodeId);
}

void NodeScene::endNodeMove()
{
    const bool moved = m_moveDirty;
    m_movingNodes = false;
    m_moveDirty = false;
    if (!moved || !m_doc || !m_doc->activeGraph()) return;

    // One edit for the whole gesture: the items carried the positions while
    // the drag was live, the graph learns about them once, here.
    m_doc->beginEdit(QStringLiteral("Move nodes"));
    Graph *g = m_doc->activeGraph();
    if (g) {
        for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
            if (GraphNode *n = g->node(it.key())) {
                n->x = qRound(it.value()->x());
                n->y = qRound(it.value()->y());
            }
        }
        for (NoteItem *note : std::as_const(m_notes)) {
            if (GraphNode *n = g->node(note->nodeId())) {
                n->x = qRound(note->x());
                n->y = qRound(note->y());
            }
        }
    }
    m_doc->commitEdit();
}

void NodeScene::emitNodeDoubleClicked(const QString &nodeId)
{
    emit nodeDoubleClicked(nodeId);
}

QRectF NodeScene::graphBounds() const
{
    QRectF bounds;
    for (NodeItem *n : std::as_const(m_nodes))
        bounds = bounds.isNull() ? n->sceneBoundingRect()
                                 : bounds.united(n->sceneBoundingRect());
    for (NoteItem *n : std::as_const(m_notes))
        bounds = bounds.isNull() ? n->sceneBoundingRect()
                                 : bounds.united(n->sceneBoundingRect());
    return bounds;
}

void NodeScene::onSelectionChanged()
{
    if (m_syncing) return;
    syncSelectionToDocument();
}

void NodeScene::syncSelectionToDocument()
{
    if (!m_doc) return;
    const QStringList ids = selectedIdsOf(this);
    if (sameIds(ids, m_doc->selection())) return;
    m_syncing = true;
    m_doc->setSelection(ids);
    m_syncing = false;
}

void NodeScene::applyDocumentSelection()
{
    if (!m_doc || m_syncing) return;
    const QStringList sel = m_doc->selection();
    m_syncing = true;
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
        it.value()->setSelected(sel.contains(it.key()));
    for (NoteItem *n : std::as_const(m_notes))
        n->setSelected(sel.contains(n->nodeId()));
    m_syncing = false;
}
