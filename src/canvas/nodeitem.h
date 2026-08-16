// One node on the canvas.
//
// Draws the coloured header bar, title/subtitle, and two pin columns with
// inline editors for unconnected literal inputs. Pin geometry is computed
// once per rebuild and exposed so wires and hit-testing agree with painting.
//
// Nodes that carry hand-written Enforce (the raw family) also draw the code
// itself, highlighted, and size themselves to it. A project can hold hundreds
// of those, and a fixed box titled "Raw Enforce" tells the reader nothing
// about which one they are looking at.
//
// A node that came from a mod dependency wears its short name in the header,
// "COT" or "CF", in that dependency's colour. Only those: a vanilla node gets
// no tag, because 29,000 nodes carrying a "DayZ" badge is noise rather than
// information, and what a reader needs to know is which of them is the one
// that will not compile on a server missing a mod.
//
// What a node says about itself, and where each piece of it goes:
//
//   header      the name, and the class that declares it
//   exec band   the declaration's own first sentence, when it has one, in the
//               row that otherwise holds two triangles and nothing else
//   pin rows    the name of each pin and the type it carries, spelled the way
//               the declaration spelled it
//   tooltip     the whole signature, the source location, and the cautions
//
// Nothing in that list makes a node taller or wider than it was: every word is
// written into room the node already spends. Height is the scarce thing on this
// canvas, because a graph is read at 75 percent zoom with several hundred nodes
// on it, and a node that grows to explain itself makes its neighbours harder to
// find than the explanation is worth. The long form lives in the Inspector and
// in the tooltip, both of which are asked for rather than glanced at.
#pragma once

#include "analysis.h"
#include "graph.h"
#include "nodeinputs.h"
#include "theme.h"

#include <QColor>
#include <QGraphicsObject>
#include <QVector>

class Document;
class QPainterPath;

class NodeItem : public QGraphicsObject {
    Q_OBJECT
public:
    NodeItem(Document *doc, const QString &nodeId, QGraphicsItem *parent = nullptr);

    QString nodeId() const { return m_nodeId; }
    // Re-reads the node from the graph and recomputes layout.
    void refresh();
    void setDiagnostics(const QVector<Diagnostic> &diags);

    // Scene position of a pin's connection point.
    QPointF pinScenePos(const QString &pinId, PinDir dir) const;
    // Pin under a scene position, or an empty id when none is close enough.
    // `reach` is in scene units; the view passes a value derived from a fixed
    // number of screen pixels so the target does not shrink as you zoom out.
    // Pass the distance back to compare candidates across several nodes.
    QString pinAt(const QPointF &scenePos, PinDir *dirOut, double reach = 0.0,
                  double *distanceOut = nullptr) const;

    // The inline value field under a scene position, or an empty pin id. Same
    // shape as pinAt and for the same reason: a field is nine scene units tall,
    // which at the 0.75 zoom a long graph fits at is a seven pixel target, so
    // `reach` comes in as a distance derived from screen pixels rather than
    // being fixed in scene units.
    QString editorAt(const QPointF &scenePos, double reach = 0.0) const;

    // Which of the two element controls is under a scene position: -1 for
    // minus, +1 for plus, 0 for neither. Only a node whose definition declares
    // a pin list draws them at all.
    int listButtonAt(const QPointF &scenePos, double reach = 0.0) const;

    // Every data input of this node, for a panel that wants a row per pin
    // without reading the private pin layout. Writes go back through
    // setNodeInput, the same helper the canvas uses.
    QVector<NodeInput> editableInputs() const;

    // Ring the pin the cursor is near, so the target is visible before the
    // press rather than discovered by missing it.
    void setHoverPin(const QString &pinId, PinDir dir);
    void clearHoverPin();
    // Highlight the value field the cursor is over. Empty clears it.
    void setHoverEditor(const QString &pinId);
    // Marks pins a wire being dragged could legally land on.
    void setDropCandidate(bool candidate);
    const NodeDef &def() const { return m_def; }
    // The drawn body, without the painting margin boundingRect adds. Layout
    // operations align to this, not to the halo.
    QRectF bodyRect() const;

    QRectF boundingRect() const override;
    // Tighter than boundingRect: the body plus a cap over each pin, so a press
    // just outside a node starts a marquee instead of dragging the node.
    QPainterPath shape() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *opt,
               QWidget *w = nullptr) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *e) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override;

