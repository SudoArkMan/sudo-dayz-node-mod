#include "nodeview.h"

#include "nodeitem.h"
#include "nodescene.h"
#include "theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaMethod>
#include <QMimeData>
#include <QMouseEvent>
#include <limits>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

namespace {

constexpr double kMinZoom = 0.25;
constexpr double kMaxZoom = 3.0;
// Below this, node titles stop being readable and the graph is just shapes.
constexpr double kLegibleZoom = 0.75;
constexpr double kZoomStep = 1.12;
// Minor grid in scene units; every fifth line is a major one.
constexpr double kGridMinor = 20.0;
constexpr int kMajorEvery = 5;
// Below this the minor grid is a grey wash that costs more than it says.
constexpr double kMinorCutoff = 0.5;
// Scene units of canvas left above and to the left of a graph that is opened
// too big to frame. Enough to read as a margin, little enough that the first
// node is still the first thing in the corner.
constexpr double kOpeningMargin = 60.0;

// A pin under the cursor, or an invalid ref. Pins overhang the node body, so
// this walks every item at the point rather than trusting the topmost one.
// How close the cursor has to be, in SCREEN pixels. Working in scene units
// made the target shrink with the zoom, so at the 0.75 a long graph fits at,
// a pin was about five pixels wide and most drags became marquees.
constexpr int kPinGrabPixels = 14;

PinRef pinRefAt(const QGraphicsView *view, const QPoint &viewPos)
{
    const QPointF scenePos = view->mapToScene(viewPos);
    // One pixel measured out into the scene, so the reach stays constant on
    // screen whatever the zoom.
    const double unitsPerPixel =
        QLineF(scenePos, view->mapToScene(viewPos + QPoint(1, 0))).length();
    const double reach = kPinGrabPixels * qMax(unitsPerPixel, 0.0001);

    // Search a box, not a point: an item is only returned for a point inside
    // its shape, and the nearest pin is often just outside the one under the
    // cursor.
    const QRect box(viewPos - QPoint(kPinGrabPixels, kPinGrabPixels),
                    QSize(kPinGrabPixels * 2, kPinGrabPixels * 2));
    PinRef best;
    double bestDist = std::numeric_limits<double>::max();
    for (QGraphicsItem *it : view->items(box)) {
        auto *node = dynamic_cast<NodeItem *>(it);
        if (!node) continue;
        PinDir dir = PinDir::In;
        double dist = 0.0;
        const QString pinId = node->pinAt(scenePos, &dir, reach, &dist);
        if (pinId.isEmpty() || dist >= bestDist) continue;
        bestDist = dist;
        best = PinRef{node->nodeId(), pinId, dir, true};
    }
    return best;
}

// A payload this view knows how to turn into a node. Both drags come in as
// Copy, so the proposed action is always the right one to accept.
bool acceptableDrag(const QMimeData *mime)
{
    return mime && (mime->hasFormat(nodeDragMimeType())
                    || mime->hasFormat(variableDragMimeType()));
}

// The node under a viewport position, or an empty id. The topmost item at a
// point can be a wire or a pin cap, so this walks the whole hit list.
QString nodeIdAt(const QGraphicsView *view, const QPoint &viewPos)
{
    const QList<QGraphicsItem *> hits = view->items(viewPos);
    for (QGraphicsItem *it : hits)
        if (auto *node = dynamic_cast<NodeItem *>(it)) return node->nodeId();
    return QString();
}

// Viewport size the last framing was measured against, kept on the view as a
// dynamic property. Every caller asks for a fit in the same turn as the switch
// that gives the canvas its real size, so the size a fit is computed from is
// routinely a layout out of date.
constexpr char kFitMeasure[] = "sudoFitViewport";

// Frames a rect and returns the zoom that survived the clamp.
double fitToRect(QGraphicsView *view, const QRectF &rect)
{
    view->fitInView(rect, Qt::KeepAspectRatio);
    const double fitted = view->transform().m11();
    const double clamped = qBound(kMinZoom, fitted, kMaxZoom);
    if (!qFuzzyCompare(fitted, clamped)) view->scale(clamped / fitted, clamped / fitted);
    return view->transform().m11();
}

} // namespace

