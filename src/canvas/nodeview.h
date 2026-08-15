// The canvas viewport: grid, pan/zoom, marquee selection, wire dragging, the
// right-click menu, and the drop target for nodes dragged out of the palette
// and variables dragged out of the variables panel.
#pragma once

#include <QGraphicsView>

class NodeScene;

// The drag payload the palette writes and this view accepts: the bare node key
// and nothing else. Declared here because the drop side owns the contract.
QString nodeDragMimeType();
// The variables panel's payload: a variable id. Get or set is decided at the
// drop, so the id is all the panel is allowed to say.
QString variableDragMimeType();

class NodeView : public QGraphicsView {
    Q_OBJECT
public:
    explicit NodeView(NodeScene *scene, QWidget *parent = nullptr);

    void zoomToFit();
    void resetZoom();
    void centerOnNode(const QString &nodeId);
    double zoom() const { return m_zoom; }

signals:
    void zoomChanged(double zoom);
    void contextAddRequested(const QPointF &scenePos);
    // Sweeping every raw node in a script is a big enough edit that the window
    // owns the policy for it. Nothing connected means the view runs the sweep
    // itself, so the menu entry is never dead.
    void convertAllRequested();

protected:
    void drawBackground(QPainter *p, const QRectF &rect) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    // Tracks which pin a press would grab, for the halo and the cursor.
    void updateHoveredPin(const QPoint &viewPos);

    QString m_hoverNode;
    QString m_hoverPin;

    NodeScene *m_scene;
    double m_zoom = 1.0;
    bool m_panning = false;
    QPoint m_panAnchor;
    // Space latches hand-drag; this remembers what to go back to so a press
    // with no drag behind it cannot strand the view in pan mode.
    bool m_spacePanning = false;

    void applyZoom(double factor, const QPoint &anchor);
    // Offers "Get <name>" and "Set <name>" for a dropped variable. Shown after
    // the drop has been accepted, never during it: a menu inside dropEvent runs
    // a nested event loop while the source's drag loop is still live.
    void showVariableDropMenu(const QString &variableId, const QPointF &scenePos,
                              const QPoint &globalPos);
};