private:
    struct PinLayout {
        Pin pin;
        QPointF pos;     // local connection point
        QRectF editor;   // inline editor rect, empty when not editable
        // What a press has to land in to count as the field. Wider and taller
        // than the drawn box: the label belongs to the same value, and the four
        // units of gap above and below the box were pure miss.
        QRectF hit;
        // Room this pin's label may take. Decided here rather than at paint
        // time, because an output label and an input's value field share a row
        // and only the layout knows where the field ended up.
        double labelRoom = 0.0;
        // The type this pin carries, in the declaration's own spelling, or
        // empty when the row says it another way. Resolved at layout time
        // because it costs a signature lookup and a repaint must not.
        QString typeText;
        bool connected = false;
    };

    QString m_hoverPin;
    PinDir m_hoverDir = PinDir::In;
    QString m_hoverEditor;
    // -1, 0 or +1, the same reading listButtonAt returns.
    int m_hoverListButton = 0;
    bool m_dropCandidate = false;

    // One coloured stretch of a code line. Measured once at layout time so a
    // repaint is drawing only, with no lexing and no catalogue lookups.
    struct CodeRun {
        QString text;
        QColor color;
        double x = 0.0;   // from the left edge of the code column
    };
    struct CodeLine {
        QVector<CodeRun> runs;
        double cutX = 0.0;     // where the cut marker goes, when elided is set
        bool elided = false;   // too wide for the node, cut with an ellipsis
    };

    Document *m_doc;
    QString m_nodeId;
    NodeDef m_def;
    QVector<PinLayout> m_pins;
    QVector<Diagnostic> m_diags;
    double m_width = theme::node::width;
    double m_headerHeight = theme::node::headerHeight;
    double m_height = 0;
    QString m_title;
    QString m_subtitle;
    // The author's own comment, drawn above the header. Empty on a node that
    // carries none, which is every node placed from the palette.
    QString m_note;

    QVector<CodeLine> m_code;    // empty on every node that is not a code node
    int m_codeHidden = 0;        // lines past the cap, counted in the footer
    double m_codeTop = 0.0;
    double m_codeLineHeight = 0.0;
    bool m_pinsOnHeader = false; // pins share the header row, so it needs clearance

    // The one sentence the declaration says about itself, and the strip of the
    // exec row it is written across. Both empty on the three nodes in four that
    // Bohemia never documented, and on every node with no exec row to spare.
    QString m_summary;
    QRectF m_summaryRect;
    // Where the rule between the flow rows and the data rows goes, or zero when
    // the node has only one of the two and there is nothing to separate.
    double m_bandRule = 0.0;

    // The tooltip is the long answer, so it is built on the first hover rather
    // than for all 496 nodes of a project at load. Cleared by anything that
    // changes what it would say.
    bool m_tipReady = false;

    // The footer a variable-arity node draws: "3 elements" on the left, minus
    // and plus on the right. Empty rects on every node with a fixed shape.
    QRectF m_listRow;
    QRectF m_listMinus;
    QRectF m_listPlus;
    int m_listCount = 0;

    // The dependency tag, empty on a vanilla node, and the colour it draws in.
    QString m_sourceTag;
    QColor m_sourceColor;

    void layoutCode(const GraphNode &node);
    // Reads the dependency a node's key names and sets the two members above.
    void resolveSource(const GraphNode &node);
    void layoutPins();
    // Room the dependency tag needs, zero when there is none. Measured here
    // rather than at each use so the width the layout reserves and the pill the
    // painter draws cannot disagree.
    double sourceTagWidth() const;
    // Width the node's own text needs, before the clamp. `node` is what the
    // author typed into the value fields, which is measured rather than the
    // declaration's defaults.
    double contentWidth(const QVector<Pin> &dataIn, const QVector<Pin> &dataOut,
                        const GraphNode *node) const;
    const PinLayout *layoutForPin(const QString &pinId) const;
    // Acts on one value field the way its type asks: a bool flips, an enum
    // offers its members, everything else prompts for text. `host` parents any
    // popup and `at` is where it opens, both in screen coordinates.
    void activateEditor(const QString &pinId, QWidget *host, const QPoint &at);
    void paintValueField(QPainter *p, const PinLayout &pl, const QString &value) const;
    // The pin's name and the type it carries, sharing one row's worth of room.
    void paintPinText(QPainter *p, const PinLayout &pl) const;
    void paintListRow(QPainter *p) const;
    // The declaration's own sentence, across the exec row, and the rule under
    // the flow rows. Both are no-ops on a node that has neither.
    void paintSummary(QPainter *p) const;
    // Everything the catalogue knows, for a reader who stopped to ask.
    QString buildTooltip() const;
    // Height of the code block including its own padding; 0 when there is none.
    double codeBlockHeight() const;
    // `bodyPath` is the node's rounded outline; the well is clipped to it so the
    // bottom corners stay round.
    void paintCode(QPainter *p, const QPainterPath &bodyPath) const;
};
