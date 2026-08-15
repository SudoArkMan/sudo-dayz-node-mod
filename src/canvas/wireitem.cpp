#include "wireitem.h"

#include "theme.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace {

// Horizontal tangents keep wires readable when nodes sit close together; the
// clamp stops the curve from ballooning across a long jump or collapsing into
// a straight line on a short one.
constexpr double kMinOffset = 20.0;
constexpr double kMaxOffset = 120.0;
constexpr double kExecWidth = 2.2;
constexpr double kDataWidth = 1.6;

} // namespace

WireItem::WireItem(QGraphicsItem *parent) : QGraphicsPathItem(parent)
{
    setZValue(-1);
    setFlag(ItemIsSelectable, false);
    // Wires never take a click: a pin sitting on top of one must win, and the
    // marquee has to sweep straight through them.
    setAcceptedMouseButtons(Qt::NoButton);
    setPen(QPen(pinColor(PinKind::Any), kDataWidth));
}

void WireItem::setEndpoints(const QPointF &from, const QPointF &to)
{
    m_from = from;
    m_to = to;
    rebuildPath();
}

void WireItem::setPinType(const PinType &type)
{
    m_type = type;
    QPen p(pinColor(m_type.kind),
           m_type.kind == PinKind::Exec ? kExecWidth : kDataWidth);
    p.setCapStyle(Qt::RoundCap);
    p.setJoinStyle(Qt::RoundJoin);
    if (m_preview) p.setStyle(Qt::DashLine);
    setPen(p);
    update();
}

void WireItem::setPreview(bool preview)
{
    m_preview = preview;
    // Preview wires ride above the settled ones so the user can see where the
    // drag is going, but still under the nodes.
    setZValue(preview ? -0.5 : -1);
    setPinType(m_type);
}

void WireItem::rebuildPath()
{
    const double offset =
        qBound(kMinOffset, std::fabs(m_to.x() - m_from.x()) * 0.5, kMaxOffset);
    QPainterPath path(m_from);
    path.cubicTo(m_from + QPointF(offset, 0), m_to - QPointF(offset, 0), m_to);
    setPath(path);
}

void WireItem::paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w)
{
    Q_UNUSED(opt);
    Q_UNUSED(w);
    const QPainterPath &curve = path();
    if (curve.elementCount() < 2) return;

    p->setRenderHint(QPainter::Antialiasing, true);
    p->setBrush(Qt::NoBrush);
    p->setPen(pen());
    p->drawPath(curve);

    // Exec wires carry the flow direction at their midpoint; data wires do not
    // need it because the pin colours already read left to right.
    if (!m_preview && m_type.kind == PinKind::Exec) {
        const QPointF mid = curve.pointAtPercent(0.5);
        const double angle = curve.angleAtPercent(0.5);
        QPolygonF head;
        head << QPointF(3.8, 0) << QPointF(-2.4, 2.9) << QPointF(-2.4, -2.9);
        p->save();
        p->translate(mid);
        p->rotate(-angle);
        p->setPen(Qt::NoPen);
        p->setBrush(pinColor(PinKind::Exec));
        p->drawPolygon(head);
        p->restore();
    }
}
