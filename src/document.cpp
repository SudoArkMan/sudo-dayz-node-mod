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
    // Those scripts belong to the project being left. Reopening one here would
    // graft another project's class into this one.
    m_closed.clear();
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
    m_closed.clear();
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

bool Document::closeScript(const QString &id)
{
    int index = -1;
    for (int i = 0; i < m_project.scripts.size(); ++i) {
        if (m_project.scripts.at(i).id != id) continue;
        index = i;
        break;
    }
    if (index < 0) return false;
    // The window has no empty state and does not need one: the canvas, the
    // outliner, the variables panel and the generator all read the active graph,
    // and there is nowhere for the user to go from a project with no scripts
    // except File > New project, which they can reach anyway.
    if (m_project.scripts.size() < 2) return false;

    m_closed.append({m_project.scripts.at(index), index});
    // Deep enough to walk back a bulk close, shallow enough that a session
    // browsing a big mod is not holding every graph it ever looked at.
    if (m_closed.size() > 16) m_closed.removeFirst();
    m_project.scripts.removeAt(index);

    // Asked of the project after the removal, so it covers an activeId that was
    // already dangling as well as the tab that has just gone. Either way the
    // editor is pointing at nothing and has to be pointed somewhere.
    const bool activeGone = !m_project.script(m_project.activeId);
    if (activeGone) {
        // The tab that took its place, which is the one now under the cursor,
        // and the last tab when the closed one was last.
        const int next = qMin(index, m_project.scripts.size() - 1);
        m_project.activeId = m_project.scripts.at(next).id;
    }

    // Snapshots of the script that has gone. Undo already skips entries whose
    // script no lookup can find, but a reopen makes them applicable again, and
    // the first Ctrl+Z after that would put the graph back to a state the user
    // stepped away from before they ever closed it.
    const auto dropSnapshots = [&id](QVector<Snapshot> &stack) {
        for (int i = stack.size() - 1; i >= 0; --i)
            if (stack.at(i).scriptId == id) stack.removeAt(i);
    };
    dropSnapshots(m_undo);
    dropSnapshots(m_redo);
    if (m_pendingValid && m_pending.scriptId == id) abandonPendingEdit();

    // The rest of the history belongs to scripts that are still here, and
    // closing a tab in the background is not a reason to take a user's undo off
    // the graph they are looking at. Moving the editor to another script is,
    // and it carries exactly the rule setActiveScript carries.
    if (activeGone) {
        m_selection.clear();
        abandonPendingEdit();
        m_undo.clear();
        m_redo.clear();
    }

    // The project now holds one script fewer than the .sdzn on disk does, so it
    // is behind again and Save is what makes the close permanent.
    setModified(true);
    emit projectChanged();
    if (activeGone) emit activeScriptChanged();
    emit graphChanged();
    emit selectionChanged();
    return true;
}

QString Document::lastClosedName() const
{
    return m_closed.isEmpty() ? QString() : m_closed.last().entry.name;
}

bool Document::reopenClosedScript()
{
    if (m_closed.isEmpty()) return false;
    ClosedScript closed = m_closed.takeLast();
    // An id the project has grown since, from an import or another reopen. Two
    // entries under one id is worse than a renumbered one: every lookup in the
    // app takes the first match, so the tab, the undo stack and the exporter
    // would each be free to pick a different script.
    if (m_project.script(closed.entry.id)) {
        do {
            closed.entry.id = nextId(QStringLiteral("s"));
        } while (m_project.script(closed.entry.id));
    }
    const int at = qBound(0, closed.index, m_project.scripts.size());
    m_project.scripts.insert(at, closed.entry);
    m_project.activeId = closed.entry.id;

    m_selection.clear();
    abandonPendingEdit();
    m_undo.clear();
    m_redo.clear();
    setModified(true);
    emit projectChanged();
    emit activeScriptChanged();
    emit graphChanged();
    emit selectionChanged();
    return true;
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