QString nodeDragMimeType()
{
    return QStringLiteral("application/x-sudo-node");
}

QString variableDragMimeType()
{
    return QStringLiteral("application/x-sudo-variable");
}

NodeView::NodeView(NodeScene *scene, QWidget *parent)
    : QGraphicsView(scene, parent), m_scene(scene)
{
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setRubberBandSelectionMode(Qt::IntersectsItemShape);
    // Zoom is anchored by hand in applyZoom, so the view must not also try.
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(theme::canvasBg());
    setMouseTracking(true);
    setAcceptDrops(true);
}

void NodeView::drawBackground(QPainter *p, const QRectF &rect)
{
    p->fillRect(rect, theme::canvasBg());
    p->setRenderHint(QPainter::Antialiasing, false);

    const double major = kGridMinor * kMajorEvery;
    QVector<QLineF> lines;

    if (m_zoom >= kMinorCutoff) {
        QPen pen(theme::gridMinor());
        pen.setCosmetic(true);
        p->setPen(pen);
        for (double x = std::floor(rect.left() / kGridMinor) * kGridMinor;
             x < rect.right(); x += kGridMinor) {
            if (std::fmod(std::fabs(x), major) < 0.001) continue; // the major pass draws it
            lines.append(QLineF(x, rect.top(), x, rect.bottom()));
        }
        for (double y = std::floor(rect.top() / kGridMinor) * kGridMinor;
             y < rect.bottom(); y += kGridMinor) {
            if (std::fmod(std::fabs(y), major) < 0.001) continue;
            lines.append(QLineF(rect.left(), y, rect.right(), y));
        }
        p->drawLines(lines);
        lines.clear();
    }

    QPen pen(theme::gridMajor());
    pen.setCosmetic(true);
    p->setPen(pen);
    for (double x = std::floor(rect.left() / major) * major; x < rect.right(); x += major)
        lines.append(QLineF(x, rect.top(), x, rect.bottom()));
    for (double y = std::floor(rect.top() / major) * major; y < rect.bottom(); y += major)
        lines.append(QLineF(rect.left(), y, rect.right(), y));
    p->drawLines(lines);
}

void NodeView::applyZoom(double factor, const QPoint &anchor)
{
    const double target = qBound(kMinZoom, m_zoom * factor, kMaxZoom);
    const double step = target / m_zoom;
    if (qFuzzyCompare(step, 1.0)) return;

    // Keep whatever sits under `anchor` where it is, so zooming reads as
    // moving towards a thing rather than towards the middle of the window.
    const QPointF before = mapToScene(anchor);
    scale(step, step);
    m_zoom = target;
    const QPointF drift = mapToScene(anchor) - before;
    centerOn(mapToScene(viewport()->rect().center()) - drift);

    emit zoomChanged(m_zoom);
}

void NodeView::zoomToFit()
{
    const QRectF bounds = m_scene ? m_scene->graphBounds() : QRectF();
    if (bounds.isNull() || bounds.isEmpty()) {
        resetZoom();
        return;
    }
    const double fitted = fitToRect(this, bounds.adjusted(-60, -60, 60, 60));

    // A long event chain fits only at a zoom where nothing is readable, and a
    // wall of unreadable boxes is a worse answer than showing the start of the
    // graph at a working size. Below the legibility floor, open where the work
    // begins instead: the top-left of the graph, at a readable zoom.
    if (fitted < kLegibleZoom) {
        setTransform(QTransform::fromScale(kLegibleZoom, kLegibleZoom));
        m_zoom = kLegibleZoom;
        const QSizeF viewSize = viewport()->size() / kLegibleZoom;
        centerOn(bounds.left() + viewSize.width() / 2 - kOpeningMargin,
                 bounds.top() + viewSize.height() / 2 - kOpeningMargin);
    } else {
        m_zoom = fitted;
    }
    emit zoomChanged(m_zoom);

    // Opening a project switches to the editor, shows every dock and asks for
    // this, all in one turn, and the layout for that has not run yet: the size
    // read above is the one the viewport had before any of it. Measuring again
    // next turn is what puts the graph where the user can see it instead of
    // off the corner of a canvas that turned out to be a different shape. One
    // turn agreeing with the last is the end of it.
    const QSize measured = viewport()->size();
    if (property(kFitMeasure).toSize() == measured) {
        setProperty(kFitMeasure, QVariant());
        return;
    }
    setProperty(kFitMeasure, measured);
    QTimer::singleShot(0, this, [this]() { zoomToFit(); });
}

