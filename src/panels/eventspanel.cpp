#include "eventspanel.h"

#include "document.h"
#include "events.h"
#include "theme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kSigRole = Qt::UserRole + 1;  // dim right-hand text on the row
constexpr int kFullRole = Qt::UserRole + 2; // the line the footer shows

// The row that declares a method instead of overriding one. Not a catalogue
// key, so it can never collide with one.
const QLatin1String kCustomKey("event.custom");

QString customFooter()
{
    return QObject::tr("void <name>() on this script. Nothing calls it until you "
                       "wire a call to it.");
}

// The class whose events are listed. A modded script overrides methods on the
// class it mods; a new one overrides methods on the class it extends.
QString classOfGraph(const Graph *g)
{
    if (!g) return {};
    return g->modded ? g->className : g->baseClass;
}

// Event refs already on the graph. Two overrides of one method do not compile,
// so this is what stops a second one being placed.
QSet<QString> placedEvents(const Graph *g)
{
    QSet<QString> out;
    if (!g) return out;
    for (const GraphNode &n : g->nodes)
        if (n.kind == NodeKind::Event) out.insert(n.ref);
    return out;
}

// The method this row becomes: the sentence that answers "does it have to be
// that one". EventInfo carries the parameter types only, and the names are half
// the answer, so they come back out of the catalogue.
QString overrideLine(const Catalog &cat, const EventInfo &e)
{
    const MethodSig sig = cat.method(e.key);
    // A key the catalogue cannot resolve has no return type to claim, so the
    // line degrades to the name and the types rather than inventing a `void`.
    if (!sig.valid) return e.name + e.signature;

    QStringList args;
    for (int i = 0; i < sig.params.size(); ++i) {
        const MethodSig::Param &p = sig.params.at(i);
        const QString dir = p.dir == 1 ? QStringLiteral("out ")
                                       : p.dir == 2 ? QStringLiteral("inout ")
                                                    : QString();
        // An index reads better than a blank when the vanilla header declared a
        // parameter with no name, and it matches the pin the node grows.
        const QString name = p.name.isEmpty() ? QStringLiteral("arg%1").arg(i) : p.name;
        args << dir + p.type + QLatin1Char(' ') + name;
    }
    const QString ret = sig.ret.isEmpty() ? QStringLiteral("void") : sig.ret;
    return QStringLiteral("override %1 %2(%3)")
        .arg(ret, e.name, args.join(QStringLiteral(", ")));
}

QString rowTooltip(const Catalog &cat, const EventInfo &e, bool placed)
{
    QStringList lines;
    lines << overrideLine(cat, e);
    if (!e.summary.isEmpty()) lines << e.summary;
    if (placed) lines << QObject::tr("Already on this graph.");
    return lines.join(QStringLiteral("\n"));
}

// Name on the left, signature dim on the right. Telling EEItemAttached from
// EEItemDetached is the name's job, but telling which one takes a slot name is
// the signature's, and a second column would cost more width than a dock has.
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QString sig = index.data(kSigRole).toString();
        if (sig.isEmpty()) {
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
        // A row the graph already has is dimmed by its own foreground, which
        // the base paint would have applied and this one has to honour.
        QColor pen = opt.state & QStyle::State_Selected
                         ? opt.palette.color(QPalette::HighlightedText)
                         : opt.palette.color(QPalette::Text);
        const QVariant fg = index.data(Qt::ForegroundRole);
        if (fg.isValid() && !(opt.state & QStyle::State_Selected))
            pen = fg.value<QBrush>().color();
        painter->setPen(pen);
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(title, Qt::ElideRight, qMax(0, text.width())));
        painter->restore();
    }
};

// How much survived the filter, split so the caller can decide whether opening
// the noise groups is the helpful thing to do.
struct Filled {
    int matched = 0;
    int noise = 0;
};

