// The graph scene: owns node items, wire items and note items, and keeps
// them in sync with the Document's active graph.
//
// The scene is the only place that mutates the graph in response to canvas
// interaction; it calls Document::beginEdit/commitEdit around each change so
// undo works uniformly.
#pragma once

#include "analysis.h"
#include "document.h"

#include <QGraphicsScene>
#include <QHash>
#include <QPointF>

class NodeItem;
class WireItem;
class NoteItem;

// A pin the user grabbed, identified across rebuilds.
struct PinRef {
    QString nodeId;
    QString pinId;
    PinDir dir = PinDir::Out;
    bool valid = false;
};

// True when a wire leaving a pin of `type` in direction `dir` could land on
// `candidate`. Object pins are only settled by the inheritance chain, so the
// catalogue has to come in with the question. This is the one rule the canvas
// and the connect-from-drag menu both answer to; a menu that offered a row the
// canvas then refused would be worse than no menu.
bool pinWouldFit(const Catalog &cat, const PinType &type, PinDir dir,
                 const Pin &candidate);

// Which edge of the selection's bounding box the nodes line up on. CentreX
// stacks them on one vertical axis, CentreY on one horizontal axis.
enum class AlignEdge { Left, Right, Top, Bottom, CentreX, CentreY };

class NodeScene : public QGraphicsScene {
    Q_OBJECT
public:
    explicit NodeScene(Document *doc, QObject *parent = nullptr);

    // Full rebuild from the active graph. Cheap enough for 29k-node catalogs
    // because only instantiated nodes exist as items.
    void rebuild();
    // Re-reads positions/labels without recreating items.
    void refreshVisuals();
    void setAnalysis(const AnalysisResult &result);

    NodeItem *itemForNode(const QString &id) const;
    // Adds a node of `key` (catalog key or builtin id) at scene position.
    void addNodeAt(const QString &key, const QPointF &scenePos);
    // The same, wired to `from` on the way in: the new node lands, its best
    // matching pin takes the wire, and the whole thing is one undo step.
    // Returns the new node id, or an empty string when nothing was placed.
    QString addNodeConnectedTo(const QString &key, const QPointF &scenePos,
                               const PinRef &from);
    // Declares a member shaped like the dragged pin, places its Get node (for a
    // drag off an input) or Set node (off an output) and wires it. One undo
    // step. Returns the new variable's id, empty when nothing was created.
    QString promoteToVariable(const PinRef &from, const QPointF &scenePos);
    void deleteSelectedNodes();
    void duplicateSelection();

    // Raw code back into a graph. The raw node is replaced by the nodes its
    // code lowers to, and the exec chain it was carrying is spliced onto them.
    // Nothing changes and no undo step is pushed when the code cannot be
    // represented yet; either way statusMessage says what happened.
    bool convertRawToNodes(const QString &nodeId, QStringList *notes = nullptr);
    // The same over every raw node in the active graph, as ONE undo step.
    // Returns how many nodes it managed to convert.
    int convertAllRaw(QStringList *notes = nullptr);
    bool isRawCodeNode(const QString &nodeId) const;

    // Name of a graph variable by id. A menu that has to read "Get health"
    // before the node exists has nowhere else to get the word from.
    QString variableName(const QString &variableId) const;

    // Layout, over the current selection. Each is one undo step; each reports
    // through statusMessage when the selection is too small to act on.
    void alignSelection(AlignEdge edge);
    // Equalises the gaps between the selected items, leaving the two outermost
    // where they are.
    void distributeSelection(Qt::Orientation orientation);
    // Slides each selected node along y until one of its wired pins is level
    // with the pin at the other end, so the wire runs flat.
    void straightenWires();

    // Wire dragging, driven by NodeView.
    void beginWireDrag(const PinRef &from, const QPointF &scenePos);
    void updateWireDrag(const QPointF &scenePos);
    // Completes on a pin, or cancels when `to` is invalid.
    void finishWireDrag(const PinRef &to);
    // A release that hit no pin at all. The graph is left exactly as it was and
    // wireDroppedOnEmpty carries the drop position, so the window can offer the
    // nodes that would fit there. Escape still goes through finishWireDrag, so
    // an abandoned drag stays silent.
    void finishWireDragOnEmpty(const QPointF &scenePos);
    bool isDraggingWire() const { return m_dragging; }
    PinRef dragSource() const { return m_dragFrom; }

    // True when a connection would be accepted, including class compatibility.
    bool wouldConnect(const PinRef &a, const PinRef &b) const;
    // The type of the pin a ref names, or a default type when the node it named
    // has gone away.
    PinType typeOfPin(const PinRef &ref) const;

    // Node dragging. NodeView brackets the gesture and the items report their
    // own movement, so wires follow live while the graph is written once on
    // release, leaving the whole drag as one undo step.
    void beginNodeMove();
    void nodeMoved(const QString &nodeId);
    void endNodeMove();
    // Items cannot emit the scene's signals themselves.
    void emitNodeDoubleClicked(const QString &nodeId);

    QRectF graphBounds() const;

signals:
    void nodeDoubleClicked(const QString &nodeId);
    void statusMessage(const QString &text);
    // A wire was dragged off `from` and let go over empty canvas.
    void wireDroppedOnEmpty(const PinRef &from, const QPointF &scenePos);

private slots:
    void onSelectionChanged();

private:
    Document *m_doc;
    QHash<QString, NodeItem *> m_nodes;
    QVector<WireItem *> m_wires;
    QVector<NoteItem *> m_notes;
    AnalysisResult m_analysis;

    bool m_dragging = false;
    PinRef m_dragFrom;
    WireItem *m_dragWire = nullptr;

    bool m_movingNodes = false; // a drag gesture is in progress
    bool m_moveDirty = false;   // ...and something actually moved
    bool m_syncing = false;     // selection is being mirrored, do not echo it

    // Resolves a key to the node it would place, with its id, position and
    // declared defaults already set. False when the key names nothing, in which
    // case the reason has been reported through statusMessage.
    bool prepareNode(const QString &key, const QPointF &scenePos, GraphNode *node,
                     NodeDef *def);
    // Drops the preview wire and clears the drag state, handing back the pin
    // the gesture started from.
    PinRef endWireDrag();
    void rebuildWires();
    void updateWiresFor(const QString &nodeId);
    void syncSelectionToDocument();
    void applyDocumentSelection();
};
