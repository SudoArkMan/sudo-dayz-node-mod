// A sticky comment on the canvas: the pale annotation block used to explain
// how a graph works. Resizable, sits behind nodes, text wraps.
#pragma once

#include <QGraphicsObject>

class Document;

class NoteItem : public QGraphicsObject {
    Q_OBJECT
public:
    NoteItem(Document *doc, const QString &nodeId, QGraphicsItem *parent = nullptr);

    QString nodeId() const { return m_nodeId; }
    void refresh();

    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *opt,
               QWidget *w = nullptr) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override;
    // The corner grip resizes instead of moving, so these three have to be
    // seen before QGraphicsItem's own drag handling gets them.
    void mousePressEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *e) override;

private:
    Document *m_doc;
    QString m_nodeId;
    QString m_text;
    QString m_title;
    QSizeF m_size;
    bool m_resizing = false;
};
