#include "document.h"

#include "scriptapi.h"

Document::Document(QObject *parent)
    : QObject(parent)
{
    m_project = newProject();
}

bool Document::loadCatalog(const QString &jsonPath)
{
    return m_catalog.load(jsonPath);
}

NodeDef Document::defForKey(const QString &key) const
{
    if (m_builtins.contains(key)) return m_builtins.def(key);
    // A project's own functions and members are nodes too, and their keys carry
    // ids rather than names so a rename does not orphan them.
    if (isScriptNodeKey(key)) {
        const auto isEnum = [this](const QString &n) { return m_catalog.isEnum(n); };
        const NodeDef def = scriptDefFor(key, m_project, isEnum);
        if (def.valid) return def;
    }
    return m_catalog.defFor(key);
}

NodeDef Document::defForNode(const GraphNode &node) const
{
    switch (node.kind) {
    case NodeKind::VarGet:
    case NodeKind::VarSet: {
        // Variable nodes are shaped by the variable they reference; the ref is
        // the variable id so a rename does not orphan the node. Resolution goes
        // through variableForRef so the canvas, the generator and the analyser
        // cannot disagree about which member a node points at.
        const Graph *g = activeGraph();
        if (g) {
            if (const GraphVariable *v = variableForRef(*g, node.ref))
                return m_builtins.variableDef(*v, node.kind == NodeKind::VarSet,
                                              m_catalog);
        }
        // The variable was deleted out from under the node. Show it as a stub
        // rather than dropping it, so the graph still round-trips.
        GraphVariable missing;
        missing.name = node.ref;
        missing.type = QStringLiteral("auto");
        return m_builtins.variableDef(missing, node.kind == NodeKind::VarSet,
                                      m_catalog);
    }
    case NodeKind::Builtin:
    case NodeKind::Comment:
        return m_builtins.defForNode(node, m_catalog);
    default:
        break;
    }
    if (m_builtins.contains(node.ref)) return m_builtins.defForNode(node, m_catalog);
    return defForKey(node.ref);
}

Graph *Document::activeGraph()
{
    ScriptEntry *s = m_project.active();
    return s ? &s->graph : nullptr;
}

const Graph *Document::activeGraph() const
{
    const ScriptEntry *s = activeEntry();
    return s ? &s->graph : nullptr;
}

const ScriptEntry *Document::activeEntry() const
{
    const ScriptEntry *s = m_project.script(m_project.activeId);
    if (!s && !m_project.scripts.isEmpty()) s = &m_project.scripts.first();
    return s;
}

bool Document::openProject(const QString &path, QString *error)
{
    Project loaded;
    if (!::loadProject(path, loaded, error)) return false;
    m_project = loaded;
    // A dangling activeId is as bad as an empty one and much harder to spot:
    // editing works because the active graph falls back to the first script,
    // but every undo snapshot is then stamped with an id no lookup can find.
    if (!m_project.script(m_project.activeId) && !m_project.scripts.isEmpty())
        m_project.activeId = m_project.scripts.first().id;
    m_selection.clear();
    abandonPendingEdit();
    m_undo.clear();
    m_redo.clear();
    setModified(false);
    emit projectChanged();
    emit activeScriptChanged();
    emit graphChanged();
    emit selectionChanged();
    return true;
}

bool Document::saveProject(const QString &path, QString *error)
{
    if (!::saveProject(m_project, path, error)) return false;
    m_project.path = path;
    setModified(false);
    return true;
}

void Document::resetToNew()
{
    m_project = newProject();
    m_selection.clear();
    abandonPendingEdit();
    m_undo.clear();
    m_redo.clear();
    setModified(false);
    emit projectChanged();
    emit activeScriptChanged();
    emit graphChanged();
    emit selectionChanged();
}

