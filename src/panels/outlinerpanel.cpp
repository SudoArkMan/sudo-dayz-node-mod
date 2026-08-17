#include "outlinerpanel.h"

#include "document.h"
#include "enforce/lexer.h"
#include "theme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

constexpr int kIdRole = Qt::UserRole;
constexpr int kSubRole = Qt::UserRole + 1;

// Title left, owning class dim right. The outliner is scanned rather than read,
// so the class has to be visible without being loud enough to compete.
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QString sub = index.data(kSubRole).toString();
        if (sub.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString title = opt.text;
        opt.text.clear();

        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        QRect text = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        if (text.width() <= 0) return;

        const QFontMetrics fm(opt.font);
        const int subWidth = qMax(0, qMin(fm.horizontalAdvance(sub), text.width() / 2));

        painter->save();
        painter->setFont(opt.font);
        if (subWidth > 0) {
            const QRect box(text.right() - subWidth, text.top(), subWidth, text.height());
            painter->setPen(theme::textDim());
            painter->drawText(box, Qt::AlignRight | Qt::AlignVCenter,
                              fm.elidedText(sub, Qt::ElideLeft, box.width()));
            text.setRight(box.left() - 6);
        }
        painter->setPen(opt.state & QStyle::State_Selected
                            ? opt.palette.color(QPalette::HighlightedText)
                            : opt.palette.color(QPalette::Text));
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(title, Qt::ElideRight, qMax(0, text.width())));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        // A row as wide as its own text makes the view wider than its viewport,
        // and then the paint above puts the dim class column past the right
        // edge, where it is cut mid-character instead of elided. Only the height
        // of this hint is wanted; the width comes from the viewport.
        size.setWidth(1);
        return size;
    }
};

// First line only: a sticky note can hold a paragraph, a list row cannot.
QString firstLine(const QString &text)
{
    const int cut = text.indexOf('\n');
    return cut < 0 ? text : text.left(cut);
}

} // namespace

OutlinerPanel::OutlinerPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_filter(new QLineEdit(this)),
      m_list(new QListWidget(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour
    // instead of the panel colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    m_filter->setPlaceholderText(tr("Filter nodes"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);

    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setTextElideMode(Qt::ElideRight);
    // Rows elide to the width they are given, so there is nothing off to the
    // side to scroll to, and the bar was costing a row of a short list.
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setItemDelegate(new RowDelegate(m_list));
    layout->addWidget(m_list, 1);

    connect(m_filter, &QLineEdit::textChanged, this, &OutlinerPanel::refresh);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                if (!current) return;
                const QString id = current->data(kIdRole).toString();
                if (!id.isEmpty()) emit nodeActivated(id);
            });

    if (m_doc) {
        connect(m_doc, &Document::graphChanged, this, &OutlinerPanel::refresh);
        connect(m_doc, &Document::activeScriptChanged, this, &OutlinerPanel::refresh);
        connect(m_doc, &Document::projectChanged, this, &OutlinerPanel::refresh);
        // Follow the canvas without answering back: the blocker keeps the
        // highlight from bouncing straight out again as an activation.
        connect(m_doc, &Document::selectionChanged, this, [this] {
            const QSignalBlocker blocker(m_list);
            const QStringList selection = m_doc->selection();
            if (selection.size() != 1) {
                m_list->setCurrentItem(nullptr);
                return;
            }
            for (int i = 0; i < m_list->count(); ++i) {
                QListWidgetItem *item = m_list->item(i);
                if (item->data(kIdRole).toString() == selection.first()) {
                    m_list->setCurrentItem(item);
                    return;
                }
            }
            m_list->setCurrentItem(nullptr);
        });
    }

    refresh();
}

void OutlinerPanel::refresh()
{
    // Rebuilding moves the current row through every entry on the way past;
    // none of that is a user selection.
    const QSignalBlocker blocker(m_list);

    const QString filter = m_filter->text().trimmed();
    QListWidgetItem *current = m_list->currentItem();
    const QString keep = current ? current->data(kIdRole).toString() : QString();
    m_list->clear();

    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    if (graph) {
        for (const GraphNode &node : graph->nodes) {
            const NodeDef def = m_doc->defForNode(node);
            // A ref the catalogue no longer knows still has to be findable, so
            // the raw ref stands in for a title rather than leaving a blank row.
            QString title = def.valid && !def.title.isEmpty() ? def.title : node.ref;
            QString sub = def.valid ? def.subtitle : tr("unknown ref");
            if (node.kind == NodeKind::Comment) {
                const QString text = firstLine(node.opts.value(QStringLiteral("text")));
                if (!text.isEmpty()) title = text;
                sub = tr("note");
            } else if (node.ref == bi::Raw || node.ref == QLatin1String("bi.rawExpr")) {
                // A list of rows all reading "Raw Enforce" is no list at all.
                // The canvas can show the code in the node body; here the
                // summary is the only thing that tells two rows apart.
                const QString code = node.opts.value(QStringLiteral("code"));
                const QString summary = enforceSummary(code, 60);
                if (!summary.isEmpty()) {
                    title = summary;
                    sub = node.ref == bi::Raw ? tr("raw") : tr("raw value");
                }
            }
            if (!filter.isEmpty()
                && !title.contains(filter, Qt::CaseInsensitive)
                && !sub.contains(filter, Qt::CaseInsensitive))
                continue;

            auto *item = new QListWidgetItem(title, m_list);
            item->setData(kIdRole, node.id);
            item->setData(kSubRole, sub);
            item->setToolTip(QStringLiteral("%1 - %2").arg(title, node.ref));
            if (!keep.isEmpty() && node.id == keep) m_list->setCurrentItem(item);
        }
    }

    if (m_list->count() == 0) {
        // A class the importer read whole but could model none of has methods
        // and no nodes, and "no nodes yet" reads as an empty new script over a
        // class that is entirely present and will generate exactly as it came
        // in. Which of the two it is is worth saying, because the answer
        // decides whether there is anything to do here.
        int keptAsText = 0;
        if (graph)
            for (const GraphFunction &fn : graph->functions)
                if (!fn.rawBody.isEmpty()) keptAsText++;

        QString message;
        QString tip;
        if (!filter.isEmpty()) {
            message = tr("Nothing matches that filter.");
        } else if (keptAsText > 0) {
            // Short enough to survive the panel width, because the half that
            // gets clipped is the half nobody reads.
            message = keptAsText == 1
                          ? tr("No nodes. 1 method is kept as text.")
                          : tr("No nodes. %1 methods are kept as text.").arg(keptAsText);
            tip = tr("The importer could not model these method bodies, so it kept "
                     "them as the Enforce they came in as. Generate returns them "
                     "unchanged, and Generated Code shows them.");
        } else {
            message = tr("No nodes yet. Add one from the palette.");
        }

        auto *empty = new QListWidgetItem(message, m_list);
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(theme::textDim());
        if (!tip.isEmpty()) empty->setToolTip(tip);
    }
}
