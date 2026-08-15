#include "palettepanel.h"

#include "canvas/nodeview.h"
#include "document.h"
#include "theme.h"

#include <QApplication>
#include <QDrag>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kSigRole = Qt::UserRole + 1;

// Rows carry a key and a title. The owning class and the compact signature are
// read back out of the catalogue here, so the palette never keeps a second copy
// of a search hit that a rebuilt catalogue could make stale.
struct RowDetail {
    QString owner;
    QString sig;
};

RowDetail detailFor(const Document *doc, const QString &key)
{
    RowDetail detail;
    if (!doc || key.isEmpty()) return detail;

    if (key.startsWith(QLatin1String("bi."))) {
        detail.owner = doc->builtins().def(key).subtitle;
        return detail;
    }

    const Catalog &cat = doc->catalog();
    MethodSig sig = cat.method(key);
    if (!sig.valid) sig = cat.globalFn(key);
    if (sig.valid) {
        // Same shape as the catalogue's own search rows, so overloads read the
        // same whether they arrived from search or from a browse.
        QStringList args;
        for (const MethodSig::Param &p : sig.params) {
            const QString prefix = p.dir == 1 ? QStringLiteral("out ")
                                              : p.dir == 2 ? QStringLiteral("inout ")
                                                           : QString();
            args << prefix + p.type;
        }
        detail.sig = QStringLiteral("(%1)").arg(args.join(QStringLiteral(", ")));
        if (!sig.ret.isEmpty() && sig.ret != QLatin1String("void"))
            detail.sig += QStringLiteral(" : %1").arg(sig.ret);
        detail.owner = sig.owner.isEmpty() ? QStringLiteral("global") : sig.owner;
        return detail;
    }

    // A key the catalogue has no entry for still has to render: the raw ref is
    // more use than a blank row when a project outlives its index.
    const NodeDef def = cat.defFor(key);
    detail.owner = def.valid ? def.subtitle : key;
    return detail;
}

// Buckets rows by category, keeping first-seen order. Search results arrive
// interleaved by relevance, so without this the same header repeats down the
// whole list.
class Grouped {
public:
    void add(const QString &category, const QString &key, const QString &title)
    {
        if (!m_rows.contains(category)) m_order << category;
        m_rows[category].append(qMakePair(key, title));
    }
    const QStringList &order() const { return m_order; }
    QVector<QPair<QString, QString>> rows(const QString &category) const
    {
        return m_rows.value(category);
    }

private:
    QStringList m_order;
    QHash<QString, QVector<QPair<QString, QString>>> m_rows;
};

// Title on the left, signature dim on the right. Telling `Set(int)` from
// `Set(string)` matters more than a wide title, and a third column would cost
// more width than the dock has.
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QString sig = index.data(kSigRole).toString();
        if (index.column() != 0 || sig.isEmpty()) {
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
        const int sigWidth = qMax(0, qMin(fm.horizontalAdvance(sig), text.width() / 2));

        painter->save();
        painter->setFont(opt.font);
        if (sigWidth > 0) {
            const QRect box(text.right() - sigWidth, text.top(), sigWidth, text.height());
            painter->setPen(theme::textDim());
            painter->drawText(box, Qt::AlignRight | Qt::AlignVCenter,
                              fm.elidedText(sig, Qt::ElideLeft, box.width()));
            text.setRight(box.left() - 6);
        }
        painter->setPen(opt.state & QStyle::State_Selected
                            ? opt.palette.color(QPalette::HighlightedText)
                            : opt.palette.color(QPalette::Text));
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(title, Qt::ElideRight, qMax(0, text.width())));
        painter->restore();
    }
};

// Drag payload is the bare key. startDrag is overridden rather than mimeData so
// the drop side never has to unpack Qt's own item-model mime format.
class NodeTree : public QTreeWidget {
public:
    explicit NodeTree(QWidget *parent = nullptr) : QTreeWidget(parent) {}

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions);
        QTreeWidgetItem *item = currentItem();
        if (!item) item = selectedItems().value(0);
        if (!item) return;
        const QString key = item->data(0, kKeyRole).toString();
        if (key.isEmpty()) return; // a category header, not a node

        auto *mime = new QMimeData;
        mime->setData(nodeDragMimeType(), key.toUtf8());
        mime->setText(item->text(0));

        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
};

} // namespace