void Document::setActiveScript(const QString &id)
{
    if (m_project.activeId == id) return;
    if (!m_project.script(id)) return;
    m_project.activeId = id;
    m_selection.clear();
    // Undo history is per-graph; keeping it across a script switch would let an
    // undo silently rewrite a script the user is no longer looking at. An edit
    // still open belongs to the graph being left behind, so it goes too.
    abandonPendingEdit();
    m_undo.clear();
    m_redo.clear();
    emit activeScriptChanged();
    emit graphChanged();
    emit selectionChanged();
}

void Document::setSelection(const QStringList &nodeIds)
{
    if (m_selection == nodeIds) return;
    m_selection = nodeIds;
    emit selectionChanged();
}

void Document::abandonPendingEdit()
{
    // The depth is deliberately left alone: it tracks begin/commit calls, and
    // the caller that opened the edit still owes its commit.
    m_pendingValid = false;
    m_pending = Snapshot{};
}

void Document::beginEdit(const QString &label)
{
    // Counted, not a flag: a gesture that calls a helper which also brackets
    // its work would otherwise have its first inner commit close the outer
    // edit, and every later mutation of that gesture would emit no
    // graphChanged. The depth rises even with no graph to snapshot so the
    // matching commit still balances.
    if (++m_editDepth > 1) return;
    const ScriptEntry *s = activeEntry();
    if (!s) {
        abandonPendingEdit();
        return;
    }
    m_pending = {s->id, s->graph, label};
    m_pendingValid = true;
}

void Document::commitEdit()
{
    if (m_editDepth > 0 && --m_editDepth > 0) return; // inner commits close nothing
    if (m_pendingValid) {
        m_undo.append(m_pending);
        if (m_undo.size() > 128) m_undo.removeFirst();
        m_redo.clear();
        abandonPendingEdit();
    }
    // A commit with no matching begin, or one whose snapshot was dropped by an
    // undo, records no history. The caller still mutated the graph, so the
    // canvas and the analyser have to hear about it.
    setModified(true);
    emit graphChanged();
}

void Document::undo()
{
    // An in-flight edit snapshotted the state being undone; committing it later
    // would put that state straight back.
    abandonPendingEdit();
    // Check before popping. Taking the entry first and bailing when its script
    // is gone throws the step away in silence, and every later Ctrl+Z eats
    // another one. Entries for a script that no longer exists can never be
    // applied, so they are dropped until an applicable one is on top.
    while (!m_undo.isEmpty() && !m_project.script(m_undo.last().scriptId))
        m_undo.removeLast();
    if (m_undo.isEmpty()) return;
    const Snapshot snap = m_undo.takeLast();
    ScriptEntry *s = m_project.script(snap.scriptId);
    if (!s) return;
    m_redo.append({snap.scriptId, s->graph, snap.label});
    s->graph = snap.graph;
    if (m_project.activeId != snap.scriptId) {
        m_project.activeId = snap.scriptId;
        emit activeScriptChanged();
    }
    m_selection.clear();
    setModified(true);
    emit graphChanged();
    emit selectionChanged();
}

void Document::redo()
{
    abandonPendingEdit();
    while (!m_redo.isEmpty() && !m_project.script(m_redo.last().scriptId))
        m_redo.removeLast();
    if (m_redo.isEmpty()) return;
    const Snapshot snap = m_redo.takeLast();
    ScriptEntry *s = m_project.script(snap.scriptId);
    if (!s) return;
    m_undo.append({snap.scriptId, s->graph, snap.label});
    s->graph = snap.graph;
    if (m_project.activeId != snap.scriptId) {
        m_project.activeId = snap.scriptId;
        emit activeScriptChanged();
    }
    m_selection.clear();
    setModified(true);
    emit graphChanged();
    emit selectionChanged();
}

void Document::touchGraph()
{
    setModified(true);
    emit graphChanged();
}

void Document::setModified(bool value)
{
    if (m_modified == value) return;
    m_modified = value;
    emit modifiedChanged(value);
}
