// Live generated script, docked under the canvas.
//
// The graph is the source, but the .c file is what ships, so the answer to
// "what does this node actually do" should never be more than a glance away.
// Regenerates as the graph changes, highlighted the same way the editor is.
//
// Lines carry the node that produced them, so clicking a line selects that
// node on the canvas and selecting a node scrolls its lines into view.
#pragma once

#include <QWidget>

class Document;
class CodeEditor;
class QLabel;
class QToolButton;
class QTimer;

class CodeViewPanel : public QWidget {
    Q_OBJECT
public:
    explicit CodeViewPanel(Document *doc, QWidget *parent = nullptr);

    // Off by default when the dock is hidden; regenerating a big graph on every
    // keystroke is wasted work nobody can see.
    void setLive(bool live);
    bool isLive() const { return m_live; }

public slots:
    // Queues a regeneration. Cheap to call often; the work is debounced.
    void scheduleRefresh();
    // Scrolls to and highlights the lines a node produced.
    void revealNode(const QString &nodeId);

signals:
    // The user clicked a line; the canvas should select and frame this node.
    void nodeActivated(const QString &nodeId);

private slots:
    void regenerate();
    void onCursorMoved();
    void copyAll();
    void saveAs();

private:
    Document *m_doc;
    CodeEditor *m_editor;
    QLabel *m_header;     // class name, line count, warning count
    QLabel *m_warnings;
    QTimer *m_debounce;
    QToolButton *m_liveToggle;
    bool m_live = true;
    // Parallel to the visible lines: which node produced each one.
    QVector<QString> m_lineOwners;
    QString m_revealed;   // node whose lines are currently marked
    bool m_syncingCursor = false;
};