QTreeWidgetItem *addGroupItem(QTreeWidget *tree, const QString &title)
{
    auto *group = new QTreeWidgetItem(tree);
    group->setText(0, title);
    group->setFlags(Qt::ItemIsEnabled);
    group->setFirstColumnSpanned(true);
    group->setFont(0, theme::uiFont(8, true));
    group->setForeground(0, theme::textDim());
    return group;
}

void addEmptyRow(QTreeWidget *tree, const QString &text)
{
    auto *empty = new QTreeWidgetItem(tree);
    empty->setText(0, text);
    empty->setFlags(Qt::NoItemFlags);
    empty->setForeground(0, theme::textDim());
    empty->setFirstColumnSpanned(true);
}

// Fills the tree with the events that match `query`, grouped in the order
// eventsForClass already put them in. A group of nothing but deprecated or
// debug entries starts collapsed, unless nothing outside them matched: a search
// that finds only debug hooks should still show them.
Filled fillEvents(QTreeWidget *tree, const Catalog &cat,
                  const QVector<EventInfo> &events, const QString &query,
                  const QSet<QString> &placed)
{
    Filled out;
    const QString q = query.trimmed();
    QHash<QString, QTreeWidgetItem *> groups;
    QHash<QString, bool> allNoise;

    for (const EventInfo &e : events) {
        if (!q.isEmpty() && !e.name.contains(q, Qt::CaseInsensitive)
            && !e.summary.contains(q, Qt::CaseInsensitive))
            continue;

        const bool noise = e.deprecated || e.debugOnly;
        if (noise) ++out.noise;
        else ++out.matched;

        // eventsForClass flags these rather than filing them apart, and ranks
        // them below everything live. Sections of their own is what makes them
        // collapsible, and the ranking is what puts those sections at the end.
        const QString title = e.deprecated ? QObject::tr("Deprecated")
                              : e.debugOnly ? QObject::tr("Debug")
                                            : e.group;
        QTreeWidgetItem *group = groups.value(title);
        if (!group) {
            group = addGroupItem(tree, title);
            groups.insert(title, group);
        }
        allNoise[title] = noise && allNoise.value(title, true);

        const bool onGraph = placed.contains(e.key);
        auto *item = new QTreeWidgetItem(group);
        item->setText(0, e.name);
        item->setData(0, kKeyRole, e.key);
        item->setData(0, kSigRole, e.signature);
        item->setData(0, kFullRole, overrideLine(cat, e));
        item->setToolTip(0, rowTooltip(cat, e, onGraph));
        if (onGraph) {
            // Italic and dim, not hidden: the answer to picking it again is to
            // go to the one that is already there, which needs it to be visible.
            QFont font = theme::uiFont(8);
            font.setItalic(true);
            item->setFont(0, font);
            item->setForeground(0, theme::textDim());
        }
    }

    tree->expandAll();
    const bool openNoise = !q.isEmpty() && out.matched == 0;
    if (!openNoise)
        for (auto it = allNoise.cbegin(); it != allNoise.cend(); ++it)
            if (it.value()) groups.value(it.key())->setExpanded(false);
    return out;
}

// The first event row, so the footer has a real signature to describe and Enter
// has somewhere to land the moment the list settles. Deliberately not the
// custom-event row: Enter on a freshly focused panel would open a name dialog
// nobody asked for.
QTreeWidgetItem *firstRow(QTreeWidget *tree)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        const QString key = top->data(0, kKeyRole).toString();
        if (!key.isEmpty() && key != kCustomKey) return top;
        if (top->childCount() > 0) return top->child(0);
    }
    return nullptr;
}

QTreeWidgetItem *rowForKey(QTreeWidget *tree, const QString &key)
{
    if (key.isEmpty()) return nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        if (top->data(0, kKeyRole).toString() == key) return top;
        for (int j = 0; j < top->childCount(); ++j)
            if (top->child(j)->data(0, kKeyRole).toString() == key)
                return top->child(j);
    }
    return nullptr;
}

void styleTree(QTreeWidget *tree)
{
    tree->setColumnCount(1);
    tree->setHeaderHidden(true);
    tree->setIndentation(10);
    tree->setUniformRowHeights(true);
    tree->setTextElideMode(Qt::ElideRight);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setItemDelegate(new RowDelegate(tree));
}

