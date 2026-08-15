#include "minimapwidget.h"

#include "catalog.h"
#include "nodeitem.h"
#include "nodescene.h"
#include "nodeview.h"
#include "noteitem.h"
#include "theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>

namespace {

constexpr int kMargin = 4;
// The scene keeps moving while a node is dragged; repainting the map on every
// one of those would cost more than the map is worth.
constexpr int kRepaintDelay = 120;

} // namespace

MiniMapWidget::MiniMapWidget(NodeScene *scene, NodeView *view, QWidget *parent)
    : QWidget(parent), m_scene(scene), m_view(view)
{
    setMinimumHeight(90);
    setCursor(Qt::PointingHandCursor);

    auto *coalesce = new QTimer(this);
    coalesce->setSingleShot(true);
    coalesce->setInterval(kRepaintDelay);
    connect(coalesce, &QTimer::timeout, this, [this] { update(); });

    if (m_scene) {
        connect(m_scene, &QGraphicsScene::changed, this, [coalesce] {
            if (!coalesce->isActive()) coalesce->start();
        });
    }
    if (m_view) {
        connect(m_view, &NodeView::zoomChanged, this, [this](double) { update(); });
        connect(m_view->horizontalScrollBar(), &QScrollBar::valueChanged,
                this, [this] { update(); });
        connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged,
                this, [this] { update(); });
    }
}

// Scene units to widget pixels. Painting and clicking both go through this, so
// the rectangle you see is the rectangle you drag.
QTransform MiniMapWidget::mapTransform() const
{
    QRectF bounds = m_scene ? m_scene->graphBounds() : QRectF();
    const QRectF visible = m_view
        ? m_view->mapToScene(m_view->viewport()->rect()).boundingRect()
        : QRectF();

    if (bounds.isNull()) bounds = visible;
    else if (!visible.isNull()) bounds = bounds.united(visible);
    if (bounds.isEmpty()) return QTransform();
    bounds.adjust(-40, -40, 40, 40);

    const QRectF area = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    if (area.width() <= 0 || area.height() <= 0) return QTransform();
    const double scale = qMin(area.width() / bounds.width(),
                              area.height() / bounds.height());

    QTransform t;
    t.translate(area.center().x(), area.center().y());
    t.scale(scale, scale);
    t.translate(-bounds.center().x(), -bounds.center().y());
    return t;
}

void MiniMapWidget::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), theme::panelBg());

    if (!m_scene) return;
    const QTransform t = mapTransform();
    if (t.isIdentity() && m_scene->graphBounds().isNull()) {
        p.setPen(theme::textDim());
        p.setFont(theme::uiFont(8));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Nothing on the canvas yet"));
        return;
    }

    const QList<QGraphicsItem *> all = m_scene->items();
    p.setPen(Qt::NoPen);
    for (QGraphicsItem *it : all) {
        if (auto *note = dynamic_cast<NoteItem *>(it)) {
            p.setBrush(accents::comment().lighter(115));
            p.drawRect(t.mapRect(note->sceneBoundingRect()));
        } else if (auto *node = dynamic_cast<NodeItem *>(it)) {
            const QColor accent = node->def().accent;
            p.setBrush(accent.isValid() ? accent.lighter(125) : theme::headerBg());
            const QRectF r = t.mapRect(node->sceneBoundingRect());
            // Never let a node vanish entirely: at this scale a hairline is
            // still the difference between "empty graph" and "busy graph".
            p.drawRect(QRectF(r.topLeft(), QSizeF(qMax(r.width(), 2.0),
                                                  qMax(r.height(), 1.5))));
        }
    }

    if (m_view) {
        const QRectF viewport =
            t.mapRect(m_view->mapToScene(m_view->viewport()->rect()).boundingRect());
        QColor fill = theme::accent();
        fill.setAlpha(38);
        p.setBrush(fill);
        p.setPen(QPen(theme::accent(), 1.0));
        p.drawRect(viewport.adjusted(0.5, 0.5, -0.5, -0.5));
    }

    p.setBrush(Qt::NoBrush);
    p.setPen(theme::border());
    p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}

void MiniMapWidget::mousePressEvent(QMouseEvent *e)
{
    centerViewOn(e->position().toPoint());
    e->accept();
}

void MiniMapWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!e->buttons().testFlag(Qt::LeftButton)) return;
    centerViewOn(e->position().toPoint());
    e->accept();
}

void MiniMapWidget::centerViewOn(const QPoint &widgetPos)
{
    if (!m_view) return;
    bool invertible = false;
    const QTransform inverse = mapTransform().inverted(&invertible);
    if (!invertible) return;
    m_view->centerOn(inverse.map(QPointF(widgetPos)));
    update();
}
