#include "noteitem.h"

#include "catalog.h"
#include "document.h"
#include "nodescene.h"
#include "theme.h"

#include <QFontMetricsF>
#include <QGraphicsSceneMouseEvent>
#include <QInputDialog>
#include <QPainter>
#include <QPainterPath>
#include <QTextOption>

namespace {

constexpr double kTitleHeight = 16.0;
constexpr double kPad = 7.0;
constexpr double kGrip = 11.0;
constexpr double kMinWidth = 90.0;
constexpr double kMinHeight = 50.0;
constexpr double kDefaultWidth = 220.0;
constexpr double kDefaultHeight = 120.0;

// Comment text has moved around between builds of the reference app, so read
// whichever key this node actually carries rather than assuming one.
QString textOf(const GraphNode &n)
{
    for (const char *key : {"text", "code", "comment", "note"}) {
        const QString v = n.opts.value(QString::fromLatin1(key));
        if (!v.isEmpty()) return v;
    }
    return QString();
}

void commitOpts(Document *doc, const QString &nodeId, const QString &label,
                const QMap<QString, QString> &values)
{
    Graph *g = doc ? doc->activeGraph() : nullptr;
    if (!g || !g->node(nodeId)) return;
    doc->beginEdit(label);
    g = doc->activeGraph();
    if (GraphNode *n = g ? g->node(nodeId) : nullptr) {
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            n->opts.insert(it.key(), it.value());
    }
    doc->commitEdit();
}

} // namespace

NoteItem::NoteItem(Document *doc, const QString &nodeId, QGraphicsItem *parent)
    : QGraphicsObject(parent), m_doc(doc), m_nodeId(nodeId),
      m_size(kDefaultWidth, kDefaultHeight)
{
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    // Notes sit under the wires: they annotate the graph, they do not join it.
    setZValue(-2);
    refresh();
}

void NoteItem::refresh()
{
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *n = g ? g->node(m_nodeId) : nullptr;
    prepareGeometryChange();
    if (!n) {
        m_text.clear();
        update();
        return;
    }
    m_text = textOf(*n);
    m_title = n->opts.value(QStringLiteral("title"));
    const double w = n->opts.value(QStringLiteral("w")).toDouble();
    const double h = n->opts.value(QStringLiteral("h")).toDouble();
    m_size = QSizeF(w > 0 ? qMax(w, kMinWidth) : kDefaultWidth,
                    h > 0 ? qMax(h, kMinHeight) : kDefaultHeight);
    setPos(n->x, n->y);
    update();
}

QRectF NoteItem::boundingRect() const
{
    return QRectF(-2.0, -2.0, m_size.width() + 4.0, m_size.height() + 4.0);
}

void NoteItem::paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w)
{
    Q_UNUSED(opt);
    Q_UNUSED(w);
    p->setRenderHint(QPainter::Antialiasing, true);

    const QRectF body(0, 0, m_size.width(), m_size.height());
    QPainterPath path;
    path.addRoundedRect(body, theme::node::radius, theme::node::radius);
    p->fillPath(path, accents::comment());

    QPainterPath titleClip;
    titleClip.addRect(QRectF(0, 0, m_size.width(), kTitleHeight));
    p->fillPath(path.intersected(titleClip), accents::comment().lighter(118));

    const QString title = m_title.isEmpty() ? QStringLiteral("Comment") : m_title;
    p->setFont(theme::uiFont(7, true));
    p->setPen(theme::textDim());
    p->drawText(QRectF(kPad, 0, m_size.width() - kPad * 2, kTitleHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(theme::uiFont(7, true))
                    .elidedText(title, Qt::ElideRight, m_size.width() - kPad * 2));

    if (!m_text.isEmpty()) {
        QTextOption o(Qt::AlignLeft | Qt::AlignTop);
        o.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        p->setFont(theme::uiFont(8));
        p->setPen(theme::text());
        p->drawText(QRectF(kPad, kTitleHeight + 3.0, m_size.width() - kPad * 2,
                           m_size.height() - kTitleHeight - kPad),
                    m_text, o);
    }

    p->setBrush(Qt::NoBrush);
    p->setPen(QPen(isSelected() ? theme::selection() : theme::border(),
                   isSelected() ? 1.5 : 1.0));
    p->drawPath(path);

    // Corner grip: two short rules, enough to say "drag here" without becoming
    // decoration in its own right.
    p->setPen(QPen(theme::textDim(), 0.9));
    const QPointF c(m_size.width() - 3.0, m_size.height() - 3.0);
    p->drawLine(c + QPointF(-6.0, 0), c + QPointF(0, -6.0));
    p->drawLine(c + QPointF(-2.5, 0), c + QPointF(0, -2.5));
}

QVariant NoteItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemPositionHasChanged) {
        if (auto *s = qobject_cast<NodeScene *>(scene())) s->nodeMoved(m_nodeId);
    } else if (change == ItemSelectedHasChanged) {
        update();
    }
    return QGraphicsObject::itemChange(change, value);
}

void NoteItem::mousePressEvent(QGraphicsSceneMouseEvent *e)
{
    const QRectF grip(m_size.width() - kGrip, m_size.height() - kGrip, kGrip, kGrip);
    if (e->button() == Qt::LeftButton && grip.contains(e->pos())) {
        m_resizing = true;
        setFlag(ItemIsMovable, false);
        e->accept();
        return;
    }
    QGraphicsObject::mousePressEvent(e);
}

void NoteItem::mouseMoveEvent(QGraphicsSceneMouseEvent *e)
{
    if (!m_resizing) {
        QGraphicsObject::mouseMoveEvent(e);
        return;
    }
    prepareGeometryChange();
    m_size = QSizeF(qMax(kMinWidth, e->pos().x()), qMax(kMinHeight, e->pos().y()));
    update();
    e->accept();
}

void NoteItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *e)
{
    if (!m_resizing) {
        QGraphicsObject::mouseReleaseEvent(e);
        return;
    }
    m_resizing = false;
    setFlag(ItemIsMovable, true);
    e->accept();
    commitOpts(m_doc, m_nodeId, QStringLiteral("Resize comment"),
               {{QStringLiteral("w"), QString::number(qRound(m_size.width()))},
                {QStringLiteral("h"), QString::number(qRound(m_size.height()))}});
}

void NoteItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e)
{
    e->accept();
    bool ok = false;
    const QString entered = QInputDialog::getMultiLineText(
        e->widget(), QStringLiteral("Comment"), QStringLiteral("Note text"),
        m_text, &ok);
    if (ok) commitOpts(m_doc, m_nodeId, QStringLiteral("Edit comment"),
                       {{QStringLiteral("text"), entered}});
}