void NodeView::resetZoom()
{
    const QPointF centre = mapToScene(viewport()->rect().center());
    setTransform(QTransform());
    m_zoom = 1.0;
    centerOn(centre);
    emit zoomChanged(m_zoom);
}

void NodeView::centerOnNode(const QString &nodeId)
{
    if (!m_scene) return;
    QGraphicsItem *item = m_scene->itemForNode(nodeId);
    if (!item) return;
    // Framing a node the user cannot read is not framing it.
    if (m_zoom < 0.6) applyZoom(0.6 / m_zoom, viewport()->rect().center());
    centerOn(item);
}

void NodeView::wheelEvent(QWheelEvent *e)
{
    const int delta = e->angleDelta().y();
    if (delta == 0) return;
    applyZoom(delta > 0 ? kZoomStep : 1.0 / kZoomStep, e->position().toPoint());
    e->accept();
}

void NodeView::mousePressEvent(QMouseEvent *e)
{
    const QPoint viewPos = e->position().toPoint();

    if (e->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panAnchor = viewPos;
        viewport()->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }

    if (e->button() == Qt::LeftButton && dragMode() != QGraphicsView::ScrollHandDrag) {
        const PinRef pin = pinRefAt(this, viewPos);
        if (pin.valid && m_scene) {
            m_scene->beginWireDrag(pin, mapToScene(viewPos));
            e->accept();
            return;
        }
        // Anything else may turn into a node drag; the scene brackets it so
        // the whole gesture lands on the undo stack as one edit.
        if (m_scene) m_scene->beginNodeMove();
    }

    QGraphicsView::mousePressEvent(e);
}

void NodeView::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint viewPos = e->position().toPoint();

    if (m_panning) {
        const QPoint delta = viewPos - m_panAnchor;
        m_panAnchor = viewPos;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        e->accept();
        return;
    }

    if (m_scene && m_scene->isDraggingWire()) {
        m_scene->updateWireDrag(mapToScene(viewPos));
        updateHoveredPin(viewPos);
        e->accept();
        return;
    }

    updateHoveredPin(viewPos);
    QGraphicsView::mouseMoveEvent(e);
}

// Ring whichever pin a press would grab, and say so with the cursor. The grab
// area is bigger than the drawn dot, so without this the user has to find it by
// missing it.
void NodeView::updateHoveredPin(const QPoint &viewPos)
{
    if (!m_scene) return;
    const PinRef pin = pinRefAt(this, viewPos);
    if (pin.nodeId == m_hoverNode && pin.pinId == m_hoverPin) return;

    if (NodeItem *previous = m_scene->itemForNode(m_hoverNode))
        previous->clearHoverPin();
    m_hoverNode = pin.nodeId;
    m_hoverPin = pin.pinId;
    if (NodeItem *item = m_scene->itemForNode(m_hoverNode))
        item->setHoverPin(pin.pinId, pin.dir);

    viewport()->setCursor(pin.valid ? Qt::CrossCursor : Qt::ArrowCursor);
}