QLabel *makeFooter(QWidget *parent)
{
    auto *footer = new QLabel(parent);
    // A QLabel guesses at rich text, and the custom-event line really does say
    // <name>, which it would otherwise swallow as an unknown tag.
    footer->setTextFormat(Qt::PlainText);
    footer->setWordWrap(true);
    footer->setFont(theme::monoFont(8));
    footer->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    return footer;
}

QTreeWidgetItem *addCustomRow(QTreeWidget *tree, const QString &query)
{
    const QString label = QObject::tr("New custom event...");
    if (!query.trimmed().isEmpty()
        && !label.contains(query.trimmed(), Qt::CaseInsensitive))
        return nullptr;
    auto *item = new QTreeWidgetItem(tree);
    item->setText(0, label);
    item->setData(0, kKeyRole, QString(kCustomKey));
    item->setData(0, kFullRole, customFooter());
    item->setToolTip(0, QObject::tr("Declare a method on this script and place its "
                                    "node. Unreal's custom event, in Enforce terms."));
    item->setForeground(0, theme::accent());
    return item;
}

} // namespace

EventsPanel::EventsPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_class(new QLabel(this)),
      m_search(new QLineEdit(this)), m_tree(new QTreeWidget(this)),
      m_footer(makeFooter(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    // Tighter than the other docks on purpose: every pixel of chrome here is a
    // row of events that stops being on screen.
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(3);

    m_class->setTextFormat(Qt::PlainText);
    m_class->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    layout->addWidget(m_class);

    m_search->setPlaceholderText(tr("Search events: attach, damage, save"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);

    styleTree(m_tree);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_footer);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) {
        populate();
    });
    // itemActivated covers the double-click and the Enter key without firing
    // twice for either.
    connect(m_tree, &QTreeWidget::itemActivated, this, &EventsPanel::onItemActivated);
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { onCurrentChanged(); });

    if (m_doc) {
        connect(m_doc, &Document::activeScriptChanged, this, &EventsPanel::refresh);
        connect(m_doc, &Document::projectChanged, this, &EventsPanel::refresh);
        // The base class is edited on the graph, and so is the set of events
        // already placed, so both arrive through this one signal.
        connect(m_doc, &Document::graphChanged, this, &EventsPanel::refresh);
    }

    setFocusProxy(m_search);
    refresh();
}

void EventsPanel::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

QString EventsPanel::targetClass() const
{
    return classOfGraph(m_doc ? m_doc->activeGraph() : nullptr);
}

void EventsPanel::refresh()
{
    const QString cls = targetClass();
    const QSet<QString> placed = placedEvents(m_doc ? m_doc->activeGraph() : nullptr);
    const bool sameClass = m_loaded && cls == m_shownClass;
    if (sameClass && placed == m_shownPlaced) return;

    if (!sameClass)
        m_events = m_doc ? eventsForClass(m_doc->catalog(), cls) : QVector<EventInfo>();
    m_shownClass = cls;
    m_shownPlaced = placed;
    m_loaded = true;
    populate();
}

void EventsPanel::populate()
{
    const QString keep = m_tree->currentItem()
                             ? m_tree->currentItem()->data(0, kKeyRole).toString()
                             : QString();

    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    m_class->setText(m_shownClass.isEmpty()
                         ? tr("Open a script to see what it can override.")
                         : tr("%1 events on %2")
                               .arg(m_events.size()).arg(m_shownClass));

    addCustomRow(m_tree, m_search->text());
    const Filled filled = m_doc ? fillEvents(m_tree, m_doc->catalog(), m_events,
                                             m_search->text(), m_shownPlaced)
                                : Filled();
    if (filled.matched == 0 && filled.noise == 0 && !m_shownClass.isEmpty())
        addEmptyRow(m_tree, tr("No event on %1 matches that.").arg(m_shownClass));

    m_tree->setUpdatesEnabled(true);

    QTreeWidgetItem *restore = rowForKey(m_tree, keep);
    if (!restore) restore = firstRow(m_tree);
    if (restore) m_tree->setCurrentItem(restore);
    onCurrentChanged();
}

