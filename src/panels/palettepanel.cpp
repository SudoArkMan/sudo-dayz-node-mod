#include "palettepanel.h"

#include "builtins.h"
#include "canvas/nodeview.h"
#include "document.h"
#include "theme.h"

#include <QApplication>
#include <QDrag>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kSigRole = Qt::UserRole + 1;
constexpr int kDocRole = Qt::UserRole + 2;  // the line the footer shows
constexpr int kWordRole = Qt::UserRole + 3; // the query an empty-state row runs

// The rows the empty state offers. Not node keys, so they cannot collide with
// one, and not index keys either: these only exist when nothing was found.
const QLatin1String kRawKey("empty.raw");
const QLatin1String kFirstWordKey("empty.firstWord");

// The catalogue files its search rows under words that describe the shape of a
// declaration rather than what it is for. "Pure" is a compiler property and
// means nothing to a DayZ modder reading a list of results.
QString resultHeading(const QString &category)
{
    if (category == QLatin1String("Functions")) return QObject::tr("Calls");
    if (category == QLatin1String("Pure")) return QObject::tr("Reads a value");
    if (category == QLatin1String("Globals")) return QObject::tr("Global functions");
    if (category == QLatin1String("Enums")) return QObject::tr("Enum types");
    if (category == QLatin1String("Events")) return QObject::tr("Events");
    if (category == QLatin1String("Constants")) return QObject::tr("Constants");
    return category.isEmpty() ? QObject::tr("Results") : category;
}

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

