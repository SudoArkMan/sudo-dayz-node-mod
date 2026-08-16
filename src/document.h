// The editing session: current project, catalog, selection, undo stack.
//
// Everything the UI mutates goes through here so panels stay in sync via
// signals rather than by knowing about each other. The canvas edits the
// active graph; the outliner, variables panel and inspector observe.
#pragma once

#include "builtins.h"
#include "catalog.h"
#include "project.h"

#include <QObject>
#include <QStringList>

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject *parent = nullptr);

    bool loadCatalog(const QString &jsonPath);
    const Catalog &catalog() const { return m_catalog; }
    const Builtins &builtins() const { return m_builtins; }

    // Resolves any node reference (catalogue key or builtin id) to a def,
    // applying per-node option overrides (Begin mode, cast target, ...).
    NodeDef defForNode(const GraphNode &node) const;
    NodeDef defForKey(const QString &key) const;

    Project &project() { return m_project; }
    const Project &project() const { return m_project; }
    Graph *activeGraph();
    const Graph *activeGraph() const;
    QString activeScriptId() const { return m_project.activeId; }

    bool openProject(const QString &path, QString *error = nullptr);
    bool saveProject(const QString &path, QString *error = nullptr);
    void resetToNew();
    bool isModified() const { return m_modified; }
    QString projectPath() const { return m_project.path; }

    // Selection lives here so the inspector and canvas agree on it.
    QStringList selection() const { return m_selection; }
    void setSelection(const QStringList &nodeIds);

    // Undo: snapshots of the active graph, coarse but reliable.
    //
    // Invariant: begin/commit nest and are depth counted, so one user gesture
    // produces exactly one undo entry and one graphChanged no matter how many
    // helpers it calls. Every beginEdit gets a matching commitEdit even when
    // there was no graph to snapshot. A snapshot records the script it was
    // taken from and is only ever applied to that script. An undo or redo
    // during an open edit abandons that edit's snapshot: it describes a state
    // the user has already stepped away from, and pushing it later would
    // resurrect what they just undid.
    void beginEdit(const QString &label);
    void commitEdit();
    void undo();
    void redo();
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    // Marks the graph dirty and re-runs analysis; emits graphChanged.
    void touchGraph();

    // Closing a script tab.
    //
    // A script has no life outside the project holding it: the tab bar is a view
    // of `project().scripts` and nothing else, so closing a tab takes the script
    // out of the project rather than hiding a window onto a file that carries on
    // existing. That is a removal, and it is not allowed to be a silent one. The
    // entry and the position it sat at go on a session stack that
    // reopenClosedScript pops, so every close has a way back inside the session,
    // and the window asks first for the scripts this project is the only copy of.
    //
    // The .c a script was imported from is never touched. What closing costs is
    // the graph, its layout and anything edited since the import.
    //
    // False when the id names no script, or when it names the last one: the
    // canvas and every dock read the active graph, and a project with no scripts
    // has none for them to read.
    bool closeScript(const QString &id);
    bool canReopenScript() const { return !m_closed.isEmpty(); }
    // The script the next reopen brings back, so the menu entry can name it.
    QString lastClosedName() const;
    bool reopenClosedScript();

public slots:
    void setActiveScript(const QString &id);

signals:
    void projectChanged();      // project loaded / scripts added or removed
    void activeScriptChanged();
    void graphChanged();        // nodes, edges, variables mutated
    void selectionChanged();
    void modifiedChanged(bool modified);

private:
    struct Snapshot { QString scriptId; Graph graph; QString label; };
    // A closed script and where in the tab order it was, so reopening puts it
    // back where the user left it rather than at the end of the bar.
    struct ClosedScript { ScriptEntry entry; int index = 0; };

    Catalog m_catalog;
    Builtins m_builtins;
    Project m_project;
    QStringList m_selection;
    QVector<Snapshot> m_undo;
    QVector<Snapshot> m_redo;
    // Session only, and never written to the .sdzn: a reopen is a way back out
    // of a mistake made a moment ago, not a second copy of the project.
    QVector<ClosedScript> m_closed;
    Snapshot m_pending;
    int m_editDepth = 0;
    bool m_pendingValid = false;
    bool m_modified = false;

    // The script the editor is really pointing at. Project::active() falls back
    // to the first script when activeId does not name one, so a snapshot has to
    // record where it actually came from or undo can never find it again.
    const ScriptEntry *activeEntry() const;
    void abandonPendingEdit();
    void setModified(bool value);
};