void NodeView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_panning && e->button() == Qt::MiddleButton) {
        m_panning = false;
        viewport()->unsetCursor();
        e->accept();
        return;
    }

    if (m_scene && m_scene->isDraggingWire() && e->button() == Qt::LeftButton) {
        const QPoint viewPos = e->position().toPoint();
        // A release on a pin connects or reports why it cannot. A release that
        // reached no pin at all is a request, not a mistake: the scene passes it
        // on so the window can offer what would fit here.
        const PinRef target = pinRefAt(this, viewPos);
        if (target.valid) m_scene->finishWireDrag(target);
        else m_scene->finishWireDragOnEmpty(mapToScene(viewPos));
        e->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(e);

    if (e->button() == Qt::LeftButton) {
        if (m_scene) m_scene->endNodeMove();
        // A space-drag that has already been let go of ends here; one that is
        // still held keeps panning until the key comes up.
        if (!m_spacePanning && dragMode() == QGraphicsView::ScrollHandDrag)
            setDragMode(QGraphicsView::RubberBandDrag);
    }
}

void NodeView::keyPressEvent(QKeyEvent *e)
{
    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);

    switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (m_scene) m_scene->deleteSelectedNodes();
        e->accept();
        return;
    case Qt::Key_D:
        if (ctrl) {
            if (m_scene) m_scene->duplicateSelection();
            e->accept();
            return;
        }
        break;
    case Qt::Key_F: {
        if (ctrl || !m_scene) {
            zoomToFit();
            e->accept();
            return;
        }
        QRectF bounds;
        const QList<QGraphicsItem *> sel = m_scene->selectedItems();
        for (QGraphicsItem *it : sel)
            bounds = bounds.isNull() ? it->sceneBoundingRect()
                                     : bounds.united(it->sceneBoundingRect());
        if (bounds.isNull()) {
            zoomToFit();
        } else {
            m_zoom = fitToRect(this, bounds.adjusted(-120, -120, 120, 120));
            emit zoomChanged(m_zoom);
        }
        e->accept();
        return;
    }
    case Qt::Key_Space:
        if (!e->isAutoRepeat()) {
            m_spacePanning = true;
            setDragMode(QGraphicsView::ScrollHandDrag);
        }
        e->accept();
        return;
    case Qt::Key_Escape:
        if (m_scene && m_scene->isDraggingWire()) {
            m_scene->finishWireDrag(PinRef{});
            e->accept();
            return;
        }
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        applyZoom(kZoomStep, viewport()->rect().center());
        e->accept();
        return;
    case Qt::Key_Minus:
        applyZoom(1.0 / kZoomStep, viewport()->rect().center());
        e->accept();
        return;
    case Qt::Key_0:
        if (ctrl) {
            resetZoom();
            e->accept();
            return;
        }
        break;
    default:
        break;
    }

    QGraphicsView::keyPressEvent(e);
}

void NodeView::keyReleaseEvent(QKeyEvent *e)
{
    // ScrollHandDrag swallows selection outright, so leaving it latched after a
    // stray Space press costs the user marquee select with no way to notice why.
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat() && m_spacePanning) {
        m_spacePanning = false;
        // Mid-drag the button is still down; mouseReleaseEvent restores the
        // drag mode so the gesture the user started is allowed to finish.
        if (!(QApplication::mouseButtons() & Qt::LeftButton))
            setDragMode(QGraphicsView::RubberBandDrag);
        e->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(e);
}