// Buckets search hits by heading, keeping first-seen order. Results arrive
// interleaved by relevance, so without this the same header repeats down the
// whole list.
class Grouped {
public:
    void add(const QString &heading, const IndexRow &row)
    {
        if (!m_rows.contains(heading)) m_order << heading;
        m_rows[heading].append(row);
    }
    const QStringList &order() const { return m_order; }
    QVector<IndexRow> rows(const QString &heading) const { return m_rows.value(heading); }

private:
    QStringList m_order;
    QHash<QString, QVector<IndexRow>> m_rows;
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
        // The name comes first. A flat half-width share for the signature cut
        // `SetHealthMaxOverride` down to `SetHealt...` to make room for
        // `...ng, float)`, which tells you nothing: the signature only earns
        // space the title does not want, and never more than half.
        const int titleWidth = fm.horizontalAdvance(title);
        const int spare = qMax(0, text.width() - titleWidth - 6);
        const int sigWidth = qMin(qMin(fm.horizontalAdvance(sig), spare), text.width() / 2);

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
        // A category header, or one of the rows that acts rather than places.
        if (key.isEmpty() || key.startsWith(QLatin1String("empty."))
            || key == nodeindex::BrowseEventsKey)
            return;

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
      m_tree(new NodeTree(this)), m_footer(new QLabel(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour
    // instead of the panel colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    // Tighter than the default on purpose: every pixel of chrome here is a row
    // of nodes that stops being on screen.
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(3);

    m_search->setPlaceholderText(tr("Search nodes, or two words: set health"));
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

    // Reading a node before placing it is the point: the palette holds 29k
    // methods and a name alone does not say which one takes a slot string.
    //
    // One elided line rather than a wrapped paragraph. Three wrapped lines took
    // nearly half the rows this dock has, and the list is what the dock is for;
    // the whole text is a hover away on the row and on this label.
    m_footer->setTextFormat(Qt::PlainText);
    m_footer->setWordWrap(false);
    m_footer->setFont(theme::uiFont(8));
    m_footer->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    // A long line would otherwise widen the dock rather than be cut.
    m_footer->setMinimumWidth(1);
    layout->addWidget(m_footer);

    connect(m_search, &QLineEdit::textChanged, this, &PalettePanel::onSearchChanged);
    // itemActivated covers both the double-click and the Enter key without
    // firing twice for either.
    connect(m_tree, &QTreeWidget::itemActivated, this, &PalettePanel::onItemActivated);
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { onCurrentChanged(); });
    if (m_doc) {
        connect(m_doc, &Document::projectChanged, this,
                [this] { populate(m_search->text()); });
        // Which class the results are legal for changes with the script, so
        // the list has to be rebuilt when the editor points somewhere else.
        connect(m_doc, &Document::activeScriptChanged, this,
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

void PalettePanel::search(const QString &query)
{
    // setText emits textChanged, which repopulates, so nothing else is needed.
    m_search->setText(query);
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

void PalettePanel::onCurrentChanged()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    // The empty state's first line is not selectable, so nothing is current
    // when it is showing and its explanation would never be read.
    if (!item) item = m_tree->topLevelItem(0);
    const QString doc = item ? item->data(0, kDocRole).toString() : QString();
    m_footerText = doc.isEmpty()
                       ? tr("Pick a node to see what it does before you place it.")
                       : doc;
    elideFooter();
}

const QVector<IndexGroup> &PalettePanel::indexFor(const QString &selfClass)
{
    if (m_indexLoaded && m_indexClass == selfClass) return m_index;
    m_index = m_doc ? nodeIndex(m_doc->catalog(), m_doc->builtins(), selfClass)
                    : QVector<IndexGroup>();
    m_indexClass = selfClass;
    m_indexLoaded = true;
    return m_index;
}

void PalettePanel::elideFooter()
{
    const QFontMetrics fm(m_footer->font());
    m_footer->setText(fm.elidedText(m_footerText, Qt::ElideRight,
                                    qMax(40, m_footer->width())));
    // The full line is on the label as well as on the row, so a summary the
    // dock is too narrow to hold is still readable without placing the node.
    m_footer->setToolTip(m_footerText);
}

void PalettePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    elideFooter();
}

void PalettePanel::onItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item) return;
    const QString key = item->data(0, kKeyRole).toString();
    if (key.isEmpty()) return;
    if (key == nodeindex::BrowseEventsKey) {
        emit eventsRequested();
        return;
    }
    if (key == kFirstWordKey) {
        // The row offers one word out of a query that matched nothing as a
        // whole, so taking it means searching for that word. Carried in its own
        // role rather than read back out of the label, which is translated.
        m_search->setText(item->data(0, kWordRole).toString());
        return;
    }
    if (key == kRawKey) {
        emit nodeRequested(bi::Raw);
        return;
    }
    emit nodeRequested(key);
}

void PalettePanel::populate(const QString &query)
{
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    const QString q = query.trimmed();
    // With nothing typed the catalogue search returns nothing at all, so the
    // dock would open blank. The task index is what a graph starts from, so it
    // is what an empty query shows.
    const bool browsing = q.isEmpty() && m_classFilter.isEmpty();

    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const QString self = g ? selfClassOf(*g) : QString();

    QSet<QString> shown;
    if (m_doc && m_classFilter.isEmpty()) {
        const QVector<IndexGroup> index = indexFor(self);
        for (int i = 0; i < index.size(); ++i) {
            IndexGroup group = index.at(i);
            if (!browsing) {
                QVector<IndexRow> keep;
                for (const IndexRow &row : group.rows)
                    if (rowMatches(q, row, group.title)) keep.append(row);
                group.rows = keep;
            }
            if (group.rows.isEmpty()) continue;
            for (const IndexRow &row : group.rows) shown.insert(row.key);
            // The first two groups carry the moment a graph starts from and the
            // deferral that could not be found. Those are open; the rest are one
            // click, which in a dock this short puts more of the index on screen
            // than four expanded groups would.
            addGroup(group, browsing && i >= 2);
        }
    }

    if (!browsing && m_doc) {
        SearchOptions opts;
        opts.limit = 80;
        opts.ofClass = m_classFilter;
        // A protected method is a real node, but only for a graph that
        // inherits the class declaring it. Offering it to everybody is how
        // `m_Timer.SetRunning(false)` got built: the palette said yes, and the
        // compiler said no once the mod was packed. Read fresh on every
        // populate rather than cached, so editing the class header in the
        // inspector takes effect on the next keystroke here.
        opts.selfClass = self;
        opts.respectAccess = true;
        Grouped hits;
        for (const SearchHit &hit : m_doc->catalog().search(q, opts)) {
            // Already above, under the group that says what it is for.
            if (shown.contains(hit.key)) continue;
            // A hook from a class this graph does not descend from generates a
            // method the engine never calls.
            if (!eventFitsClass(m_doc->catalog(), hit.category, hit.subtitle, self))
                continue;
            IndexRow row;
            row.key = hit.key;
            row.title = hit.title;
            row.detail = hit.subtitle;
            row.doc = nodeSummary(m_doc->catalog(), m_doc->builtins(), hit.key);
            hits.add(resultHeading(hit.category), row);
        }
        // Bucketed in arrival order, so the closest match's heading leads.
        for (const QString &heading : hits.order()) {
            IndexGroup group;
            group.title = heading;
            group.rows = hits.rows(heading);
            addGroup(group, false);
        }
    }

    if (m_tree->topLevelItemCount() == 0) addEmptyState(q);

    m_tree->setUpdatesEnabled(true);
    if (QTreeWidgetItem *first = m_tree->topLevelItem(0)) {
        // The first row rather than the first heading, so the footer has
        // something to describe the moment the list settles.
        m_tree->setCurrentItem(first->childCount() > 0 ? first->child(0) : first);
        // setCurrentItem scrolls to what it selected, and the view scrolls again
        // when it is first laid out, which in a dock five rows tall put the
        // heading above it off the top: the list opened on rows with nothing
        // saying what they were for. Queued, because the second scroll happens
        // after this returns and the first populate runs before the panel has
        // ever been shown.
        QTimer::singleShot(0, m_tree, [tree = m_tree]() { tree->scrollToTop(); });
    }
    onCurrentChanged();
}

void PalettePanel::addGroup(const IndexGroup &group, bool collapsed)
{
    if (group.rows.isEmpty()) return;

    auto *heading = new QTreeWidgetItem(m_tree);
    heading->setText(0, group.title);
    heading->setFlags(Qt::ItemIsEnabled);
    heading->setFirstColumnSpanned(true);
    heading->setFont(0, theme::uiFont(8, true));
    heading->setForeground(0, theme::textDim());
    if (!group.doc.isEmpty()) {
        heading->setToolTip(0, group.doc);
        // The heading is not a node, so its footer line is the group's reason
        // for existing. That is where the counts behind the ordering live.
        heading->setData(0, kDocRole, group.doc);
    }

    for (const IndexRow &row : group.rows) {
        const RowDetail detail = detailFor(m_doc, row.key);
        auto *item = new QTreeWidgetItem(heading);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        item->setText(0, row.title);
        item->setData(0, kKeyRole, row.key);
        item->setData(0, kSigRole, detail.sig);
        item->setData(0, kDocRole, row.doc);
        item->setText(1, row.detail.isEmpty() ? detail.owner : row.detail);
        item->setForeground(1, theme::textDim());
        // A row that acts on a panel instead of placing a node is marked, so
        // the one entry that is not a node does not read as one.
        if (row.key == nodeindex::BrowseEventsKey) item->setForeground(0, theme::accent());

        QStringList tip;
        tip << (detail.sig.isEmpty() ? row.title
                                     : QStringLiteral("%1 %2").arg(row.title, detail.sig));
        if (!row.doc.isEmpty()) tip << row.doc;
        if (m_doc)
            for (const QString &caution :
                 nodeCautions(m_doc->catalog(), m_doc->builtins(), row.key))
                tip << QStringLiteral("Caution: %1").arg(caution);
        item->setToolTip(0, tip.join(QStringLiteral("\n")));
    }

    heading->setExpanded(!collapsed);
}

void PalettePanel::addEmptyState(const QString &query)
{
    const auto line = [this](const QString &text, const QString &key,
                             const QString &doc) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, text);
        item->setFirstColumnSpanned(true);
        item->setData(0, kDocRole, doc);
        if (key.isEmpty()) {
            item->setFlags(Qt::NoItemFlags);
            item->setForeground(0, theme::textDim());
        } else {
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setData(0, kKeyRole, key);
            item->setForeground(0, theme::accent());
        }
        return item;
    };

    // Nothing found may mean nothing exists, or it may mean everything that
    // matched was protected and out of this class's reach. Those are different
    // problems and a user who is told the first one when it was the second goes
    // looking for a spelling mistake that is not there.
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const QString self = g ? selfClassOf(*g) : QString();
    if (m_doc && !query.isEmpty()) {
        SearchOptions plain;
        plain.limit = 1;
        plain.ofClass = m_classFilter;
        const QVector<SearchHit> any = m_doc->catalog().search(query, plain);
        if (!any.isEmpty()) {
            line(self.isEmpty()
                     ? tr("%1 is declared protected. Give this script a base class to "
                          "reach it.").arg(any.first().title)
                     : tr("%1 is declared protected on %2, and %3 does not inherit it.")
                           .arg(any.first().title, any.first().subtitle, self),
                 QString(), QString());
            return;
        }
    }

    if (!m_classFilter.isEmpty()) {
        // With nothing typed, an empty class view is a class the catalogue has
        // never heard of rather than a query that found nothing, and telling
        // the user their search failed sends them to fix the wrong thing.
        line(query.isEmpty()
                 ? tr("The catalogue has no class called %1.").arg(m_classFilter)
                 : tr("Nothing on %1 matches that.").arg(m_classFilter),
             QString(),
             query.isEmpty()
                 ? tr("It may come from a mod the catalogue was not built against. "
                      "Nodes for it can still be written as Raw Enforce.")
                 : QString());
        return;
    }

    // The search covers names, and every declaration in DayZ is one word, so
    // the true statement is about names rather than about the query being too
    // long. Saying "try a shorter term" was the tool blaming the reader for a
    // bug it has since fixed.
    line(tr("No node is named \"%1\".").arg(query), QString(),
         tr("The search reads names, owning classes and signatures across the whole "
            "catalogue. Every term has to land somewhere on the row."));

    // A two-word query that finds nothing usually has one word that does. The
    // offer names the word, so taking it is a decision rather than a guess.
    if (m_doc) {
        const QStringList words = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.size() > 1) {
            for (const QString &word : words) {
                SearchOptions one;
                one.limit = 1;
                if (m_doc->catalog().search(word, one).isEmpty()) continue;
                QTreeWidgetItem *row =
                    line(tr("Search for \"%1\" on its own").arg(word), kFirstWordKey,
                         tr("Every word has to land somewhere on the same row. One of "
                            "these does not, so the other is where the results are."));
                row->setData(0, kWordRole, word);
                break;
            }
        }
    }

    line(self.isEmpty() ? tr("Browse the events this script can override")
                        : tr("Browse the events on %1").arg(self),
         nodeindex::BrowseEventsKey,
         tr("A hook is found by the moment it fires, not by its name. That list is "
            "ranked and grouped; this search is not."));

    line(tr("Write it as Enforce in a Raw node"), kRawKey,
         tr("Keeps the line as text and still generates. The importer can read some "
            "of it back into nodes later, and refuses rather than guessing."));
}