PalettePanel::PalettePanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_search(new QLineEdit(this)),
      m_tree(new NodeTree(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour
    // instead of the panel colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    m_search->setPlaceholderText(tr("Search nodes: GetHealth, EEInit, Branch"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);

    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(10);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setItemDelegate(new RowDelegate(m_tree));
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, &PalettePanel::onSearchChanged);
    // itemActivated covers both the double-click and the Enter key without
    // firing twice for either.
    connect(m_tree, &QTreeWidget::itemActivated, this, &PalettePanel::onItemActivated);
    if (m_doc) {
        connect(m_doc, &Document::projectChanged, this,
                [this] { populate(m_search->text()); });
    }

    setFocusProxy(m_search);
    populate(QString());
}

void PalettePanel::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void PalettePanel::setClassFilter(const QString &className)
{
    if (m_classFilter == className) return;
    m_classFilter = className;
    populate(m_search->text());
}

void PalettePanel::onSearchChanged(const QString &text)
{
    populate(text);
}

void PalettePanel::onItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item) return;
    const QString key = item->data(0, kKeyRole).toString();
    if (!key.isEmpty()) emit nodeRequested(key);
}

void PalettePanel::populate(const QString &query)
{
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    const QString q = query.trimmed();
    // With nothing typed the catalogue search returns nothing at all, so the
    // dock would open blank. The builtins are what a new graph starts from, so
    // they are what an empty query shows.
    const bool browsing = q.isEmpty() && m_classFilter.isEmpty();

    Grouped builtin;
    // A class filter means "members of this class", and a builtin is not a
    // member of anything, so it drops out of that view entirely.
    if (m_doc && m_classFilter.isEmpty()) {
        for (const NodeDef &def : m_doc->builtins().all()) {
            if (!browsing
                && !def.title.contains(q, Qt::CaseInsensitive)
                && !def.subtitle.contains(q, Qt::CaseInsensitive)
                && !def.category.contains(q, Qt::CaseInsensitive))
                continue;
            builtin.add(def.category.isEmpty() ? tr("Builtins") : def.category,
                        def.key, def.title);
        }
    }

    // Builtins::categories() is the intended display order; a category it does
    // not name still shows, in discovery order, rather than disappearing.
    QStringList order;
    if (m_doc) {
        for (const QString &category : m_doc->builtins().categories())
            if (!builtin.rows(category).isEmpty()) order << category;
    }
    for (const QString &category : builtin.order())
        if (!order.contains(category)) order << category;
    for (const QString &category : order) addGroup(category, builtin.rows(category));

    if (!browsing && m_doc) {
        SearchOptions opts;
        opts.limit = 80;
        opts.ofClass = m_classFilter;
        Grouped hits;
        for (const SearchHit &hit : m_doc->catalog().search(q, opts))
            hits.add(hit.category.isEmpty() ? tr("Results") : hit.category,
                     hit.key, hit.title);
        // Bucketed in arrival order, so the closest match's category leads.
        for (const QString &category : hits.order())
            addGroup(category, hits.rows(category));
    }

    if (m_tree->topLevelItemCount() == 0) {
        auto *empty = new QTreeWidgetItem(m_tree);
        empty->setText(0, m_classFilter.isEmpty()
                              ? tr("No match. Try a shorter term.")
                              : tr("Nothing on %1 matches that.").arg(m_classFilter));
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(0, theme::textDim());
        empty->setFirstColumnSpanned(true);
    }

    m_tree->expandAll();
    m_tree->setUpdatesEnabled(true);
}

void PalettePanel::addGroup(const QString &title,
                            const QVector<QPair<QString, QString>> &rows)
{
    if (rows.isEmpty()) return;

    auto *group = new QTreeWidgetItem(m_tree);
    group->setText(0, title);
    group->setFlags(Qt::ItemIsEnabled);
    group->setFirstColumnSpanned(true);
    group->setFont(0, theme::uiFont(8, true));
    group->setForeground(0, theme::textDim());

    for (const QPair<QString, QString> &row : rows) {
        const RowDetail detail = detailFor(m_doc, row.first);
        auto *item = new QTreeWidgetItem(group);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        item->setText(0, row.second);
        item->setData(0, kKeyRole, row.first);
        item->setData(0, kSigRole, detail.sig);
        item->setText(1, detail.owner);
        item->setForeground(1, theme::textDim());
        item->setToolTip(0, detail.sig.isEmpty()
                                ? row.second
                                : QStringLiteral("%1 %2").arg(row.second, detail.sig));
    }
}