void NodeView::contextMenuEvent(QContextMenuEvent *e)
{
    const QPointF scenePos = mapToScene(e->pos());
    const QString nodeId = nodeIdAt(this, e->pos());
    const bool raw = m_scene && !nodeId.isEmpty() && m_scene->isRawCodeNode(nodeId);
    if (!raw) {
        emit contextAddRequested(scenePos);
        e->accept();
        return;
    }

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction *convert = menu.addAction(QStringLiteral("Convert to nodes"));
    convert->setToolTip(QStringLiteral(
        "Read the code and replace this node with the graph it describes."));
    QAction *convertAll = menu.addAction(QStringLiteral("Convert all raw code"));
    menu.addSeparator();
    QAction *add = menu.addAction(QStringLiteral("Add node..."));
    e->accept();

    const QAction *chosen = menu.exec(e->globalPos());
    if (!chosen || !m_scene) return;
    if (chosen == convert) {
        m_scene->convertRawToNodes(nodeId);
    } else if (chosen == convertAll) {
        // The window gets first refusal so it can confirm a sweep this large;
        // with nothing listening the entry still has to do something.
        if (isSignalConnected(QMetaMethod::fromSignal(&NodeView::convertAllRequested)))
            emit convertAllRequested();
        else
            m_scene->convertAllRaw();
    } else if (chosen == add) {
        emit contextAddRequested(scenePos);
    }
}

void NodeView::dragEnterEvent(QDragEnterEvent *e)
{
    if (acceptableDrag(e->mimeData())) {
        e->acceptProposedAction();
        return;
    }
    QGraphicsView::dragEnterEvent(e);
}

void NodeView::dragMoveEvent(QDragMoveEvent *e)
{
    if (acceptableDrag(e->mimeData())) {
        e->acceptProposedAction();
        return;
    }
    QGraphicsView::dragMoveEvent(e);
}

void NodeView::dropEvent(QDropEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (!m_scene || !acceptableDrag(mime)) {
        QGraphicsView::dropEvent(e);
        return;
    }

    // The cursor is over the node's top-left corner during the drag, so the
    // node lands where the pointer was rather than centred under it.
    const QPointF scenePos = mapToScene(e->position().toPoint());

    if (mime->hasFormat(variableDragMimeType())) {
        const QString id = QString::fromUtf8(mime->data(variableDragMimeType()));
        if (id.isEmpty()) {
            e->ignore();
            return;
        }
        // Read here because the drop is the last moment these are still the
        // keys the user was holding while they dragged.
        const Qt::KeyboardModifiers mods = e->modifiers();
        e->acceptProposedAction();
        if (mods.testFlag(Qt::ControlModifier)) {
            m_scene->addNodeAt(QStringLiteral("var.set.%1").arg(id), scenePos);
        } else if (mods.testFlag(Qt::AltModifier)) {
            m_scene->addNodeAt(QStringLiteral("var.get.%1").arg(id), scenePos);
        } else {
            const QPoint globalPos = viewport()->mapToGlobal(e->position().toPoint());
            QTimer::singleShot(0, this, [this, id, scenePos, globalPos] {
                showVariableDropMenu(id, scenePos, globalPos);
            });
        }
        return;
    }

    const QString key = QString::fromUtf8(mime->data(nodeDragMimeType()));
    if (key.isEmpty()) {
        e->ignore();
        return;
    }
    m_scene->addNodeAt(key, scenePos);
    e->acceptProposedAction();
}

void NodeView::showVariableDropMenu(const QString &variableId, const QPointF &scenePos,
                                    const QPoint &globalPos)
{
    if (!m_scene) return;
    // The graph can have moved on between the drop and this menu, and offering
    // "Get " with nothing after it would be worse than offering nothing.
    const QString name = m_scene->variableName(variableId);
    if (name.isEmpty()) return;

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction *getter = menu.addAction(QStringLiteral("Get %1").arg(name));
    getter->setToolTip(QStringLiteral("Hold Alt while dropping to skip this menu."));
    QAction *setter = menu.addAction(QStringLiteral("Set %1").arg(name));
    setter->setToolTip(QStringLiteral("Hold Ctrl while dropping to skip this menu."));

    const QAction *chosen = menu.exec(globalPos);
    if (chosen == getter)
        m_scene->addNodeAt(QStringLiteral("var.get.%1").arg(variableId), scenePos);
    else if (chosen == setter)
        m_scene->addNodeAt(QStringLiteral("var.set.%1").arg(variableId), scenePos);
}
