// A connection between two pins: a horizontal-tangent bezier, coloured by the
// source pin's type. Exec wires are thicker and carry a mid-arrow.
#pragma once

#include "pins.h"

#include <QGraphicsPathItem>

class WireItem : public QGraphicsPathItem {
public:
    WireItem(QGraphicsItem *parent = nullptr);

    void setEndpoints(const QPointF &from, const QPointF &to);
    void setPinType(const PinType &type);
    void setEdgeId(const QString &id) { m_edgeId = id; }
    QString edgeId() const { return m_edgeId; }
    // Dragging wires are dashed and ignore hit-testing.
    void setPreview(bool preview);

    void paint(QPainter *p, const QStyleOptionGraphicsItem *opt,
               QWidget *w = nullptr) override;

private:
    QString m_edgeId;
    PinType m_type;
    QPointF m_from;
    QPointF m_to;
    bool m_preview = false;

    void rebuildPath();
};
