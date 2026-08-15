// MiniMap dock: the whole graph at a glance with the viewport rectangle
// drawn over it. Click or drag to move the view.
#pragma once

#include <QWidget>

class NodeScene;
class NodeView;

class MiniMapWidget : public QWidget {
    Q_OBJECT
public:
    MiniMapWidget(NodeScene *scene, NodeView *view, QWidget *parent = nullptr);

    QSize sizeHint() const override { return {220, 120}; }

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    NodeScene *m_scene;
    NodeView *m_view;

    QTransform mapTransform() const;
    void centerViewOn(const QPoint &widgetPos);
};