void EventsPanel::onCurrentChanged()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    const QString line = item ? item->data(0, kFullRole).toString() : QString();
    m_footer->setText(line.isEmpty()
                          ? tr("Pick an event to see the method it becomes.")
                          : line);
}

void EventsPanel::onItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item) return;
    const QString key = item->data(0, kKeyRole).toString();
    if (key.isEmpty()) return;
    if (key == kCustomKey) emit customEventRequested();
    else emit eventRequested(key);
}

// ----------------------------------------------------------------- popup

EventPopup::EventPopup(Document *doc, QWidget *parent)
    : QWidget(parent, Qt::Popup), m_doc(doc), m_search(new QLineEdit(this)),
      m_tree(new QTreeWidget(this)), m_footer(makeFooter(this))
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName(QStringLiteral("eventPopup"));
    setStyleSheet(QStringLiteral("QWidget#eventPopup { background: %1;"
                                 " border: 1px solid %2; }")
                      .arg(theme::panelBg().name(), theme::accent().name()));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    m_search->setPlaceholderText(tr("Add event: type to search"));
    layout->addWidget(m_search);
    styleTree(m_tree);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_footer);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) {
        populate();
    });
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { pick(item); });
    connect(m_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) { pick(item); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { updateFooter(); });

    // The caret stays in the search box; the arrow keys and Return are handed
    // to the list from there so a pick never needs a Tab.
    m_search->installEventFilter(this);
    resize(420, 380);
    populate();
}

void EventPopup::populate()
{
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const QString cls = classOfGraph(g);
    const QVector<EventInfo> events =
        m_doc ? eventsForClass(m_doc->catalog(), cls) : QVector<EventInfo>();

    addCustomRow(m_tree, m_search->text());
    const Filled filled = m_doc ? fillEvents(m_tree, m_doc->catalog(), events,
                                             m_search->text(), placedEvents(g))
                                : Filled();
    if (filled.matched == 0 && filled.noise == 0)
        addEmptyRow(m_tree, cls.isEmpty()
                                ? tr("Open a script first.")
                                : tr("No event on %1 matches that.").arg(cls));

    m_tree->setUpdatesEnabled(true);
    if (QTreeWidgetItem *first = firstRow(m_tree)) m_tree->setCurrentItem(first);
    updateFooter();
}

void EventPopup::updateFooter()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    const QString line = item ? item->data(0, kFullRole).toString() : QString();
    m_footer->setText(line.isEmpty() ? tr("Pick an event to see the method it becomes.")
                                     : line);
}

void EventPopup::popupAt(const QPoint &globalPos)
{
    QPoint at = globalPos;
    if (const QScreen *screen = QGuiApplication::screenAt(globalPos)) {
        const QRect avail = screen->availableGeometry();
        at.setX(qBound(avail.left(), at.x(), avail.right() - width()));
        at.setY(qBound(avail.top(), at.y(), avail.bottom() - height()));
    }
    move(at);
    show();
    m_search->setFocus(Qt::PopupFocusReason);
}

bool EventPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_search || event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);

    auto *key = static_cast<QKeyEvent *>(event);
    switch (key->key()) {
    case Qt::Key_Down:
    case Qt::Key_Up:
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
        QCoreApplication::sendEvent(m_tree, key);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        pick(m_tree->currentItem());
        return true;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void EventPopup::pick(QTreeWidgetItem *item)
{
    if (m_picked || !item) return;
    const QString key = item->data(0, kKeyRole).toString();
    if (key.isEmpty()) return;
    m_picked = true;
    // Closing first keeps the popup's mouse grab off the canvas while the node
    // is being placed and selected.
    close();
    if (key == kCustomKey) emit customEventPicked();
    else emit eventPicked(key);
}
