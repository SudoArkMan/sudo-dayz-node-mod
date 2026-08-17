#include "mainwindow.h"

#include "analysis.h"
#include "branding.h"
#include "canvas/minimapwidget.h"
#include "canvas/nodescene.h"
#include "canvas/nodeview.h"
#include "codegen.h"
#include "document.h"
#include "enforce/import.h"
#include "modlibrary.h"
#include "modtemplate.h"
#include "nodeindex.h"
#include "panels/codeviewpanel.h"
#include "panels/eventspanel.h"
#include "panels/explorerpanel.h"
#include "panels/inspectorpanel.h"
#include "panels/modbrowser.h"
#include "panels/outlinerpanel.h"
#include "panels/palettepanel.h"
#include "panels/testpanel.h"
#include "panels/variablespanel.h"
#include "recentprojects.h"
#include "theme.h"
#include "widgets/codedialog.h"
#include "widgets/configeditor.h"
#include "widgets/filedialog.h"
#include "widgets/newmoddialog.h"
#include "widgets/newscriptdialog.h"
#include "widgets/startpage.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kDocRole = Qt::UserRole + 1; // the line the popup's footer shows

// The row that turns the dragged value into a member instead of looking for a
// node to take it. Not a node key, so it can never collide with one.
const QLatin1String kPromoteKey("promote.variable");

// The row that hands the search over to the events list. Also not a node key,
// and shared with the palette so one string names it everywhere.
const QLatin1String kAddEventKey = nodeindex::BrowseEventsKey;

// Ranked catalogue hits to test against the dragged pin, and how many survivors
// are worth listing. The catalogue holds 29k entries; testing them all would
// cost a def build each, so the menu walks the ranking and stops early.
constexpr int kFitScan = 400;
constexpr int kFitKeep = 40;

// QDockWidget::setWidget reparents the panel onto the dock, so a panel always
// knows the dock it is in and nothing has to carry both pointers around.
QDockWidget *dockOf(QWidget *panel)
{
    return panel ? qobject_cast<QDockWidget *>(panel->parentWidget()) : nullptr;
}

// Brings the dock a panel lives in to the front when it is stacked behind
// another. Docks are split by default, where raise does nothing, but the layout
// is the user's to rearrange and a tabbed inspector shows nothing at all.
void revealDock(QWidget *panel)
{
    QDockWidget *dock = dockOf(panel);
    // A dock closed from the View menu stays closed. Raising it would reopen it
    // and take the space back every time a node was selected.
    if (!dock || !dock->toggleViewAction()->isChecked()) return;
    dock->raise();
}

// Puts a tabbed dock's own tab in front, which is what raise() is supposed to
// do and here does not: raise() only asks the tab bar to move when the widget
// actually changes place in the sibling stack, and a dock that has just been
// shown is already at the top of it. Measured rather than assumed, by taking a
// picture of the window and reading which tab was in front.
//
// The tab carries the dock's address in its data, which is Qt's own key for it.
// Matched on the title as well, because that data is an implementation detail
// and a build where it is not there should still find the tab.
void raiseDockTab(QMainWindow *window, QDockWidget *dock)
{
    if (!window || !dock) return;
    const auto address = quintptr(dock);
    for (QTabBar *bar : window->findChildren<QTabBar *>()) {
        for (int i = 0; i < bar->count(); ++i) {
            const bool byData = bar->tabData(i).canConvert<quintptr>()
                                && bar->tabData(i).value<quintptr>() == address;
            if (!byData && bar->tabText(i) != dock->windowTitle()) continue;
            bar->setCurrentIndex(i);
            return;
        }
    }
}

// True when a dock is the one in front of its tab group, or is not tabbed at
// all. QDockWidget::isVisible() answers true for a dock sitting behind another
// in the same group, so it cannot be asked which tab the user is looking at:
// the bottom row was sized at the Mod Browser's share on every window that
// opened on the generated file, and the left column paid about 120 pixels for
// it on a 950 tall screen. Measured, not reasoned: the probe printed
// browsing=1 with the Generated Code tab in front on all three calls.
//
// Matched on the dock's address, which is Qt's own key for the tab, and on the
// title as well, because that data is an implementation detail.
bool dockIsInFront(QMainWindow *window, QDockWidget *dock)
{
    if (!window || !dock || !dock->isVisible()) return false;
    if (window->tabifiedDockWidgets(dock).isEmpty()) return true;
    const auto address = quintptr(dock);
    for (QTabBar *bar : window->findChildren<QTabBar *>()) {
        for (int i = 0; i < bar->count(); ++i) {
            const bool byData = bar->tabData(i).canConvert<quintptr>()
                                && bar->tabData(i).value<quintptr>() == address;
            if (!byData && bar->tabText(i) != dock->windowTitle()) continue;
            return bar->currentIndex() == i;
        }
    }
    // Tabified with no bar built yet, which is the state during the first
    // layout. The old reading stands rather than guessing the other way.
    return true;
}

// One way in for every surface that says "this node". Clicking a node on the
// canvas, activating a row in the Graph Outliner and clicking a line in the
// generated file all have to end with the same node selected, framed and open
// in the inspector. Three call sites doing it by hand is how they drift apart.
void revealNode(Document *doc, NodeView *view, QWidget *inspector,
                const QString &nodeId)
{
    if (!doc || nodeId.isEmpty()) return;
    doc->setSelection({nodeId});
    if (view) view->centerOnNode(nodeId);
    revealDock(inspector);
}

// Nodes whose content is hand-written text rather than pins. Everything else on
// the canvas is described by the catalogue, so there is nothing to type into it.
bool isCodeNode(const GraphNode &node)
{
    return node.ref == bi::Raw || node.ref == bi::Comment
           || node.ref == QLatin1String("bi.rawExpr")
           || node.kind == NodeKind::Comment;
}

// How much of an imported script the graph could not model: raw nodes and
// methods kept verbatim. Counted off the graph rather than taken from the
// importer's statement tally, because this is the number that decides whether
// the user is warned, and a statement the graph handles another way (the super
// call an event node emits itself) is not text left behind. Comments are left
// out: they regenerate as comments.
int textKeptIn(const Graph &g)
{
    int n = 0;
    for (const GraphNode &node : g.nodes)
        if (node.ref == bi::Raw || node.ref == QLatin1String("bi.rawExpr")) ++n;
    for (const GraphFunction &f : g.functions)
        if (f.hasRawBody) ++n;
    return n;
}

// The scaffolder reports the folders it made, not the token it substituted, and
// the prefix is the one path segment between them: <modRoot>/<prefix>/Scripts.
QString prefixFromScaffold(const ModTemplateResult &result)
{
    if (result.modRoot.isEmpty() || result.scriptsRoot.isEmpty())
        return QFileInfo(result.modRoot).fileName();
    const QString rel = QDir(result.modRoot).relativeFilePath(result.scriptsRoot);
    const QString first = rel.section(QLatin1Char('/'), 0, 0);
    if (first.isEmpty() || first == QLatin1String(".") || first == QLatin1String(".."))
        return QFileInfo(result.modRoot).fileName();
    return first;
}

// The prefix of a mod folder picked by hand: the child holding Scripts, which
// is the one folder DayZ's config.cpp addresses by name. Falls back to the
// folder's own name so export still lands somewhere sensible.
QString prefixOfModFolder(const QString &modRoot)
{
    const QDir root(modRoot);
    const QStringList children = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                QDir::Name);
    for (const QString &child : children)
        if (QFileInfo(root.filePath(child + QStringLiteral("/Scripts"))).isDir())
            return child;
    return root.dirName();
}

QString countOf(int n, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

// A real output pin on the open graph, for the headless picture of the drag-out
// menu. An object pin is preferred over a primitive because the class filter is
// the half of that menu worth looking at, and a primitive is preferred over
// nothing. Invalid when the graph has no output pin at all, which is a graph
// with no nodes.
PinRef dragSourceForScreenshot(Document *doc, bool exec)
{
    PinRef best;
    int bestRank = -1;
    const Graph *g = doc ? doc->activeGraph() : nullptr;
    if (!g) return best;
    for (const GraphNode &node : g->nodes) {
        const NodeDef def = doc->defForNode(node);
        for (const Pin &pin : def.pins) {
            if (pin.dir != PinDir::Out) continue;
            const bool isExec = pin.type.kind == PinKind::Exec;
            if (isExec != exec) continue;
            const int rank = pin.type.kind == PinKind::Object ? 2 : 1;
            if (rank <= bestRank) continue;
            bestRank = rank;
            best = {node.id, pin.id, PinDir::Out, true};
        }
    }
    return best;
}

// Where a class starts in a file already on disk, or -1.
int classHeaderAt(const QString &file, const QString &className)
{
    if (className.isEmpty() || file.isEmpty()) return -1;
    const QRegularExpression header(
        QStringLiteral("(?:\\A|\\n)[ \\t]*(?:modded[ \\t]+)?class[ \\t]+%1\\b")
            .arg(QRegularExpression::escape(className)));
    const QRegularExpressionMatch m = header.match(file);
    return m.hasMatch() ? m.capturedStart() : -1;
}

// The stretch of `file` holding one class. A file with two classes in it has
// two user regions, and handing the whole file to the generator would give both
// classes the first one.
QString classSection(const QString &file, const QString &className,
                     const QStringList &siblings)
{
    if (siblings.size() < 2) return file;
    const int start = classHeaderAt(file, className);
    if (start < 0) return {};
    int end = file.size();
    for (const QString &other : siblings) {
        if (other == className) continue;
        const int at = classHeaderAt(file, other);
        if (at > start && at < end) end = at;
    }
    return file.mid(start, end - start);
}

// A file's text, or an empty string when there is no file yet. The handle is
// closed before returning: Windows will not let QSaveFile rename over a file
// this process still has open, so reading the previous contents and writing the
// new ones cannot overlap.
QString readFileText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// The preamble put back in front of the class, ending in exactly one blank
// line, so writing a file twice does not add spacing each time.
// Re-reads the widget's style so a `severity` property change actually repaints.
// Colour has to come from the sheet: every QLabel rule in theme.cpp names a
// colour, and a stylesheet colour beats QWidget::setPalette.
void setSeverity(QWidget *w, const char *value)
{
    if (!w) return;
    const QVariant current = w->property("severity");
    const QVariant next = value && *value ? QVariant(QLatin1String(value)) : QVariant();
    if (current == next) return;
    w->setProperty("severity", next);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// What closing a tab costs, which is the only question a close prompt turns on.
// Read from the script rather than asked of the user, because the user cannot
// see which of the three a tab is without opening the file it came from.
enum class CloseCost {
    Browsed,  // read out of another mod; the Mod Browser has it again on demand
    OnDisk,   // imported from a .c that is still there, so the file outlives the tab
    OnlyCopy, // authored here, so this project is the only place the graph exists
};

CloseCost closeCostOf(const ScriptEntry &s)
{
    if (graphIsReadOnly(s.graph)) return CloseCost::Browsed;
    // A sourcePath naming a file that has since been deleted or moved is worth
    // no more than none at all, so it is checked rather than trusted.
    if (!s.sourcePath.isEmpty() && QFileInfo::exists(s.sourcePath))
        return CloseCost::OnDisk;
    return CloseCost::OnlyCopy;
}

// The cross on a script tab.
//
// Painted here rather than left to setTabsClosable, which draws
// PE_IndicatorTabClose: a dark grey X, and drawn at QIcon::Disabled on every tab
// that is not the current one or under the mouse. Against #1b1e23 that is close
// to invisible, and it cannot be swapped for a lighter one because Qt's close
// button paints the primitive and ignores any icon it is given.
class TabCloseButton : public QAbstractButton {
public:
    TabCloseButton(const QString &name, QWidget *parent)
        : QAbstractButton(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
        setToolTip(QStringLiteral("Close %1").arg(name));
        setFixedSize(14, 14);
    }

    QSize sizeHint() const override { return {14, 14}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const bool hot = underMouse() || isDown();
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? theme::accent().darker(150) : theme::headerBg());
            p.drawRoundedRect(QRectF(rect()), 3, 3);
        }
        QPen pen(hot ? theme::text() : theme::textDim());
        pen.setWidthF(1.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const QRectF x = QRectF(rect()).adjusted(4.5, 4.5, -4.5, -4.5);
        p.drawLine(x.topLeft(), x.bottomRight());
        p.drawLine(x.topRight(), x.bottomLeft());
    }

    // The cross is dim until the pointer is on it, so both edges have to repaint.
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
};

// Every script in the project, filtered as you type.
//
// The tab bar starts scrolling somewhere around eight tabs and a bar that has to
// be scrolled is not a way to find anything, least of all in a session where the
// Mod Browser has been filling it with other people's classes. Same shape as the
// add-node search below it and for the same reason: the caret is in the box from
// the first frame and Return picks.
//
// Deliberately not a Q_OBJECT, like AddNodePopup: it lives in this file, so a
// callback is cheaper than a signal and does not need moc to see the class.
class ScriptListPopup : public QWidget {
public:
    ScriptListPopup(const Project &project, std::function<void(const QString &)> onPick,
                    QWidget *parent)
        : QWidget(parent, Qt::Popup), m_onPick(std::move(onPick)),
          m_search(new QLineEdit(this)), m_list(new QTreeWidget(this))
    {
        setAttribute(Qt::WA_DeleteOnClose, true);
        setAttribute(Qt::WA_StyledBackground, true);
        setObjectName(QStringLiteral("scriptListPopup"));
        setStyleSheet(QStringLiteral("QWidget#scriptListPopup { background: %1;"
                                     " border: 1px solid %2; }")
                          .arg(theme::panelBg().name(), theme::accent().name()));

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(4);

        m_search->setPlaceholderText(tr("Go to script: type to search"));
        layout->addWidget(m_search);

        m_list->setColumnCount(2);
        m_list->setHeaderHidden(true);
        m_list->setRootIsDecorated(false);
        m_list->setUniformRowHeights(true);
        m_list->setTextElideMode(Qt::ElideMiddle);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        layout->addWidget(m_list, 1);

        for (const ScriptEntry &s : project.scripts) {
            Row row;
            row.id = s.id;
            row.name = s.name;
            // A browsed class has no folder of the user's to show, and where it
            // came from is what tells two mods' PlayerBase apart.
            row.detail = graphIsReadOnly(s.graph) ? graphOrigin(s.graph) : s.folder;
            row.browsed = graphIsReadOnly(s.graph);
            row.current = s.id == project.activeId;
            m_rows.append(row);
        }

        connect(m_search, &QLineEdit::textChanged, this,
                [this](const QString &text) { populate(text); });
        connect(m_list, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *item, int) { pick(item); });
        connect(m_list, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem *item, int) { pick(item); });

        m_search->installEventFilter(this);
        resize(420, 340);
        populate(QString());
    }

    void popupAt(const QPoint &globalPos)
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_search || event->type() != QEvent::KeyPress)
            return QWidget::eventFilter(watched, event);

        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            QCoreApplication::sendEvent(m_list, key);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            pick(m_list->currentItem());
            return true;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    struct Row {
        QString id;
        QString name;
        QString detail;
        bool browsed = false;
        bool current = false;
    };

    void pick(QTreeWidgetItem *item)
    {
        if (m_picked || !item) return;
        const QString id = item->data(0, kKeyRole).toString();
        if (id.isEmpty()) return;
        m_picked = true;
        close();
        if (m_onPick) m_onPick(id);
    }

    void populate(const QString &query)
    {
        m_list->setUpdatesEnabled(false);
        m_list->clear();
        const QString q = query.trimmed();
        QTreeWidgetItem *landOn = nullptr;
        for (const Row &row : m_rows) {
            if (!matchesQuery(q, {row.name, row.detail})) continue;
            auto *item = new QTreeWidgetItem(m_list);
            item->setText(0, row.name);
            item->setData(0, kKeyRole, row.id);
            item->setText(1, row.detail);
            item->setForeground(1, theme::textDim());
            item->setToolTip(0, row.detail);
            // Same dimming the tab bar gives a browsed class, so the two
            // surfaces agree about which scripts are not the user's.
            if (row.browsed) item->setForeground(0, theme::textDim());
            if (row.current) {
                item->setFont(0, theme::uiFont(8, true));
                item->setForeground(0, theme::accent());
            }
            // The script in front is where the arrow keys start, so Down goes to
            // the next tab rather than back to the top of the list.
            if (!landOn || row.current) landOn = item;
        }
        if (landOn) m_list->setCurrentItem(landOn);
        m_list->setUpdatesEnabled(true);
    }

    std::function<void(const QString &)> m_onPick;
    QLineEdit *m_search;
    QTreeWidget *m_list;
    QVector<Row> m_rows;
    bool m_picked = false;
};

// The add-node search the canvas right-click opens, and the menu a wire dropped
// on empty canvas opens too: a search box over a result list, at the cursor,
// keyboard-driven from the first keystroke. This is the primary way nodes get
// added, so it carries the graph's own variables as well as the task index and
// the catalogue.
//
// Two things it has to teach, because neither is discoverable by looking. That
// dropping a wire here narrows the list to what fits, which is a better way in
// than any name search: the footer says so on every open. And, once narrowed,
// what it narrowed to, because a short list with no explanation reads as a
// broken one.
//
// Deliberately not a Q_OBJECT: it lives in this file, so a callback is cheaper
// than a signal and does not need moc to see the class.
class AddNodePopup : public QWidget {
public:
    AddNodePopup(Document *doc, std::function<void(const QString &)> onPick,
                 QWidget *parent)
        : QWidget(parent, Qt::Popup), m_doc(doc), m_onPick(std::move(onPick)),
          m_search(new QLineEdit(this)), m_list(new QTreeWidget(this)),
          m_mode(new QLabel(this)), m_footer(new QLabel(this))
    {
        setAttribute(Qt::WA_DeleteOnClose, true);
        setAttribute(Qt::WA_StyledBackground, true);
        setObjectName(QStringLiteral("addNodePopup"));
        setStyleSheet(QStringLiteral("QWidget#addNodePopup { background: %1;"
                                     " border: 1px solid %2; }")
                          .arg(theme::panelBg().name(), theme::accent().name()));

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(4);

        m_search->setPlaceholderText(tr("Add node: type to search"));
        layout->addWidget(m_search);

        // Above the list, not below it, and never replaced by anything: in
        // connect mode this is the only thing on screen that says why the list
        // is short, and the rest of the time it is the only thing that says the
        // gesture exists. Putting it in the footer meant it was covered by the
        // selected row's own line the instant the popup opened.
        m_mode->setTextFormat(Qt::PlainText);
        m_mode->setWordWrap(true);
        m_mode->setFont(theme::uiFont(8));
        m_mode->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
        layout->addWidget(m_mode);

        m_list->setColumnCount(2);
        m_list->setHeaderHidden(true);
        m_list->setRootIsDecorated(false);
        m_list->setIndentation(10);
        m_list->setUniformRowHeights(true);
        m_list->setTextElideMode(Qt::ElideRight);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        layout->addWidget(m_list, 1);

        m_footer->setTextFormat(Qt::PlainText);
        m_footer->setWordWrap(true);
        m_footer->setFont(theme::uiFont(8));
        m_footer->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
        m_footer->setMinimumHeight(QFontMetrics(theme::uiFont(8)).lineSpacing() * 2 + 2);
        layout->addWidget(m_footer);

        connect(m_search, &QLineEdit::textChanged, this,
                [this](const QString &text) { populate(text); });
        connect(m_list, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *item, int) { pick(item); });
        connect(m_list, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem *item, int) { pick(item); });
        connect(m_list, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *, QTreeWidgetItem *) { updateFooter(); });

        // Once, not per keystroke: building the index resolves two dozen method
        // names through a search over 29k rows apiece, and the only thing it
        // depends on is the class, which cannot change while a popup is open.
        if (m_doc) {
            const Graph *g = m_doc->activeGraph();
            m_index = nodeIndex(m_doc->catalog(), m_doc->builtins(),
                                g ? selfClassOf(*g) : QString());
        }

        // The caret stays in the search box the whole time; the arrow keys and
        // Return are handed to the list from here so a pick never needs a Tab.
        m_search->installEventFilter(this);
        resize(440, 400);
        populate(QString());
    }

    // Narrows the list to nodes a wire from this pin could actually land on:
    // every row offered afterwards has at least one pin the source connects to.
    // A drag from an output needs candidates with a matching input, and the
    // reverse.
    void setCompatibleWith(const PinType &type, PinDir sourceDir)
    {
        m_fitType = type;
        m_fitDir = sourceDir;
        m_fitting = true;
        m_search->setPlaceholderText(tr("Connect to: type to search"));
        populate(m_search->text());
    }

    void popupAt(const QPoint &globalPos)
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_search || event->type() != QEvent::KeyPress)
            return QWidget::eventFilter(watched, event);

        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            QCoreApplication::sendEvent(m_list, key);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            pick(m_list->currentItem());
            return true;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void pick(QTreeWidgetItem *item)
    {
        // A click and an activation can both land on one gesture; the first one
        // through wins, so a double-click cannot add the node twice.
        if (m_picked || !item) return;
        const QString key = item->data(0, kKeyRole).toString();
        if (key.isEmpty()) return;
        m_picked = true;
        // Closing first keeps the popup's mouse grab off the canvas while the
        // node is being added and selected.
        close();
        if (m_onPick) m_onPick(key);
    }

    // A heading, or nullptr when the group turned out empty. Headings are not
    // pickable and carry the group's reason for existing, so the footer says
    // why the group is where it is when the arrow keys land on one.
    QTreeWidgetItem *addHeading(const QString &title, const QString &doc)
    {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, title);
        item->setFlags(Qt::ItemIsEnabled);
        item->setFirstColumnSpanned(true);
        item->setFont(0, theme::uiFont(8, true));
        item->setForeground(0, theme::textDim());
        item->setData(0, kDocRole, doc);
        if (!doc.isEmpty()) item->setToolTip(0, doc);
        return item;
    }

    void addRow(const QString &title, const QString &key, const QString &detail,
                const QString &doc = QString(), QTreeWidgetItem *under = nullptr)
    {
        auto *item = under ? new QTreeWidgetItem(under) : new QTreeWidgetItem(m_list);
        item->setText(0, title);
        item->setData(0, kKeyRole, key);
        item->setData(0, kDocRole, doc);
        item->setText(1, detail);
        item->setForeground(1, theme::textDim());
        if (!doc.isEmpty()) item->setToolTip(0, doc);
    }

    // True when a wire from the dragged pin has somewhere to land on this node.
    // Everything outside connect mode passes.
    bool defFits(const NodeDef &def) const
    {
        if (!m_fitting) return true;
        if (!def.valid || !m_doc) return false;
        for (const Pin &p : def.pins)
            if (pinWouldFit(m_doc->catalog(), m_fitType, m_fitDir, p)) return true;
        return false;
    }

    bool keyFits(const QString &key) const
    {
        if (!m_fitting) return true;
        if (!m_doc) return false;
        // An action row places nothing, so there is no pin for a wire to reach.
        if (key == kPromoteKey || key == kAddEventKey) return false;
        // defFor is memoised, so walking a ranking costs one def build per
        // entry per session and nothing after that.
        return defFits(key.startsWith(QLatin1String("bi."))
                           ? m_doc->builtins().def(key)
                           : m_doc->catalog().defFor(key));
    }

    // What the dragged pin carries, in the words the canvas paints on it.
    QString draggedTypeWord() const
    {
        if (m_fitType.kind == PinKind::Object || m_fitType.kind == PinKind::Enum)
            return m_fitType.cls.isEmpty() ? pinKindName(m_fitType.kind) : m_fitType.cls;
        return pinKindName(m_fitType.kind);
    }

    // The line under the list. In connect mode it says what the list was
    // narrowed to, because a short list is otherwise indistinguishable from a
    // broken one. Otherwise it teaches the gesture that does the narrowing,
    // which nothing on screen would otherwise mention.
    QString standingLine() const
    {
        if (!m_fitting) {
            return tr("Tip: drag a wire off any pin and let go on empty canvas. This "
                      "menu then lists only the nodes that fit it.");
        }
        const QString word = draggedTypeWord();
        if (m_fitType.kind == PinKind::Exec) {
            return m_fitDir == PinDir::Out
                       ? tr("Showing nodes that can run next.")
                       : tr("Showing nodes that can run before this one.");
        }
        return m_fitDir == PinDir::Out
                   ? tr("Showing nodes with an input that takes %1.").arg(word)
                   : tr("Showing nodes with an output that gives %1.").arg(word);
    }

    void updateFooter()
    {
        m_mode->setText(standingLine());
        const QTreeWidgetItem *item = m_list->currentItem();
        // The empty state's first line is not selectable, so nothing is current
        // when it is showing and its explanation would never be read.
        if (!item) item = m_list->topLevelItem(0);
        const QString doc = item ? item->data(0, kDocRole).toString() : QString();
        m_footer->setText(doc.isEmpty()
                              ? tr("Pick a row to see what it does before you place it.")
                              : doc);
    }

    void populate(const QString &query)
    {
        m_list->setUpdatesEnabled(false);
        m_list->clear();
        const QString q = query.trimmed();
        // The same reading the catalogue search takes, so typing two words does
        // not empty the half of this list that is not the catalogue.
        const auto matches = [&q](const QString &a, const QString &b) {
            return matchesQuery(q, {a, b});
        };

        // The value the wire is carrying has no member behind it yet, and this
        // is where it gets one. First row because it is the answer whenever the
        // catalogue has nothing that fits. Exec pins carry no value to store.
        //
        // The type word is dropped in rather than the article being chosen for
        // it: "a EntityAI" is what picking one gives you.
        if (m_fitting && m_fitType.kind != PinKind::Exec
            && matches(tr("Promote to variable"), QString()))
            addRow(tr("Promote to variable"), kPromoteKey, tr("new member"),
                   tr("Declares a member shaped like the %1 pin you dragged, and wires "
                      "it up. The answer when nothing in the catalogue fits.")
                       .arg(draggedTypeWord()));

        // Variables lead: they are the one node family that exists only in this
        // graph, so no amount of catalogue searching would turn them up.
        const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
        QTreeWidgetItem *varHead = nullptr;
        if (g && !g->variables.isEmpty()) {
            for (const GraphVariable &v : g->variables) {
                if (!matches(v.name, v.type)) continue;
                // Get has only an output and Set takes the value on an input,
                // so which of the two is offered follows the drag's direction.
                // The defs are only built when there is a pin to test them
                // against; outside connect mode both rows always show.
                const bool get =
                    !m_fitting
                    || defFits(m_doc->builtins().variableDef(v, false, m_doc->catalog()));
                const bool set =
                    !m_fitting
                    || defFits(m_doc->builtins().variableDef(v, true, m_doc->catalog()));
                if (!get && !set) continue;
                if (!varHead)
                    varHead = addHeading(tr("This graph's members"),
                                         tr("Declared on this class in the Variable "
                                            "Manager. Nothing outside this graph has "
                                            "them, so no search would find them."));
                if (get)
                    addRow(tr("Get %1").arg(v.name),
                           QStringLiteral("var.get.%1").arg(v.id), v.type,
                           tr("Reads the member %1.").arg(v.name), varHead);
                if (set)
                    addRow(tr("Set %1").arg(v.name),
                           QStringLiteral("var.set.%1").arg(v.id), v.type,
                           tr("Writes a new value into the member %1.").arg(v.name),
                           varHead);
            }
        }

        // The task index, which is where the answer is when the question is
        // "how do I do X" rather than "what is it called". It leads with the
        // way into the ranked event list, because searching for a node by name
        // is no use to someone who does not know EEItemAttached exists.
        QSet<QString> shown;
        if (m_doc) {
            for (const IndexGroup &group : m_index) {
                QTreeWidgetItem *head = nullptr;
                for (const IndexRow &row : group.rows) {
                    if (!rowMatches(q, row, group.title)) continue;
                    if (!keyFits(row.key)) continue;
                    if (!head) head = addHeading(group.title, group.doc);
                    addRow(row.title, row.key, row.detail, row.doc, head);
                    shown.insert(row.key);
                }
            }
        }

        if (!q.isEmpty() && m_doc) {
            SearchOptions opts;
            opts.limit = m_fitting ? kFitScan : 60;
            const QString self = g ? selfClassOf(*g) : QString();
            // The palette withholds what the generated script could not call
            // and so does this: a node picked here lands on the same graph.
            opts.selfClass = self;
            opts.respectAccess = true;
            QTreeWidgetItem *head = nullptr;
            int kept = 0;
            for (const SearchHit &hit : m_doc->catalog().search(q, opts)) {
                if (shown.contains(hit.key)) continue;
                // The same rule the dock applies: an event this class does not
                // inherit is a method with the right name on the wrong class.
                if (!eventFitsClass(m_doc->catalog(), hit.category, hit.subtitle, self))
                    continue;
                if (m_fitting) {
                    if (kept >= kFitKeep) break;
                    if (!keyFits(hit.key)) continue;
                    ++kept;
                }
                if (!head) head = addHeading(tr("Everything named like that"), QString());
                addRow(hit.title, hit.key,
                       hit.subtitle.isEmpty() ? hit.category : hit.subtitle,
                       nodeSummary(m_doc->catalog(), m_doc->builtins(), hit.key), head);
            }
        }

        if (m_list->topLevelItemCount() == 0) addEmptyState(q);
        m_list->expandAll();

        if (QTreeWidgetItem *first = m_list->topLevelItem(0))
            m_list->setCurrentItem(first->childCount() > 0 ? first->child(0) : first);
        m_list->setUpdatesEnabled(true);
        updateFooter();
    }

    // What to say when nothing survived. Naming the pin rather than the query
    // is what makes a narrowed list readable: the reason there are no rows is
    // the pin, and the two rows that follow are the real ways out of it.
    void addEmptyState(const QString &query)
    {
        auto *empty = new QTreeWidgetItem(m_list);
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(0, theme::textDim());
        empty->setFirstColumnSpanned(true);
        if (m_fitting) {
            const QString word = draggedTypeWord();
            const bool exec = m_fitType.kind == PinKind::Exec;
            QString said;
            if (exec)
                said = m_fitDir == PinDir::Out ? tr("No node can run next from there.")
                                               : tr("No node can run before that one.");
            else
                said = m_fitDir == PinDir::Out
                           ? tr("No node takes %1 as an input.").arg(word)
                           : tr("No node gives %1 as an output.").arg(word);
            if (!query.isEmpty())
                said = tr("Nothing named \"%1\" fits that pin.").arg(query);
            empty->setText(0, said);
            empty->setData(0, kDocRole,
                           exec ? tr("A Raw Enforce node takes the flow and keeps the "
                                     "statement as text.")
                                : tr("Promote to variable stores the value on the class "
                                     "instead, and a Raw Expression writes it as "
                                     "Enforce."));
            // An exec pin has no value to write as an expression, and a value
            // pin has no flow for a statement node to take.
            if (exec)
                addRow(tr("Raw Enforce"), QStringLiteral("bi.raw"), tr("inline code"),
                       tr("Keeps the statement as text and still generates."));
            else
                addRow(tr("Raw Expression"), QStringLiteral("bi.rawExpr"),
                       tr("inline value"),
                       tr("Writes the value as Enforce text. Still generates, and the "
                          "importer can read some of it back into nodes later."));
            return;
        }
        empty->setText(0, tr("No node is named \"%1\".").arg(query));
        empty->setData(0, kDocRole,
                       tr("The search reads names, owning classes and signatures. Every "
                          "term has to land somewhere on the row."));
        addRow(tr("Browse events instead"), kAddEventKey,
               tr("what this class can hook"),
               tr("A hook is found by the moment it fires, not by its name. That list "
                  "is ranked and grouped; this search is not."));
        addRow(tr("Raw Enforce"), QStringLiteral("bi.raw"), tr("inline code"),
               tr("Keeps the line as text and still generates. Refusing to guess is "
                  "why the importer never rewrites code it did not understand."));
    }

    Document *m_doc;
    std::function<void(const QString &)> m_onPick;
    QLineEdit *m_search;
    QTreeWidget *m_list;
    QLabel *m_mode;
    QLabel *m_footer;
    // The task index for the class this popup opened on, built once.
    QVector<IndexGroup> m_index;
    bool m_picked = false;
    // Connect mode: the pin a wire was dragged off, and the direction it left
    // in. Unset means the plain add-node search.
    PinType m_fitType;
    PinDir m_fitDir = PinDir::Out;
    bool m_fitting = false;
};

} // namespace

MainWindow::MainWindow(Document *doc, QWidget *parent)
    : QMainWindow(parent)
    , m_doc(doc)
{
    setWindowTitle(QStringLiteral("SUDO DayZ Node Mod"));
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);

    m_scene = new NodeScene(m_doc, this);
    m_view = new NodeView(m_scene, this);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tabRow = new QWidget(central);
    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(0);

    m_tabs = new QTabBar(tabRow);
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setUsesScrollButtons(true);
    // A long name degrades to an ellipsis instead of being cut off mid-word at
    // the clip, and the dropdown beside the bar reaches every script whatever
    // the bar can fit.
    m_tabs->setElideMode(Qt::ElideRight);
    // Room for a name to survive.
    //
    // Once the tabs no longer fit, QTabBar lays every one of them out at its
    // minimum rather than at its size hint, so this floor is the whole of what a
    // tab gets in any project big enough to scroll. Measured on the 25 script
    // project in resources, at 1600x950 and again at 1280x800:
    //
    //   theme.cpp's 64px, no cross   18 of 25 elided, and five of them read
    //                                "SUDO_Com..." with no way to tell which
    //   120px, with the cross        12 of 25 elided, three pairs still identical
    //   140px, with the cross        5 of 25 elided, no two the same
    //
    // 140 is 120 plus the 18 the cross and its gap take, which is what makes it
    // the number: it hands back exactly what the cross costs. Wider is not free.
    // Every tab pays this, so the bar starts scrolling sooner, and scrolling is
    // the recoverable end of the trade: the dropdown beside the bar and Go to
    // script reach any script whatever the bar can fit, while two tabs that read
    // the same are a coin toss every time.
    //
    // On the widget rather than in theme.cpp because that file's QTabBar rules
    // also style the dock tab bars, where "Events" and "MiniMap" would be padded
    // out to nothing.
    m_tabs->setStyleSheet(
        QStringLiteral("QTabBar::tab { min-width: 140px; padding-right: 6px; }"));
    m_tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    // Middle-click closes, which is what people try before they look for a
    // cross. QTabBar reads the left button and nothing else, so the release has
    // to be caught here or it reaches nothing that could act on it.
    m_tabs->installEventFilter(this);
    tabLayout->addWidget(m_tabs, 1);

    m_tabList = new QToolButton(tabRow);
    m_tabList->setText(QStringLiteral("▾"));
    m_tabList->setToolTip(QStringLiteral("All scripts"));
    m_tabList->setAutoRaise(true);
    tabLayout->addWidget(m_tabList);

    layout->addWidget(tabRow);

    // Directly over the canvas, because that is where the graph is read and the
    // one place a warning about it cannot be missed. Hidden for the user's own
    // scripts, which is every script until they open the mod browser.
    m_readOnlyBar = new QLabel(central);
    m_readOnlyBar->setObjectName(QStringLiteral("readOnlyBar"));
    m_readOnlyBar->setContentsMargins(8, 4, 8, 4);
    m_readOnlyBar->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_readOnlyBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // The line is elided to the bar's own width, and the window's resize event
    // arrives before the layout has given the bar its new one. Watching the bar
    // itself is what keeps the text and the width in step.
    m_readOnlyBar->installEventFilter(this);
    m_readOnlyBar->setStyleSheet(
        QStringLiteral("QLabel#readOnlyBar { background: %1; color: %2; "
                       "border-bottom: 1px solid %3; }")
            .arg(theme::headerBg().name(), theme::warningColor().name(),
                 theme::warningColor().name()));
    m_readOnlyBar->hide();
    layout->addWidget(m_readOnlyBar);

    layout->addWidget(m_view, 1);

    m_recent.load();
    m_editor = central;
    m_startPage = new StartPage(&m_recent, this);
    // Asks for no height of its own. A stack is as tall as the tallest page it
    // holds whichever one is showing, and the start page's three columns wanted
    // 547 of it, so the page nobody is looking at while the editor is up was
    // setting the floor under every dock in it: a window asked for 800 tall came
    // back 814, and the bottom row could not be given more than 186 however hard
    // it asked, which is three rows in each of the Mod Browser's two lists. The
    // page's own columns scroll, so it gives up nothing by shrinking.
    m_startPage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_editor);
    m_stack->addWidget(m_startPage);
    setCentralWidget(m_stack);

    buildMenus();
    buildToolBar();
    buildDocks();
    buildStatusBar();

    connect(m_tabList, &QToolButton::clicked, this, &MainWindow::showTabList);
    connect(m_tabs, &QTabBar::customContextMenuRequested,
            this, &MainWindow::showTabMenu);

    connect(m_doc, &Document::graphChanged, this, &MainWindow::onGraphChanged);
    connect(m_doc, &Document::projectChanged, this, &MainWindow::onProjectChanged);
    connect(m_doc, &Document::activeScriptChanged, this, [this]() {
        m_scene->rebuild();
        runAnalysis();
        refreshTabs();
        m_view->zoomToFit();
    });
    connect(m_doc, &Document::graphChanged, this, &MainWindow::updateReadOnlyBar);
    connect(m_doc, &Document::modifiedChanged, this, [this](bool) { updateWindowTitle(); });
    connect(m_tabs, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_palette, &PalettePanel::nodeRequested,
            this, &MainWindow::onPaletteNodeRequested);
    // The palette's index leads with the row that hands over to the events
    // list, because a hook is found by the moment it fires and not by its name.
    // The dock is raised and its search focused, so the handover lands on a
    // list that is ready to be typed into rather than on a tab behind another.
    connect(m_palette, &PalettePanel::eventsRequested, this, [this] {
        if (QDockWidget *dock = dockOf(m_events)) {
            if (!dock->toggleViewAction()->isChecked()) dock->toggleViewAction()->trigger();
            dock->raise();
            raiseDockTab(this, dock);
        }
        m_events->focusSearch();
    });
    connect(m_events, &EventsPanel::eventRequested, this, &MainWindow::placeEventNode);
    connect(m_events, &EventsPanel::customEventRequested,
            this, &MainWindow::addCustomEvent);
    connect(m_events, &EventsPanel::statusMessage, this, &MainWindow::flashStatus);
    connect(m_view, &NodeView::contextAddRequested, this, &MainWindow::onContextAdd);
    connect(m_outliner, &OutlinerPanel::nodeActivated, this, [this](const QString &id) {
        revealNode(m_doc, m_view, m_inspector, id);
    });
    connect(m_variables, &VariablesPanel::variableNodeRequested, this,
            [this](const QString &id, bool setter) {
                if (id.isEmpty()) return;
                onPaletteNodeRequested(QStringLiteral("var.%1.%2")
                                           .arg(setter ? QStringLiteral("set")
                                                       : QStringLiteral("get"), id));
            });
    connect(m_scene, &NodeScene::statusMessage, this, [this](const QString &t) {
        flashStatus(t);
    });
    connect(m_scene, &NodeScene::wireDroppedOnEmpty, this,
            [this](const PinRef &from, const QPointF &at) {
                showConnectSearch(from, at);
            });
    // Double-click is the fast way into a raw node's code. The inspector's
    // button is the one you find without being told.
    connect(m_scene, &NodeScene::nodeDoubleClicked, this, [this](const QString &id) {
        const Graph *g = m_doc->activeGraph();
        const GraphNode *node = g ? g->node(id) : nullptr;
        if (!node || !isCodeNode(*node)) return;
        if (refuseReadOnlyEdit()) return;
        CodeDialog::editNodeCode(this, m_doc, id);
    });

    connect(m_startPage, &StartPage::openRequested, this, &MainWindow::openProjectPath);
    connect(m_startPage, &StartPage::browseRequested, this, &MainWindow::openProject);
    connect(m_startPage, &StartPage::newProjectRequested, this, &MainWindow::newProject);
    connect(m_startPage, &StartPage::newModRequested, this, &MainWindow::newMod);
    connect(m_startPage, &StartPage::browseModsRequested,
            this, &MainWindow::browseInstalledMods);
    connect(m_startPage, &StartPage::templateRequested,
            this, &MainWindow::startFromTemplate);
    connect(m_startPage, &StartPage::statusMessage, this, &MainWindow::flashStatus);

    // Two minutes: long enough that the write is never in the way of typing,
    // short enough that a crash costs an edit or two rather than a session.
    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setInterval(120000);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::writeAutosave);
    m_autosaveTimer->start();

    refreshTabs();
    m_scene->rebuild();
    runAnalysis();
    // The editor is built and stays built; the start page is what sits in front
    // of it until a project is opened. main() opening one from the command line
    // switches to the editor before the window is shown.
    showStartPage();
    updateWindowTitle();
}

void MainWindow::showStartPage()
{
    if (m_stack->currentWidget() == m_startPage) {
        m_startPage->refresh();
        return;
    }
    // A dock the user closed from the View menu stays closed when the editor
    // comes back, so only the ones that were open are put away.
    m_putAwayDocks.clear();
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        if (dock->isHidden()) continue;
        m_putAwayDocks.append(dock);
        dock->hide();
    }
    m_putAwayToolBar = m_toolBar && !m_toolBar->isHidden();
    if (m_putAwayToolBar) m_toolBar->hide();

    m_stack->setCurrentWidget(m_startPage);
    m_startPage->refresh();
    updateWindowTitle();
}

void MainWindow::showEditor()
{
    if (m_stack->currentWidget() == m_editor) return;
    m_stack->setCurrentWidget(m_editor);
    for (const QPointer<QDockWidget> &dock : m_putAwayDocks)
        if (dock) dock->show();
    m_putAwayDocks.clear();
    if (m_putAwayToolBar && m_toolBar) m_toolBar->show();
    m_putAwayToolBar = false;
    updateWindowTitle();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    // First, because it is the way back to everything under it once a project
    // is open.
    file->addAction(QStringLiteral("Start page"), this, &MainWindow::showStartPage);
    file->addSeparator();
    file->addAction(QStringLiteral("New mod..."),
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
                    this, &MainWindow::newMod);
    file->addAction(QStringLiteral("New project"), QKeySequence::New,
                    this, &MainWindow::newProject);
    file->addAction(QStringLiteral("Open project..."), QKeySequence::Open,
                    this, &MainWindow::openProject);
    // Beside Open project, because "open somebody else's mod" is the same
    // question asked about a different file, and it was the one thing in this
    // app with no way in from the menu bar at all.
    file->addAction(QStringLiteral("Open mod or pbo..."),
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
                    this, &MainWindow::openModFromDisk);
    file->addAction(QStringLiteral("Browse installed mods"),
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B),
                    this, &MainWindow::browseInstalledMods);
    file->addSeparator();
    file->addAction(QStringLiteral("Set mod folder..."), this,
                    &MainWindow::setModFolder);
    file->addSeparator();
    file->addAction(QStringLiteral("Save"), QKeySequence::Save,
                    this, &MainWindow::saveProject);
    file->addAction(QStringLiteral("Save as..."), QKeySequence::SaveAs,
                    this, &MainWindow::saveProjectAs);
    file->addSeparator();
    file->addAction(QStringLiteral("Save script to file..."),
                    QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S),
                    this, &MainWindow::saveScriptFile);
    file->addAction(QStringLiteral("Export scripts..."), this, &MainWindow::exportScripts);
    file->addSeparator();
    // Here rather than under View, because a tab is not a window onto a file
    // that carries on existing: closing one takes the script out of the project,
    // which is a File-menu sized thing to do.
    file->addAction(QStringLiteral("Close script"), QKeySequence(Qt::CTRL | Qt::Key_W),
                    this, &MainWindow::closeActiveScript);
    m_reopenAction =
        file->addAction(QStringLiteral("Reopen closed script"),
                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T),
                        this, &MainWindow::reopenClosedScript);
    m_reopenAction->setEnabled(false);
    file->addSeparator();
    file->addAction(QStringLiteral("Edit mod config..."), this,
                    &MainWindow::editModConfig);
    file->addSeparator();
    // No shortcut. QKeySequence::Quit has no chord on Windows and resolves to
    // the soft key spelled Exit, so asking for it put the word Exit in the
    // shortcut column and the row read as its own label twice. Alt+F4 is the
    // platform's own way out and the window manager already serves it.
    file->addAction(QStringLiteral("Exit"), this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    edit->addAction(QStringLiteral("Undo"), QKeySequence::Undo, m_doc, &Document::undo);
    QAction *redo = edit->addAction(QStringLiteral("Redo"), m_doc, &Document::redo);
    // The single-sequence overload takes only the first platform binding, which
    // on Windows is Ctrl+Y, leaving the contracted Ctrl+Shift+Z unbound.
    redo->setShortcuts(QKeySequence::Redo);
    edit->addSeparator();
    edit->addAction(QStringLiteral("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D),
                    this, [this]() {
        if (refuseReadOnlyEdit()) return;
        m_scene->duplicateSelection();
    });
    edit->addAction(QStringLiteral("Delete"), QKeySequence::Delete,
                    this, [this]() {
        if (refuseReadOnlyEdit()) return;
        m_scene->deleteSelectedNodes();
    });
    edit->addSeparator();
    edit->addAction(QStringLiteral("Add node here..."), QKeySequence(Qt::CTRL | Qt::Key_Space),
                    this, [this]() {
        showAddNodeSearch(m_view->mapToScene(m_view->viewport()->rect().center()));
    });
    edit->addSeparator();
    buildLayoutActions(edit, nullptr);

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("Fit graph"), QKeySequence(Qt::CTRL | Qt::Key_F),
                    this, [this]() { m_view->zoomToFit(); });
    view->addAction(QStringLiteral("Reset zoom"), QKeySequence(Qt::CTRL | Qt::Key_0),
                    this, [this]() { m_view->resetZoom(); });
    view->addSeparator();
    // The tab bar scrolls once a project carries more scripts than the window is
    // wide, and scrolling is not a way to find a name you already know.
    view->addAction(QStringLiteral("Go to script..."), QKeySequence(Qt::CTRL | Qt::Key_P),
                    this, &MainWindow::showTabList);
    view->addSeparator();
    // Dock toggles are appended in buildDocks once the docks exist.

    QMenu *tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    tools->addAction(QStringLiteral("Edit code"), QKeySequence(Qt::Key_F2),
                     this, &MainWindow::editSelectedCode);
    tools->addSeparator();
    tools->addAction(QStringLiteral("Generate Enforce Script"),
                     QKeySequence(Qt::Key_F7), this, &MainWindow::showGeneratedCode);
    tools->addAction(QStringLiteral("Validate graph"), QKeySequence(Qt::Key_F8),
                     this, [this]() {
        runAnalysis();
        flashStatus(m_analysis.errors == 0 && m_analysis.warnings == 0
                ? QStringLiteral("Graph is clean.")
                : QStringLiteral("%1 errors, %2 warnings.")
                      .arg(m_analysis.errors).arg(m_analysis.warnings));
    });

    // Between Tools and Preferences, because running the mod is the step after
    // generating it. Filled in buildTestMenu once the Test dock exists.
    m_testMenu = menuBar()->addMenu(QStringLiteral("&Test"));

    QMenu *prefs = menuBar()->addMenu(QStringLiteral("&Preferences"));
    prefs->addAction(QStringLiteral("About"), this, [this]() {
        const auto totals = m_doc->catalog().totals();
        QMessageBox::about(
            this, QStringLiteral("SUDO DayZ Node Mod"),
            QStringLiteral("<b>SUDO DayZ Node Mod %1</b><br><br>"
                           "Visual scripting for DayZ Enforce Script.<br><br>"
                           "Catalogue: %2 classes, %3 methods, %4 enums, %5 globals, "
                           "%6 constants<br>Indexed from %7")
                .arg(QCoreApplication::applicationVersion())
                .arg(totals.value("classes")).arg(totals.value("methods"))
                .arg(totals.value("enums")).arg(totals.value("globals"))
                .arg(totals.value("consts"))
                .arg(m_doc->catalog().source()));
    });
}

void MainWindow::buildToolBar()
{
    QToolBar *bar = addToolBar(QStringLiteral("Graph"));
    bar->setMovable(false);
    bar->setIconSize(QSize(16, 16));

    // DESIGN.md order: align/distribute, straighten wires, validate, generate.
    buildLayoutActions(nullptr, bar);
    bar->addSeparator();
    bar->addAction(QStringLiteral("Validate"), this, [this]() { runAnalysis(); });
    bar->addAction(QStringLiteral("Generate"), this, &MainWindow::showGeneratedCode);
    bar->addSeparator();
    // Zoom is not in the contracted toolbar, so it goes last: handy, not part
    // of the promised set.
    bar->addAction(QStringLiteral("Fit"), this, [this]() { m_view->zoomToFit(); });
    bar->addAction(QStringLiteral("100%"), this, [this]() { m_view->resetZoom(); });

    m_toolBar = bar;
    // An expanding blank widget is what pins the mark to the right: a toolbar
    // lays its items out left to right and has no alignment of its own.
    m_toolBarGap = new QWidget(bar);
    m_toolBarGap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBarGap->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->addWidget(m_toolBarGap);
    m_cornerMark = new QLabel(bar);
    m_cornerMark->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_cornerMark->setContentsMargins(0, 0, 6, 0);
    bar->addWidget(m_cornerMark);
    updateCornerMark();
}

void MainWindow::updateCornerMark()
{
    if (!m_toolBar || !m_cornerMark) return;

    // Cut to the bar rather than to a constant: the bar's height follows the
    // user's font size, and a fixed pixmap would either float in it or push it
    // taller. The guard matters because this runs on every resize. Dragging the
    // window to a screen of another density is a recut too, not just a rescale,
    // or the mark goes soft on the second monitor.
    const int height = qBound(14, m_toolBar->height() - 8, 32);
    const qreal ratio = devicePixelRatioF();
    if (height != m_cornerHeight || !qFuzzyIsNull(ratio - m_cornerRatio)) {
        m_cornerHeight = height;
        m_cornerRatio = ratio;
        m_cornerMark->setPixmap(branding::cornerMark(height, ratio));
    }

    // The mark is decoration and the actions are the toolbar's job, so a narrow
    // window loses the mark rather than pushing Fit and 100% behind the
    // overflow arrow.
    const QLayout *layout = m_toolBar->layout();
    const int spacing = layout ? qMax(0, layout->spacing()) : 0;
    int actions = 0;
    for (QAction *action : m_toolBar->actions()) {
        const QWidget *w = m_toolBar->widgetForAction(action);
        if (!w || w == m_toolBarGap || w == m_cornerMark) continue;
        actions += w->sizeHint().width() + spacing;
    }
    const int room = actions + m_cornerMark->sizeHint().width() + 24;
    m_cornerMark->setVisible(m_toolBar->width() >= room);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateCornerMark();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_readOnlyBar && event->type() == QEvent::Resize)
        updateReadOnlyBar();
    // Middle-click closes the tab under the pointer. On release, not on press,
    // so a click that started on a tab and ended somewhere else closes nothing.
    if (watched == m_tabs && event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::MiddleButton) {
            const int index = m_tabs->tabAt(mouse->position().toPoint());
            const QString id = index >= 0 ? m_tabs->tabData(index).toString() : QString();
            if (!id.isEmpty()) {
                closeScripts({id});
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildLayoutActions(QMenu *menu, QToolBar *bar)
{
    struct Entry {
        QString label;
        std::function<void()> run;
    };

    const QVector<Entry> aligns{
        {QStringLiteral("Left edges"), [this]() { m_scene->alignSelection(AlignEdge::Left); }},
        {QStringLiteral("Right edges"), [this]() { m_scene->alignSelection(AlignEdge::Right); }},
        {QStringLiteral("Top edges"), [this]() { m_scene->alignSelection(AlignEdge::Top); }},
        {QStringLiteral("Bottom edges"), [this]() { m_scene->alignSelection(AlignEdge::Bottom); }},
        {QStringLiteral("Centres, vertical axis"),
         [this]() { m_scene->alignSelection(AlignEdge::CentreX); }},
        {QStringLiteral("Centres, horizontal axis"),
         [this]() { m_scene->alignSelection(AlignEdge::CentreY); }},
    };
    const QVector<Entry> spreads{
        {QStringLiteral("Horizontally"),
         [this]() { m_scene->distributeSelection(Qt::Horizontal); }},
        {QStringLiteral("Vertically"),
         [this]() { m_scene->distributeSelection(Qt::Vertical); }},
    };

    const auto fill = [](QMenu *target, const QVector<Entry> &entries) {
        for (const Entry &e : entries) target->addAction(e.label, e.run);
    };

    if (bar) {
        // A flat row of eight buttons would crowd out validate and generate, so
        // the two families collapse into one menu button each.
        auto *alignButton = new QToolButton(bar);
        alignButton->setText(QStringLiteral("Align"));
        alignButton->setPopupMode(QToolButton::InstantPopup);
        auto *alignMenu = new QMenu(alignButton);
        fill(alignMenu, aligns);
        alignButton->setMenu(alignMenu);
        bar->addWidget(alignButton);

        auto *spreadButton = new QToolButton(bar);
        spreadButton->setText(QStringLiteral("Distribute"));
        spreadButton->setPopupMode(QToolButton::InstantPopup);
        auto *spreadMenu = new QMenu(spreadButton);
        fill(spreadMenu, spreads);
        spreadButton->setMenu(spreadMenu);
        bar->addWidget(spreadButton);

        bar->addAction(QStringLiteral("Straighten wires"), this,
                       [this]() { m_scene->straightenWires(); });
    }

    if (menu) {
        QMenu *alignMenu = menu->addMenu(QStringLiteral("Align"));
        fill(alignMenu, aligns);
        QMenu *spreadMenu = menu->addMenu(QStringLiteral("Distribute"));
        fill(spreadMenu, spreads);
        menu->addAction(QStringLiteral("Straighten wires"), this,
                        [this]() { m_scene->straightenWires(); });
    }
}

void MainWindow::buildStatusBar()
{
    // Two labels, not one: the contracted '● N Errors ▲ N Warnings' line is
    // what the user reads to know whether the graph is sound, so a transient
    // note must not be able to take its place.
    m_status = new QLabel(this);
    statusBar()->addWidget(m_status);
    m_message = new QLabel(this);
    statusBar()->addWidget(m_message, 1);

    m_statusResetTimer = new QTimer(this);
    m_statusResetTimer->setSingleShot(true);
    connect(m_statusResetTimer, &QTimer::timeout, this, [this]() {
        m_message->clear();
        m_message->setToolTip(QString());
        setSeverity(m_message, "");
    });

    auto *zoomLabel = new QLabel(this);
    statusBar()->addPermanentWidget(zoomLabel);
    connect(m_view, &NodeView::zoomChanged, zoomLabel, [zoomLabel](double z) {
        zoomLabel->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
    });
    zoomLabel->setText(QStringLiteral("100%"));

    // Both counters and the zoom describe the graph on the canvas. The start
    // page has no graph to have errors in and nothing to zoom, so they step out
    // there and leave the bar to whatever the page has just done.
    //
    // Emptied as well as hidden: a status bar shows and hides its own
    // non-permanent widgets whenever it is asked to lay out again, and a label
    // that came back carrying the last project's counts would be worse than one
    // that had never left.
    const auto followPage = [this, zoomLabel]() {
        const bool editing = m_stack && m_stack->currentWidget() == m_editor;
        if (editing) updateStatusCounts();
        else m_status->clear();
        zoomLabel->setText(editing ? QStringLiteral("%1%").arg(qRound(m_view->zoom() * 100))
                                   : QString());
        m_status->setVisible(editing);
        zoomLabel->setVisible(editing);
    };
    connect(m_stack, &QStackedWidget::currentChanged, this,
            [followPage](int) { followPage(); });
    followPage();
}

void MainWindow::buildDocks()
{
    const auto makeDock = [this](const QString &title, QWidget *body,
                                 Qt::DockWidgetArea area,
                                 Qt::DockWidgetAreas allowed
                                 = Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea) {
        auto *dock = new QDockWidget(title, this);
        dock->setObjectName(title);
        dock->setWidget(body);
        dock->setAllowedAreas(allowed);
        addDockWidget(area, dock);
        return dock;
    };

    m_outliner = new OutlinerPanel(m_doc, this);
    m_palette = new PalettePanel(m_doc, this);
    m_events = new EventsPanel(m_doc, this);
    m_variables = new VariablesPanel(m_doc, this);
    m_inspector = new InspectorPanel(m_doc, this);
    m_minimap = new MiniMapWidget(m_scene, m_view, this);
    m_codeView = new CodeViewPanel(m_doc, this);
    m_explorer = new ExplorerPanel(m_doc, this);
    m_modBrowser = new ModBrowserPanel(m_doc, this);
    m_testRun = new TestPanel(m_doc, this);

    QDockWidget *outlinerDock =
        makeDock(QStringLiteral("Graph Outliner"), m_outliner, Qt::LeftDockWidgetArea);
    QDockWidget *paletteDock =
        makeDock(QStringLiteral("Node Palette"), m_palette, Qt::LeftDockWidgetArea);
    QDockWidget *eventsDock =
        makeDock(QStringLiteral("Events"), m_events, Qt::LeftDockWidgetArea);
    QDockWidget *explorerDock =
        makeDock(QStringLiteral("Mod Explorer"), m_explorer, Qt::LeftDockWidgetArea);
    // A library of 266 mods with a class list under it is two lists, and the
    // left column has room for about three rows of each. Down here it has the
    // window's whole width, and the panel puts its two lists side by side once
    // it is wider than it is tall. Every area is allowed, because the layout is
    // the user's to rearrange and the panel reads either way round.
    QDockWidget *browserDock =
        makeDock(QStringLiteral("Mod Browser"), m_modBrowser, Qt::BottomDockWidgetArea,
                 Qt::AllDockWidgetAreas);
    QDockWidget *varsDock =
        makeDock(QStringLiteral("Variable Manager"), m_variables, Qt::RightDockWidgetArea);
    QDockWidget *inspectorDock =
        makeDock(QStringLiteral("Node Inspector"), m_inspector, Qt::RightDockWidgetArea);
    QDockWidget *minimapDock =
        makeDock(QStringLiteral("MiniMap"), m_minimap, Qt::RightDockWidgetArea);
    // Bottom only: the generated file is wide and short, and putting it beside
    // the canvas would squeeze both.
    QDockWidget *codeDock =
        makeDock(QStringLiteral("Generated Code"), m_codeView, Qt::BottomDockWidgetArea,
                 Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    // Behind the generated file rather than beside it: both want the full width
    // and only one of them is worth watching at a time.
    QDockWidget *testDock =
        makeDock(QStringLiteral("Test"), m_testRun, Qt::BottomDockWidgetArea,
                 Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    // The three wide panels, in the order they are reached: what the graph
    // generates, what somebody else's mod holds, and what happened when it ran.
    // The generated file is in front, which is the contracted layout.
    tabifyDockWidget(codeDock, browserDock);
    tabifyDockWidget(browserDock, testDock);
    codeDock->raise();

    splitDockWidget(outlinerDock, paletteDock, Qt::Vertical);
    splitDockWidget(paletteDock, eventsDock, Qt::Vertical);
    // Two tabs in the last slice rather than two more slices of a column that
    // was already showing the Mod Explorer one line of prose at a time. They are
    // the two you use one at a time: the hooks this class can override, and your
    // own mod's files.
    tabifyDockWidget(eventsDock, explorerDock);
    eventsDock->raise();
    // Queued: the signal arrives while the tab group is still swapping, and a
    // resize asked for there is measured against the layout on its way out.
    connect(browserDock, &QDockWidget::visibilityChanged, this,
            [this]() { QTimer::singleShot(0, this, [this]() { applyBottomRowSize(); }); });
    splitDockWidget(varsDock, inspectorDock, Qt::Vertical);
    splitDockWidget(inspectorDock, minimapDock, Qt::Vertical);

    // Clicking a line in the generated file selects the node behind it, and
    // selecting a node marks the lines it produced.
    connect(m_codeView, &CodeViewPanel::nodeActivated, this, [this](const QString &id) {
        revealNode(m_doc, m_view, m_inspector, id);
    });
    connect(m_doc, &Document::selectionChanged, this, [this]() {
        const QStringList sel = m_doc->selection();
        if (sel.size() == 1) m_codeView->revealNode(sel.first());
    });

    // Direct, and it has to stay direct: Graph is not a registered metatype, so
    // a queued connection could not carry the argument at all.
    connect(m_modBrowser, &ModBrowserPanel::graphRequested, this,
            &MainWindow::openBrowsedGraph, Qt::DirectConnection);
    connect(m_modBrowser, &ModBrowserPanel::statusChanged, this,
            &MainWindow::flashStatus);
    // The start page's Read a mod card says what is actually installed, which
    // the library only knows once its cache has loaded and its scan has run.
    connect(m_modBrowser->library(), &ModLibrary::modsChanged, this,
            &MainWindow::updateModLibraryLine);
    updateModLibraryLine();

    // A .c is a graph, a .cpp is a config tree, a .sdzn is a project, and
    // everything else is text, so the explorer says which of the four it found
    // and the main window opens it the right way.
    connect(m_explorer, &ExplorerPanel::fileActivated, this, &MainWindow::openModFile);
    connect(m_explorer, &ExplorerPanel::scriptActivated, this, &MainWindow::openModScript);
    connect(m_explorer, &ExplorerPanel::configActivated, this, &MainWindow::openModConfig);
    connect(m_explorer, &ExplorerPanel::projectActivated,
            this, &MainWindow::openProjectPath);
    // A failed build is worth seeing with the dock tabbed behind the code view.
    connect(m_testRun, &TestPanel::statusMessage, this, &MainWindow::flashStatus);
    buildTestMenu();
    syncExplorerRoot();

    // Dock toggles go under View, which buildMenus left open for them.
    for (QAction *a : menuBar()->actions()) {
        if (a->text() != QLatin1String("&View") || !a->menu()) continue;
        for (QDockWidget *d : {outlinerDock, paletteDock, eventsDock, explorerDock,
                               browserDock, varsDock, inspectorDock, minimapDock,
                               codeDock, testDock})
            a->menu()->addAction(d->toggleViewAction());
    }
}

void MainWindow::applyDockSizes()
{
    applyBottomRowSize();

    // Three slices for the left column, still near enough to even that no list
    // is starved to feed another. The palette takes the extra because it is now
    // the index the other two are reached through: its first row opens the
    // Events list, and its browse view is twelve groups rather than eight
    // categories, so an even third opened it on three rows and no heading.
    resizeDocks({dockOf(m_outliner), dockOf(m_palette), dockOf(m_events)},
                {210, 260, 220}, Qt::Vertical);

    // The variable table is the panel; the inspector and the minimap both read
    // fine at their own minimum, so the table is what the spare height goes to.
    resizeDocks({dockOf(m_variables), dockOf(m_inspector), dockOf(m_minimap)},
                {320, 230, 150}, Qt::Vertical);
    resizeDocks({dockOf(m_outliner), dockOf(m_variables)}, {320, 380}, Qt::Horizontal);
}

void MainWindow::applyBottomRowSize()
{
    // Nothing to divide before the window has been shown, and the first show
    // calls this itself.
    if (!m_docksSized) return;

    // The generated file is a view of what the canvas already holds, so it opens
    // at the lines the panel asks for and not at the two fifths of the window it
    // used to take. Capped by the window as well, because a dozen lines of a
    // large font on a short screen is the canvas's room again.
    const int usable = qMax(400, height());
    // A quarter, not a third: at 800 tall a third of the window put more pixels
    // under the generated file than were left for the canvas, which is the one
    // thing the app exists to show.
    int wanted = qBound(160, m_codeView->preferredDockHeight(), usable / 4);

    // The browser is the one panel down here holding two lists rather than one
    // scrolling view, so while it is the tab in front the row is worth more. A
    // tab group has one height whichever tab is up, so the share has to follow
    // the tab rather than be picked once.
    QDockWidget *browser = dockOf(m_modBrowser);
    const bool browsing = dockIsInFront(this, browser);
    if (browsing) {
        wanted = qMax(wanted, usable * 2 / 5);
        // And no further. Two fifths of a 950 tall window is 380, and taking all
        // of it left the Node Palette and the Events list on three rows each to
        // put twelve in the browser. The cap is what the left column needs for
        // four rows in each of its three, and it only bites where there is room
        // to share: on a short window the floor below wins and the panel the
        // user just asked for keeps its share.
        wanted = qMin(wanted, qMax(250, usable - 500 - 90));
    }

    // Named by whichever tab is up. resizeDocks brings the dock it is given to
    // the front of its tab group, so naming the code view here while the
    // browser is the tab the user asked for takes it straight back off them.
    resizeDocks({browsing ? browser : dockOf(m_codeView)}, {wanted}, Qt::Vertical);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    // Once. A layout the user has dragged into shape is theirs, and re-running
    // this on every show would take it back off them.
    if (m_docksSized) return;
    m_docksSized = true;

    // Two hooks for the headless UI check, both unset in a normal run. The
    // window is built at one size by main.cpp and the docks only divide the
    // height they are given, so a layout that fails on a small screen cannot be
    // seen at all without being able to ask for that screen.
    const QString size = qEnvironmentVariable("SUDO_UI_SIZE");
    const QStringList wh = size.split(QLatin1Char('x'), Qt::SkipEmptyParts);
    if (wh.size() == 2 && wh.at(0).toInt() > 0 && wh.at(1).toInt() > 0)
        resize(wh.at(0).toInt(), wh.at(1).toInt());

    applyDockSizes();
    // The palette's search results and its empty state cannot be photographed
    // without a query in the box, and nothing types one in a headless run.
    if (m_palette) {
        const QString query = qEnvironmentVariable("SUDO_UI_SEARCH");
        if (!query.isEmpty()) m_palette->search(query);
    }
    // A panel that only fills in for a selected node cannot be photographed at
    // all without one, and nothing clicks the canvas in a headless run.
    const QString pick = qEnvironmentVariable("SUDO_UI_SELECT").trimmed();
    if (!pick.isEmpty() && m_doc) m_doc->setSelection({pick});
    browseForScreenshot();
    openMenuForScreenshot();
    openPopupForScreenshot();

    // The canvas only gets its real width once the docks have taken theirs, and
    // a graph framed against the window's first guess sits off to one side of
    // it. Queued, because the layout runs after this returns.
    QTimer::singleShot(0, this, [this]() { m_view->zoomToFit(); });
}

void MainWindow::browseForScreenshot()
{
    // "3D Printer" opens that mod's first class; "3D Printer#4" opens the fifth
    // row, because the first class in a mod is often one the importer kept as
    // text and a picture of an empty canvas says nothing about the browser. A
    // value naming something on disk is opened as a path instead, which is the
    // other half of the feature and needs a picture of its own.
    QString wanted = qEnvironmentVariable("SUDO_UI_BROWSE").trimmed();
    if (wanted.isEmpty() || !m_modBrowser) return;
    int row = 0;
    const int hash = wanted.lastIndexOf(QLatin1Char('#'));
    if (hash > 0) {
        row = qMax(0, wanted.mid(hash + 1).toInt());
        wanted = wanted.left(hash);
    }

    // The same call the File menu makes, so the picture is of what a user gets
    // rather than of a state only this hook can reach. It also switches to the
    // editor, and a dock shown over the start page is a dock in a place the app
    // never puts one.
    browseInstalledMods();

    // The scan is on a worker thread and the import is spread over timer ticks.
    // Both are waited out here rather than hoped for, because a picture taken
    // during either shows an empty list and says nothing.
    const auto settle = [](const std::function<bool()> &busy, int ms) {
        QElapsedTimer clock;
        clock.start();
        while (busy() && clock.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        QCoreApplication::processEvents();
    };

    ModLibrary *library = m_modBrowser->library();
    settle([library]() { return library->isScanning(); }, 60000);

    if (QFileInfo::exists(wanted)) {
        if (!m_modBrowser->openFromDisk(wanted)) return;
    } else {
        QString folder;
        for (const ModEntry &mod : library->mods()) {
            if (!mod.hasScripts()) continue;
            if (!mod.name.contains(wanted, Qt::CaseInsensitive)
                && !mod.folderName.contains(wanted, Qt::CaseInsensitive))
                continue;
            folder = mod.folder;
            break;
        }
        if (folder.isEmpty() || !m_modBrowser->selectMod(folder)) return;
    }

    ModBrowserPanel *browser = m_modBrowser;
    settle([browser]() { return browser->isOpening(); }, 60000);
    if (!m_modBrowser->openClassAt(row)) m_modBrowser->openClassAt(0);
    QCoreApplication::processEvents();
}

void MainWindow::openMenuForScreenshot()
{
    // A menu is its own top level window, so a picture of the main window alone
    // never shows one. main.cpp paints any open popup into the grab; this is the
    // half that opens it, matched on the title with the & accelerator taken out.
    const QString wanted = qEnvironmentVariable("SUDO_UI_MENU").trimmed();
    if (wanted.isEmpty()) return;
    for (QAction *action : menuBar()->actions()) {
        QMenu *menu = action->menu();
        if (!menu) continue;
        QString title = action->text();
        title.remove(QLatin1Char('&'));
        if (title.compare(wanted, Qt::CaseInsensitive) != 0) continue;
        menuBar()->setActiveAction(action);
        const QRect where = menuBar()->actionGeometry(action);
        menu->popup(menuBar()->mapToGlobal(where.bottomLeft()));
        return;
    }
}

void MainWindow::openPopupForScreenshot()
{
    const QString wanted = qEnvironmentVariable("SUDO_UI_POPUP").trimmed().toLower();
    if (wanted.isEmpty() || !m_view) return;

    // Left of centre and high, so the popup sits over canvas rather than over
    // the docks it is being judged beside.
    const QPoint at = m_view->viewport()->mapToGlobal(
        QPoint(m_view->viewport()->width() / 3, m_view->viewport()->height() / 5));
    const QPointF scenePos = m_view->mapToScene(m_view->mapFromGlobal(at));

    // The tab bar's own two surfaces. The menu is a QMenu and main.cpp paints
    // any of those into the grab by itself, so this one is finished here.
    if (wanted == QLatin1String("tabmenu")) {
        const int index = m_tabs->currentIndex();
        if (QMenu *menu = buildTabMenu(index))
            menu->popup(m_tabs->mapToGlobal(m_tabs->tabRect(index).bottomLeft()));
        QCoreApplication::processEvents();
        return;
    }

    if (wanted == QLatin1String("scripts")) {
        showTabList();
    } else if (wanted == QLatin1String("event")) {
        showEventSearch(scenePos);
    } else if (wanted.startsWith(QLatin1String("connect"))) {
        // Opened through showConnectSearch rather than by building the popup
        // here, so the picture is of the path a wire drag really takes.
        const PinRef from =
            dragSourceForScreenshot(m_doc, wanted.endsWith(QLatin1String("exec")));
        if (!from.valid) return;
        showConnectSearch(from, scenePos);
    } else {
        showAddNodeSearch(scenePos);
    }

    // Both of those defer the popup by one turn of the event loop so it goes up
    // after the gesture that opened it. There is no gesture here, so the turn
    // has to be taken by hand.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (!top->isVisible() || !top->isWindow()) continue;
        const QString name = top->objectName();
        if (name != QLatin1String("addNodePopup") && name != QLatin1String("eventPopup")
            && name != QLatin1String("scriptListPopup"))
            continue;
        // The pixmap is the popup's own rendering at its own size and position.
        // Painted into a child of the window because that is what win.grab()
        // reaches; the popup itself is then closed rather than left holding the
        // mouse grab.
        auto *overlay = new QLabel(this);
        overlay->setPixmap(top->grab());
        overlay->setFixedSize(top->size());
        overlay->move(mapFromGlobal(top->mapToGlobal(QPoint(0, 0))));
        overlay->raise();
        overlay->show();
        top->close();
        return;
    }
}

void MainWindow::buildTestMenu()
{
    if (!m_testMenu || !m_testRun) return;
    // The panel owns the actions, so a shortcut pressed with the dock closed
    // does the same thing the button does. Adding them here is also what puts
    // the shortcuts on the window: an action nothing has added is never heard.
    m_testMenu->addAction(m_testRun->linkAction());
    m_testMenu->addAction(m_testRun->buildAction());
    m_testMenu->addSeparator();
    // Above the launch entries because it decides what they load, and on the
    // menu at all because the dock it lives in is tabbed behind the generated
    // file: a button nobody can see is a feature nobody finds.
    m_testMenu->addAction(m_testRun->modsAction());
    m_testMenu->addSeparator();
    m_testMenu->addAction(m_testRun->launchAction());
    m_testMenu->addAction(m_testRun->stopAction());
    m_testMenu->addSeparator();
    m_testMenu->addAction(m_testRun->recheckAction());
    m_testMenu->addAction(m_testRun->toolsAction());
}

void MainWindow::refreshTabs()
{
    QSignalBlocker block(m_tabs);
    while (m_tabs->count() > 0) m_tabs->removeTab(0);
    const Project &p = m_doc->project();
    int active = 0;
    for (int i = 0; i < p.scripts.size(); ++i) {
        const ScriptEntry &s = p.scripts.at(i);
        m_tabs->addTab(s.name);
        m_tabs->setTabData(i, s.id);
        // A browsed class has no file of its own to name, and saying where it
        // really came from is the more useful answer anyway.
        const QString origin = graphIsReadOnly(s.graph) ? graphOrigin(s.graph) : QString();
        if (origin.isEmpty()) {
            m_tabs->setTabToolTip(i, QStringLiteral("%1/%2.c").arg(s.folder, s.name));
        } else {
            m_tabs->setTabToolTip(
                i, QStringLiteral("Read only, from %1").arg(origin));
            // Dimmed, so a tab that cannot be exported does not read as one of
            // the user's own while it is sitting in the background.
            m_tabs->setTabTextColor(i, theme::textDim());
        }

        // The bar owns the button from here: removeTab hides it and defers its
        // delete, which is what makes closing from inside the button's own click
        // safe even though the first thing the close does is rebuild this bar.
        auto *cross = new TabCloseButton(s.name, m_tabs);
        const QString id = s.id;
        connect(cross, &QAbstractButton::clicked, this,
                [this, id]() { closeScripts({id}); });
        m_tabs->setTabButton(i, QTabBar::RightSide, cross);

        if (s.id == p.activeId) active = i;
    }
    m_tabs->setCurrentIndex(active);
    // A bar told to make a tab other than the first one current, at a moment
    // when it has not been laid out yet, scrolls to reach it and leaves its own
    // layout dirty: it then reports a height of zero and the whole row vanishes
    // off the top of the editor while the tabs are all still in it. Nothing
    // resizes it afterwards, so nothing ever asks it to think again. Every open
    // that lands on the first script misses this, because setting an index that
    // is already current does nothing at all.
    m_tabs->updateGeometry();
    updateReadOnlyBar();
    updateReopenAction();
}

void MainWindow::updateReadOnlyBar()
{
    if (!m_readOnlyBar) return;
    const Graph *g = m_doc->activeGraph();
    if (!g || !graphIsReadOnly(*g)) {
        m_readOnlyBar->hide();
        return;
    }

    const QString origin = graphOrigin(*g);
    const QString head = QStringLiteral("Read only. %1 was read out of ").arg(g->className);
    // Says what is guaranteed and no more. Export leaves it out and no write
    // this window makes puts it in the user's mod; the Copy and Save as buttons
    // under the canvas are still the user's to press, on code they are looking
    // at, and a bar that claimed otherwise would be wrong the first time
    // somebody tried it.
    const QString tail = QStringLiteral(". Export leaves it out, and nothing here "
                                        "writes it into your mod.");
    const QString where = origin.isEmpty() ? QStringLiteral("another author's mod")
                                           : origin;
    m_readOnlyBar->setToolTip(head + where + tail);

    // Three whole sentences for three widths, and the order they are given up in
    // is the order they are worth. The pbo path goes first, then the file it was
    // in; the rule itself is the last thing to go, because a bar with room for
    // the file name and not for what the file may not do is the wrong half. The
    // tooltip keeps all of it whatever is shown.
    //
    // Written out rather than elided. Left eliding the origin cut it inside the
    // archive's own name and put the cut mark straight in front of the suffix,
    // so the bar read "was read out of ....pbo/Scripts/4_World": four dots and a
    // path starting nowhere. The file and the mod are the two parts of an origin
    // worth keeping, and dropping the second half of the tail with them is what
    // makes the line fit rather than fall through to the shortest one.
    const int colon = origin.indexOf(QLatin1String(": "));
    const QString modName = colon > 0 ? origin.left(colon) : QString();
    const QString fileName = origin.section(QLatin1Char('/'), -1);
    const QString middle =
        modName.isEmpty() || fileName.isEmpty()
            ? QString()
            : QStringLiteral("Read only. %1 was read out of %2 in %3. Export leaves "
                             "it out.")
                  .arg(g->className, fileName, modName);
    const QString shorter =
        QStringLiteral("Read only. %1 came out of another mod, and export leaves it out.")
            .arg(g->className);
    const QFontMetrics metrics(m_readOnlyBar->font());
    const int avail = qMax(120, m_readOnlyBar->width() - 16);

    QString shown;
    if (metrics.horizontalAdvance(head + where + tail) <= avail)
        shown = head + where + tail;
    else if (!middle.isEmpty() && metrics.horizontalAdvance(middle) <= avail)
        shown = middle;
    else
        shown = metrics.elidedText(shorter, Qt::ElideRight, avail);
    m_readOnlyBar->setText(shown);
    m_readOnlyBar->show();
}

bool MainWindow::refuseReadOnlyEdit()
{
    const Graph *g = m_doc->activeGraph();
    if (!g || !graphIsReadOnly(*g)) return false;
    flashStatus(QStringLiteral("%1 is read only. It came out of another mod, so "
                               "it is there to be read.")
                    .arg(g->className));
    m_message->setToolTip(graphOrigin(*g));
    return true;
}

void MainWindow::onTabChanged(int index)
{
    if (index < 0) return;
    const QString id = m_tabs->tabData(index).toString();
    if (!id.isEmpty()) m_doc->setActiveScript(id);
}

void MainWindow::editSelectedCode()
{
    if (refuseReadOnlyEdit()) return;
    const QStringList selection = m_doc->selection();
    const Graph *g = m_doc->activeGraph();
    const GraphNode *node = (selection.size() == 1 && g)
                                ? g->node(selection.first())
                                : nullptr;
    if (!node || !isCodeNode(*node)) {
        flashStatus(QStringLiteral("Select one raw or comment node to edit its code."));
        return;
    }
    CodeDialog::editNodeCode(this, m_doc, node->id);
}

void MainWindow::onProjectChanged()
{
    refreshTabs();
    updateWindowTitle();
    syncExplorerRoot();
}

void MainWindow::syncExplorerRoot()
{
    // Called from every place the mod folder can change, which is exactly when
    // the Test dock's paths and checklist stop being true.
    if (m_testRun) m_testRun->refresh();
    if (!m_explorer) return;
    m_explorer->setModRoot(m_doc->project().modRoot);
}

QString MainWindow::scriptsFolder() const
{
    const Project &p = m_doc->project();
    if (p.modRoot.isEmpty()) return {};
    const QDir root(p.modRoot);
    const QString prefix = p.modPrefix.isEmpty() ? prefixOfModFolder(p.modRoot)
                                                 : p.modPrefix;
    return QDir::cleanPath(root.filePath(prefix + QStringLiteral("/Scripts")));
}

QString MainWindow::modConfigPath() const
{
    const QString scripts = scriptsFolder();
    if (scripts.isEmpty()) return {};
    const QString path = QDir(scripts).filePath(QStringLiteral("config.cpp"));
    return QFileInfo(path).isFile() ? QDir::cleanPath(path) : QString();
}

void MainWindow::onGraphChanged()
{
    m_scene->refreshVisuals();
    runAnalysis();
}

void MainWindow::runAnalysis()
{
    const Graph *g = m_doc->activeGraph();
    if (!g) return;

    // What else is in reach besides the vanilla catalogue. Without it every
    // class the project declares itself reads as a name that does not exist,
    // which is the wrong answer on any project holding more than one script and
    // is the state a template lands in on its first frame.
    const Project &p = m_doc->project();
    DependencyContext depCtx;
    depCtx.deps = p.dependencies;
    for (const ScriptEntry &s : p.scripts)
        if (!s.graph.className.isEmpty())
            depCtx.knownClasses.append(s.graph.className);
    // Nothing here indexes a dependency's script tree yet, so a project that
    // declares one has a part of its chain this tool has not read. Saying so is
    // what stops "CF_ModuleWorld does not exist" being reported to somebody
    // whose only mistake was not owning a copy of CF.
    depCtx.unindexedDependency = !p.dependencies.isEmpty();

    m_analysis = analyzeGraph(*g, m_doc->catalog(), m_doc->builtins(), p.activeId,
                              depCtx);
    m_scene->setAnalysis(m_analysis);
    updateStatusCounts();
}

void MainWindow::updateStatusCounts()
{
    m_status->setText(QStringLiteral("  ●  %1 Errors      ▲  %2 Warnings")
                          .arg(m_analysis.errors).arg(m_analysis.warnings));
    setSeverity(m_status, m_analysis.errors > 0      ? "error"
                          : m_analysis.warnings > 0  ? "warning"
                                                     : "");
}

void MainWindow::flashStatus(const QString &text)
{
    m_message->setText(QStringLiteral("  ") + text);
    // The status bar clips rather than elides, so the whole line is always
    // reachable. A caller with detail behind the line overwrites this.
    m_message->setToolTip(text);
    setSeverity(m_message, "note");
    m_statusResetTimer->start(4000);
}

void MainWindow::onPaletteNodeRequested(const QString &key)
{
    // Every surface that adds a node arrives here, so this is the one place the
    // read only question has to be asked on the way in.
    if (refuseReadOnlyEdit()) {
        m_hasPendingAdd = false;
        return;
    }
    // A right-click on the canvas says where the next node goes, whichever
    // surface it is finally picked from. Consumed here, so it can never apply
    // to a second node.
    const QPointF at = m_hasPendingAdd
                           ? m_pendingAddPos
                           : m_view->mapToScene(m_view->viewport()->rect().center());
    m_hasPendingAdd = false;
    m_scene->addNodeAt(key, at);
}

void MainWindow::onContextAdd(const QPointF &scenePos)
{
    showAddNodeSearch(scenePos);
}

void MainWindow::showAddNodeSearch(const QPointF &scenePos)
{
    // Ahead of the popup rather than after it: a search that offers a list and
    // then turns down whatever is picked wastes the gesture it invited.
    if (refuseReadOnlyEdit()) return;
    m_pendingAddPos = scenePos;
    m_hasPendingAdd = true;

    auto *popup = new AddNodePopup(m_doc, [this, scenePos](const QString &key) {
        if (key == kAddEventKey) showEventSearch(scenePos);
        else onPaletteNodeRequested(key);
    }, this);
    popup->popupAt(m_view->viewport()->mapToGlobal(m_view->mapFromScene(scenePos)));
}

void MainWindow::showEventSearch(const QPointF &scenePos)
{
    if (refuseReadOnlyEdit()) return;
    m_pendingAddPos = scenePos;
    m_hasPendingAdd = true;
    const QPoint at = m_view->viewport()->mapToGlobal(m_view->mapFromScene(scenePos));

    // Reached from a row in the popup that is closing as this runs. A Qt::Popup
    // holds the mouse grab, so the second one goes up once the first is gone.
    QTimer::singleShot(0, this, [this, at] {
        auto *popup = new EventPopup(m_doc, this);
        connect(popup, &EventPopup::eventPicked, this, &MainWindow::placeEventNode);
        connect(popup, &EventPopup::customEventPicked, this, &MainWindow::addCustomEvent);
        popup->popupAt(at);
    });
}

void MainWindow::placeEventNode(const QString &key)
{
    const Graph *g = m_doc->activeGraph();
    if (g) {
        for (const GraphNode &n : g->nodes) {
            if (n.ref != key) continue;
            // Overriding one method twice does not compile, and someone asking
            // for an event the graph already has meant the one they have.
            m_hasPendingAdd = false;
            revealNode(m_doc, m_view, m_inspector, n.id);
            flashStatus(QStringLiteral("%1 is already on this graph.")
                            .arg(m_doc->defForKey(key).title));
            return;
        }
    }
    onPaletteNodeRequested(key);
}

void MainWindow::addCustomEvent()
{
    if (!m_doc->activeGraph()) {
        flashStatus(QStringLiteral("Open a script before adding events."));
        return;
    }
    if (refuseReadOnlyEdit()) return;

    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New custom event"),
                              QStringLiteral("Name of the method this script declares:"),
                              QLineEdit::Normal, QStringLiteral("MyEvent"), &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) return;

    // A name that is not an identifier reaches the generator as a fragment and
    // takes the whole file down with it, so it is refused here where there is
    // somewhere to say why.
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    if (!shape.match(name).hasMatch()) {
        flashStatus(QStringLiteral("%1 is not a method name. Letters, digits and "
                                   "underscores, not starting with a digit.").arg(name));
        return;
    }
    for (const GraphFunction &f : m_doc->activeGraph()->functions) {
        if (f.name.compare(name, Qt::CaseInsensitive) != 0) continue;
        flashStatus(QStringLiteral("This script already declares %1().").arg(f.name));
        return;
    }

    GraphFunction fn;
    fn.name = name;
    fn.returns = QStringLiteral("void");

    // The declaration and the node that gives it a body are one gesture.
    // beginEdit is depth counted, so the add nested inside this one does not
    // push a second undo entry.
    m_doc->beginEdit(QStringLiteral("Add custom event %1").arg(name));
    Graph *g = m_doc->activeGraph();
    if (!g) {
        m_doc->commitEdit();
        return;
    }
    fn.id = uniqueId(*g, QStringLiteral("f"));
    g->functions.append(fn);
    const QString owner = g->className;
    onPaletteNodeRequested(QStringLiteral("fn.entry.%1").arg(fn.id));
    m_doc->commitEdit();

    flashStatus(QStringLiteral("Declared void %1() on %2. Call it from a Call node, "
                               "or from your own code.").arg(name, owner));
}

void MainWindow::showConnectSearch(const PinRef &from, const QPointF &scenePos)
{
    if (refuseReadOnlyEdit()) return;
    const PinType type = m_scene->typeOfPin(from);
    const QPoint at = m_view->viewport()->mapToGlobal(m_view->mapFromScene(scenePos));

    // Opened from the mouse release that ended the drag. A Qt::Popup takes the
    // mouse grab, so it goes up once that gesture is over rather than inside it.
    QTimer::singleShot(0, this, [this, from, scenePos, type, at] {
        auto *popup = new AddNodePopup(m_doc, [this, from, scenePos](const QString &key) {
            if (key == kPromoteKey) {
                // The derived name is a starting point, not an answer, so hand
                // the caret straight to it the way creating one does.
                const QString variableId = m_scene->promoteToVariable(from, scenePos);
                if (!variableId.isEmpty() && m_variables) m_variables->beginRename(variableId);
            }
            else m_scene->addNodeConnectedTo(key, scenePos, from);
        }, this);
        popup->setCompatibleWith(type, from.dir);
        popup->popupAt(at);
    });
}

void MainWindow::showTabList()
{
    const Project &p = m_doc->project();
    if (p.scripts.isEmpty()) return;

    auto *popup = new ScriptListPopup(p, [this](const QString &id) {
        m_doc->setActiveScript(id);
        m_view->zoomToFit();
    }, this);
    // Right edge under the right edge of the button that opens it. The button
    // sits at the far right of the tab row, and hanging the popup off its left
    // corner puts most of it past the window.
    const QPoint under =
        m_tabList->mapToGlobal(QPoint(m_tabList->width(), m_tabList->height()));
    popup->popupAt(under - QPoint(popup->width(), 0));
}

void MainWindow::showTabMenu(const QPoint &pos)
{
    const int index = m_tabs->tabAt(pos);
    // Off the tabs themselves there is no tab this is about, so the gesture
    // keeps the answer it gave before there was anything to close: the list of
    // every script in the project.
    if (index < 0) {
        showTabList();
        return;
    }
    if (QMenu *menu = buildTabMenu(index))
        menu->popup(m_tabs->mapToGlobal(pos));
}

QMenu *MainWindow::buildTabMenu(int index)
{
    if (index < 0 || index >= m_tabs->count()) return nullptr;

    const Project &p = m_doc->project();
    const QString id = m_tabs->tabData(index).toString();
    // The tab index and the script index agree because refreshTabs walks the
    // project in order, but the id is the thing the rest of the app is keyed on
    // and looking it up is what keeps a stale bar from closing the wrong script.
    int at = -1;
    for (int i = 0; i < p.scripts.size(); ++i) {
        if (p.scripts.at(i).id != id) continue;
        at = i;
        break;
    }
    if (at < 0) return nullptr;
    const QString name = p.scripts.at(at).name;

    QStringList others;
    QStringList toRight;
    QStringList browsed;
    for (int i = 0; i < p.scripts.size(); ++i) {
        const ScriptEntry &s = p.scripts.at(i);
        if (i != at) others << s.id;
        if (i > at) toRight << s.id;
        if (closeCostOf(s) == CloseCost::Browsed) browsed << s.id;
    }

    // Shown with popup() rather than exec(), so it deletes itself once it has
    // been dismissed instead of holding an event loop open while a close prompt
    // wants one of its own.
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose, true);
    menu->setToolTipsVisible(true);

    QAction *one = menu->addAction(QStringLiteral("Close %1").arg(name));
    one->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(one, &QAction::triggered, this, [this, id]() { closeScripts({id}); });

    QAction *rest = menu->addAction(
        QStringLiteral("Close other tabs (%1)").arg(others.size()));
    rest->setEnabled(!others.isEmpty());
    connect(rest, &QAction::triggered, this, [this, others]() { closeScripts(others); });

    QAction *right = menu->addAction(
        QStringLiteral("Close tabs to the right (%1)").arg(toRight.size()));
    right->setEnabled(!toRight.isEmpty());
    connect(right, &QAction::triggered, this, [this, toRight]() { closeScripts(toRight); });

    // The entry the Mod Browser makes necessary: reading four classes out of
    // somebody else's mod leaves four tabs that carry none of the user's work,
    // and taking them one at a time is the wrong amount of effort for that.
    QAction *reads = menu->addAction(
        QStringLiteral("Close browsed scripts (%1)").arg(browsed.size()));
    reads->setEnabled(!browsed.isEmpty());
    reads->setToolTip(QStringLiteral("Scripts read out of other mods. Nothing in "
                                     "them is yours, and the Mod Browser opens "
                                     "them again."));
    connect(reads, &QAction::triggered, this, [this, browsed]() { closeScripts(browsed); });

    menu->addSeparator();
    // The File menu's own action, so the label, the shortcut and the enabled
    // state are written once and cannot disagree between the two menus.
    if (m_reopenAction) menu->addAction(m_reopenAction);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Go to script..."), this, &MainWindow::showTabList);
    return menu;
}

void MainWindow::closeScripts(const QStringList &ids)
{
    const Project &p = m_doc->project();
    QStringList live;
    QStringList onlyCopies;
    QString firstName;
    int owned = 0;
    for (const QString &id : ids) {
        const ScriptEntry *s = p.script(id);
        // A menu built before an import or another close can name a script that
        // is no longer there. Skipping is the only safe reading of that: closing
        // whatever now sits at that position would close something else.
        if (!s) continue;
        if (live.isEmpty()) firstName = s->name;
        live << id;
        const CloseCost cost = closeCostOf(*s);
        if (cost == CloseCost::Browsed) continue;
        owned++;
        if (cost == CloseCost::OnlyCopy) onlyCopies << s->name;
    }
    if (live.isEmpty()) return;

    // Asked when the graph is the only copy of itself, and when one gesture
    // takes more than one of the user's own scripts. A single close of a script
    // whose .c is still on disk is neither, and gets the status line and the
    // reopen entry rather than a box in the way.
    if (!onlyCopies.isEmpty() || owned > 1) {
        const QString what = live.size() == 1
                                 ? QStringLiteral("Close %1?").arg(firstName)
                                 : QStringLiteral("Close %1 scripts?").arg(live.size());
        QStringList why;
        if (onlyCopies.size() == 1) {
            why << QStringLiteral("This project is the only place %1 exists.")
                       .arg(onlyCopies.first());
        } else if (onlyCopies.size() > 1) {
            why << QStringLiteral("This project is the only place these exist: %1.")
                       .arg(onlyCopies.join(QStringLiteral(", ")));
        }
        why << (live.size() == 1
                    ? QStringLiteral("Closing takes it out of the project. No .c on "
                                     "disk is touched, and Reopen closed script puts "
                                     "it back until this session ends.")
                    : QStringLiteral("Closing takes them out of the project. No .c on "
                                     "disk is touched, and Reopen closed script puts "
                                     "them back one at a time until this session "
                                     "ends."));

        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Close scripts"));
        box.setText(what);
        box.setInformativeText(why.join(QStringLiteral("\n\n")));
        QPushButton *go = box.addButton(QStringLiteral("Close"),
                                        QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != go) return;
    }

    int closed = 0;
    QString last;
    for (const QString &id : live) {
        // Re-read per step: each close rewrites the project, and the name is
        // wanted after the entry it belongs to has gone.
        const ScriptEntry *s = p.script(id);
        const QString name = s ? s->name : QString();
        if (!m_doc->closeScript(id)) continue;
        last = name;
        closed++;
    }

    if (closed == 0) {
        flashStatus(QStringLiteral("The project keeps at least one script, and this "
                                   "is the last one."));
        return;
    }
    flashStatus(closed == 1
                    ? QStringLiteral("Closed %1. Ctrl+Shift+T brings it back.").arg(last)
                    : QStringLiteral("Closed %1 scripts. Ctrl+Shift+T brings them "
                                     "back, newest first.").arg(closed));
}

void MainWindow::closeActiveScript()
{
    // The start page is in front of the editor, not beside it, so a shortcut
    // pressed there would change a project nobody can see.
    if (m_stack->currentWidget() != m_editor) return;
    const int index = m_tabs->currentIndex();
    const QString id = index >= 0 ? m_tabs->tabData(index).toString() : QString();
    if (id.isEmpty()) return;
    closeScripts({id});
}

void MainWindow::reopenClosedScript()
{
    // Read before the pop, because after it the stack names the one below.
    const QString name = m_doc->lastClosedName();
    if (!m_doc->reopenClosedScript()) {
        flashStatus(QStringLiteral("Nothing has been closed this session."));
        return;
    }
    showEditor();
    m_view->zoomToFit();
    flashStatus(QStringLiteral("Reopened %1.").arg(name));
}

void MainWindow::updateReopenAction()
{
    if (!m_reopenAction) return;
    const QString name = m_doc->lastClosedName();
    m_reopenAction->setEnabled(m_doc->canReopenScript());
    m_reopenAction->setText(name.isEmpty()
                                ? QStringLiteral("Reopen closed script")
                                : QStringLiteral("Reopen %1").arg(name));
}

void MainWindow::newProject()
{
    if (!maybeSaveChanges(QStringLiteral("Starting a new project"))) return;
    m_doc->resetToNew();
    showEditor();
    m_view->zoomToFit();
}

void MainWindow::startFromTemplate(const StartTemplate &tpl)
{
    if (tpl.kind == StartTemplateKind::Project) {
        if (!tpl.available) {
            QMessageBox::warning(
                this, QStringLiteral("Templates"),
                QStringLiteral("%1 is not installed. It ships in resources/ beside "
                               "the executable.")
                    .arg(QFileInfo(tpl.projectPath).fileName()));
            return;
        }
        openProjectPath(tpl.projectPath);
        return;
    }

    if (tpl.kind == StartTemplateKind::Files) {
        startFromTemplateFiles(tpl);
        return;
    }

    if (!maybeSaveChanges(QStringLiteral("Starting from a template"))) return;

    // resetToNew first, so the import reads the project it is landing in rather
    // than the one being replaced.
    m_doc->resetToNew();
    Project &p = m_doc->project();
    const QString text = scriptSkeleton(tpl.script, &m_doc->catalog());
    const ImportResult result =
        importEnforceText(text, m_doc->catalog(), m_doc->builtins(), p);
    if (!result.ok || result.scripts.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Templates"),
                             QStringLiteral("The %1 template could not be read as a "
                                            "graph.\n\n%2")
                                 .arg(tpl.title, result.error));
        return;
    }

    // The new project's placeholder script would otherwise sit beside the
    // template as an empty second tab.
    p.scripts.clear();
    p.activeId.clear();
    const QString firstId = appendImportedScripts(result, QString(), tpl.module);
    if (const ScriptEntry *first = p.script(firstId))
        p.name = first->name;
    if (!firstId.isEmpty()) m_doc->setActiveScript(firstId);
    // Never saved anywhere, so the project is modified from the first frame and
    // Save has to ask where it goes.
    m_doc->touchGraph();
    refreshTabs();
    showEditor();
    m_view->zoomToFit();
    flashStatus(QStringLiteral("Started from %1. Save the project to give it a "
                               "file.").arg(tpl.title));
}

void MainWindow::startFromTemplateFiles(const StartTemplate &tpl)
{
    if (!tpl.available || tpl.files.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Templates"),
            QStringLiteral("The %1 template is not installed. Its scripts ship in "
                           "resources/templates/%2 beside the executable.")
                .arg(tpl.title, tpl.id));
        return;
    }
    if (!maybeSaveChanges(QStringLiteral("Starting from a template"))) return;

    // resetToNew first, so every import below reads the project it is landing
    // in rather than the one being replaced.
    m_doc->resetToNew();
    Project &p = m_doc->project();
    p.scripts.clear();
    p.activeId.clear();
    if (!tpl.projectName.isEmpty()) p.name = tpl.projectName;

    // Declared before the first import, so a call into a dependency resolves
    // against the chain rather than against nothing. A dependency with no
    // scriptRoot is a normal state: the facts are still worth carrying, the
    // badges still draw, and the user points it at their own copy when they
    // have one.
    for (const QString &id : tpl.dependencies) {
        ModDependency dep = knownDependency(id);
        if (dep.id.isEmpty()) {
            dep.id = id;
            dep.displayName = id;
            dep.shortName = shortNameFor(id);
        }
        p.dependencies.append(dep);
    }
    for (const QString &id : tpl.optionalDependencies) {
        ModDependency dep = knownDependency(id);
        if (dep.id.isEmpty()) {
            dep.id = id;
            dep.displayName = id;
            dep.shortName = shortNameFor(id);
        }
        // What `optional` and `loadedDefine` were added for: the code behind
        // the #ifdef compiles either way, and DZ315 has something to check.
        dep.optional = true;
        p.dependencies.append(dep);
    }

    QString firstId;
    QStringList failed;
    int classes = 0;
    int keptAsText = 0;
    QStringList notes;
    for (const QString &file : tpl.files) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            failed << QFileInfo(file).fileName();
            continue;
        }
        const QString text = QString::fromUtf8(in.readAll());
        in.close();

        const ImportResult result =
            importEnforceText(text, m_doc->catalog(), m_doc->builtins(), p);
        if (!result.ok || result.scripts.isEmpty()) {
            failed << QFileInfo(file).fileName();
            continue;
        }

        // An empty sourcePath on purpose. Saving must ask where the project
        // goes, and exporting must ask for a mod folder, or the first Ctrl+S
        // would write over the copy of the template that ships with the app.
        const QString module =
            QFileInfo(QFileInfo(file).absolutePath()).fileName();
        const QString id = appendImportedScripts(result, QString(), module);
        if (firstId.isEmpty()) firstId = id;
        classes += result.scripts.size();
        for (const ImportedScript &imported : result.scripts)
            keptAsText += textKeptIn(imported.graph);
        notes += result.notes;
    }

    if (firstId.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Templates"),
                             QStringLiteral("None of the %1 template's scripts could "
                                            "be read as a graph.")
                                 .arg(tpl.title));
        m_doc->resetToNew();
        refreshTabs();
        return;
    }

    // Land on the graph with the most nodes in it, not on the first file in
    // load order. A method the importer kept as text leaves no node behind, so
    // a script whose every method is text opens on an empty canvas reading "no
    // nodes yet" over a class that is entirely present in the generated code.
    // Landing there is the worst first frame a working template could have, and
    // which file it happens to be is an accident of alphabetical order.
    QString landOn = firstId;
    int best = -1;
    for (const ScriptEntry &s : p.scripts) {
        if (s.graph.nodes.size() <= best) continue;
        best = s.graph.nodes.size();
        landOn = s.id;
    }
    m_doc->setActiveScript(landOn);
    // Never saved anywhere, so the project is modified from the first frame and
    // Save has to ask where it goes.
    m_doc->touchGraph();
    refreshTabs();
    showEditor();
    m_view->zoomToFit();

    QString report = QStringLiteral("Started from %1: %2 script%3, %4 class%5")
                         .arg(tpl.title)
                         .arg(p.scripts.size())
                         .arg(p.scripts.size() == 1 ? QString() : QStringLiteral("s"))
                         .arg(classes)
                         .arg(classes == 1 ? QString() : QStringLiteral("es"));
    // Said out loud rather than left for somebody to find. These templates are
    // written to regenerate byte for byte, and the pieces the graph could not
    // model are the pieces that will be written back as the text they came in
    // as. Reading that as a failure is the wrong reading, so it is named.
    if (keptAsText > 0)
        report += QStringLiteral(", %1 kept as text").arg(keptAsText);
    report += QStringLiteral(". Save the project to give it a file.");

    // The addons this one needs beyond the ones every DayZ mod already has.
    // Every template requires DZ_Scripts, which the bundled config.cpp already
    // declares, so naming that alone would be noise; anything past it is a line
    // somebody has to type or the mod does not load.
    QStringList extraAddons;
    for (const QString &addon : tpl.requiredAddons)
        if (addon != QLatin1String("DZ_Scripts")) extraAddons << addon;

    const QString addonLine =
        tpl.requiredAddons.isEmpty()
            ? QString()
            : QStringLiteral("config.cpp needs requiredAddons[] = { \"%1\" };")
                  .arg(tpl.requiredAddons.join(QStringLiteral("\", \"")));

    if (extraAddons.isEmpty()) {
        flashStatus(report);
    } else {
        // Said in the line rather than only in the tooltip on it, and left up
        // rather than cleared after four seconds. Leaving an addon out of
        // requiredAddons does not fail here and does not fail at compile: the
        // mod builds, ships, and the server refuses to compile the script at
        // boot naming a class the author never typed. One line now is the whole
        // difference, so it stays on screen until something replaces it.
        m_message->setText(QStringLiteral("  ") + report + QLatin1Char(' ')
                           + addonLine);
        setSeverity(m_message, "note");
        m_statusResetTimer->stop();
    }

    QStringList tip;
    if (!addonLine.isEmpty()) tip << addonLine;
    if (!failed.isEmpty())
        tip << QStringLiteral("These files could not be read: %1")
                   .arg(failed.join(QStringLiteral(", ")));
    tip += notes;
    m_message->setToolTip(tip.isEmpty() ? report
                                        : report + QStringLiteral("\n\n")
                                              + tip.join(QLatin1Char('\n')));
}

void MainWindow::newMod()
{
    if (!modTemplateAvailable()) {
        QMessageBox::warning(this, QStringLiteral("New mod"),
                             QStringLiteral("The mod template is missing. It ships in "
                                            "resources/mod-template beside the "
                                            "executable."));
        return;
    }
    // The new mod takes the session over: the editor ends up on a project
    // saved inside the folder that is about to be created, so anything unsaved
    // here would be left behind with nowhere to go back to.
    if (!maybeSaveChanges(QStringLiteral("Starting a new mod"))) return;

    const ModTemplateResult result = NewModDialog::run(this);
    if (!result.ok) {
        // An empty error is a cancelled dialog, which needs no report.
        if (!result.error.isEmpty())
            QMessageBox::warning(this, QStringLiteral("New mod"), result.error);
        return;
    }

    const QString prefix = prefixFromScaffold(result);
    m_doc->resetToNew();
    Project &p = m_doc->project();
    p.name = prefix;
    p.modRoot = QDir::cleanPath(result.modRoot);
    p.modPrefix = prefix;
    // The graph starts named after the mod rather than as MyItem: the first
    // export then writes a file whose name matches the folder it lands in.
    if (ScriptEntry *first = p.active()) {
        first->name = prefix;
        first->graph.className = prefix;
    }

    const QString projectPath =
        QDir(p.modRoot).filePath(prefix + QStringLiteral(".sdzn"));
    QString error;
    if (!m_doc->saveProject(projectPath, &error)) {
        QMessageBox::warning(this, QStringLiteral("New mod"), error);
        return;
    }

    afterSave(projectPath);
    refreshTabs();
    m_scene->rebuild();
    runAnalysis();
    syncExplorerRoot();
    showEditor();
    m_view->zoomToFit();

    // The dialog has already reported the folder it wrote, so this names the
    // one thing it could not know about: the project file Save now writes to.
    flashStatus(QStringLiteral("%1 created. Project saved as %2")
                    .arg(prefix, QDir::toNativeSeparators(projectPath)));
}

void MainWindow::setModFolder()
{
    Project &p = m_doc->project();
    QString start = p.modRoot;
    if (start.isEmpty() && !m_doc->projectPath().isEmpty())
        start = QFileInfo(m_doc->projectPath()).absolutePath();

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Set mod folder"), start);
    if (dir.isEmpty()) return;

    p.modRoot = QDir::cleanPath(dir);
    p.modPrefix = prefixOfModFolder(p.modRoot);
    // The mod folder is part of the project and belongs in the .sdzn, so the
    // document has to know it is behind the file on disk again.
    m_doc->touchGraph();
    syncExplorerRoot();
    flashStatus(QStringLiteral("Mod folder set to %1")
                    .arg(QDir::toNativeSeparators(p.modRoot)));
}

void MainWindow::updateModLibraryLine()
{
    if (!m_startPage || !m_modBrowser) return;
    const QVector<ModEntry> mods = m_modBrowser->library()->mods();
    if (mods.isEmpty()) {
        m_startPage->setModLibraryLine(
            QStringLiteral("A .pbo or a mod folder from disk, as graphs to read."));
        return;
    }
    int withScripts = 0;
    for (const ModEntry &mod : mods)
        if (mod.hasScripts()) ++withScripts;
    // One line, and the count is what makes it an invitation rather than a
    // label: the card sits next to three that start something, and this is the
    // one that says there are 216 mods here to read.
    // "on this machine" rather than "installed": the count includes anything
    // opened by path, which is on the machine but was never installed.
    m_startPage->setModLibraryLine(
        QStringLiteral("%1 mods on this machine, %2 with script. Read any of them, or a "
                       ".pbo from disk, as graphs.")
            .arg(mods.size())
            .arg(withScripts));
}

void MainWindow::browseInstalledMods()
{
    if (!m_modBrowser) return;
    // The docks are put away while the start page is up, so showing one there
    // would put a panel on a page that has none.
    showEditor();

    const auto reveal = [this]() {
        QDockWidget *dock = dockOf(m_modBrowser);
        if (!dock) return;
        // Closed from the View menu, so its toggle is off and show() alone
        // would leave the two out of step.
        dock->toggleViewAction()->setChecked(true);
        dock->show();
        dock->raise();
        raiseDockTab(this, dock);
        m_modBrowser->takeFocus();
    };
    reveal();
    // Twice, and the second time is the one that lands. Coming back from the
    // start page re-shows every dock at once, and the tab bar is only built
    // once the layout pass after that has run, so the first attempt has no tab
    // to put in front yet.
    QTimer::singleShot(0, this, reveal);

    flashStatus(QStringLiteral("Mod Browser. Pick a mod to read its classes, or "
                               "Open... for a pbo anywhere on disk."));
}

void MainWindow::openModFromDisk()
{
    if (!m_modBrowser) return;
    browseInstalledMods();
    m_modBrowser->openFromDisk();
}

void MainWindow::openBrowsedGraph(const QString &name, const Graph &graph)
{
    Project &p = m_doc->project();
    const QString origin = graphOrigin(graph);

    // The same class twice is the tab the user already has. Keyed on the origin
    // rather than the class name: two mods that both mod PlayerBase are two
    // different pieces of code with one name.
    // The browser is reachable from the start page, and a class opened there
    // used to land on a canvas nobody was looking at: the status line said it
    // had opened and the window still showed the start page.
    showEditor();

    if (!origin.isEmpty()) {
        for (const ScriptEntry &s : p.scripts) {
            if (graphOrigin(s.graph) != origin) continue;
            m_doc->setActiveScript(s.id);
            m_view->zoomToFit();
            flashStatus(QStringLiteral("%1 is already open.").arg(s.name));
            return;
        }
    }

    ScriptEntry entry;
    // The counter behind nextId restarts every launch and knows nothing about
    // the ids already in this project.
    do {
        entry.id = nextId(QStringLiteral("s"));
    } while (p.script(entry.id));
    entry.graph = graph;
    entry.name = entry.graph.className.isEmpty() ? name : entry.graph.className;
    if (entry.name.isEmpty()) entry.name = QStringLiteral("Untitled");
    entry.folder = entry.graph.module.isEmpty() ? QStringLiteral("4_World")
                                                : entry.graph.module;
    // sourcePath stays empty, and that is the point. It names the file a script
    // is written back to, and this one is written back nowhere.
    //
    // The panel marks what it hands over and this marks it again, because every
    // write in this file asks the mark rather than asking where the graph came
    // from. An unmarked graph reaching here would be exported.
    if (!graphIsReadOnly(entry.graph))
        markGraphReadOnly(entry.graph, name, QString(), QString());

    p.scripts.append(entry);
    m_doc->setActiveScript(entry.id);
    // The project holds a script the .sdzn does not, so it is behind again. A
    // save keeps the graph, and the mark rides the file, so it comes back read
    // only rather than as a script of the user's own.
    m_doc->touchGraph();
    refreshTabs();

    flashStatus(QStringLiteral("Opened %1, read only. Export leaves it out, and "
                               "nothing here writes it into your mod.")
                    .arg(entry.name));
    if (!origin.isEmpty()) m_message->setToolTip(origin);
}

void MainWindow::openModFile(const QString &path)
{
    QString error;
    if (!FileDialog::openFile(this, m_doc, path, &error))
        QMessageBox::warning(this, QStringLiteral("Open file"), error);
}

void MainWindow::openModConfig(const QString &path)
{
    QString error;
    if (ConfigEditor::openFile(this, m_doc, path, &error)) return;

    // A config that cannot be read as a tree is the one the user most needs in
    // front of them, so the text editor takes over rather than the app stopping
    // at a warning.
    QMessageBox::warning(this, QStringLiteral("Open config"),
                         QStringLiteral("%1 could not be read as a class tree.\n\n%2\n\n"
                                        "Opening it as text instead.")
                             .arg(QFileInfo(path).fileName(), error));
    openModFile(path);
}

void MainWindow::editModConfig()
{
    const QString path = modConfigPath();
    if (!path.isEmpty()) {
        openModConfig(path);
        return;
    }

    const Project &p = m_doc->project();
    if (p.modRoot.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Edit mod config"),
            QStringLiteral("This project has no mod folder yet. Use File > New mod "
                           "to scaffold one, or Set mod folder to point at one you "
                           "already have."));
        return;
    }
    // The folder is there and the config is not, which is worth saying with the
    // path in it: it is nearly always a prefix that does not match the folder.
    QMessageBox::information(
        this, QStringLiteral("Edit mod config"),
        QStringLiteral("There is no config.cpp under\n\n%1")
            .arg(QDir::toNativeSeparators(scriptsFolder())));
}

void MainWindow::openModScript(const QString &path)
{
    const QString file = QFileInfo(path).fileName();
    Project &p = m_doc->project();

    // Importing one file twice leaves two graphs over the same class, and
    // whichever is written last silently wins. The tab the user already has is
    // the one they mean.
    for (const ScriptEntry &s : p.scripts) {
        if (!sameScriptFile(s.sourcePath, path)) continue;
        const QString id = s.id;
        m_doc->setActiveScript(id);
        m_view->zoomToFit();
        flashStatus(QStringLiteral("%1 is already open.").arg(file));
        return;
    }

    const ImportResult result =
        importEnforceFile(path, m_doc->catalog(), m_doc->builtins(), p);
    if (!result.ok || result.scripts.isEmpty()) {
        const QString why = result.error.isEmpty()
                                ? QStringLiteral("It holds no class this editor can draw.")
                                : result.error;
        QMessageBox::warning(this, QStringLiteral("Open script"),
                             QStringLiteral("%1 could not be opened as a graph.\n\n%2\n\n"
                                            "Opening it as text instead.")
                                 .arg(file, why));
        openModFile(path);
        return;
    }

    const QString source = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString firstId =
        appendImportedScripts(result, source, moduleForPath(path));

    // Selecting the script is what rebuilds the canvas, refreshes the tabs and
    // frames the graph; the project holding scripts the .sdzn does not is what
    // makes it modified.
    if (!firstId.isEmpty()) m_doc->setActiveScript(firstId);
    m_doc->touchGraph();
    refreshTabs();

    const int lowered = result.totalLowered();
    const int total = result.totalStatements();
    int keptAsText = 0;
    for (const ImportedScript &imported : result.scripts)
        keptAsText += textKeptIn(imported.graph);

    QString report = QStringLiteral("Opened %1: %2 %3")
                         .arg(file)
                         .arg(result.scripts.size())
                         .arg(result.scripts.size() == 1 ? QStringLiteral("class")
                                                         : QStringLiteral("classes"));
    // The graph is the honest witness, not the statement counter. A `super`
    // call gets absorbed by the event node that carries it, so a file modelled
    // completely can still report zero statements lowered, and "0 of 1" reads
    // like a failure when nothing failed.
    if (keptAsText == 0) {
        report += QStringLiteral(", fully modelled");
    } else {
        if (total > 0)
            report += QStringLiteral(", %1 of %2 statements as nodes").arg(lowered).arg(total);
        // Said out loud, because generating this script writes the file back
        // from the graph, and those are the pieces it will write back as the
        // text they came in as.
        report += QStringLiteral(". %1 kept as text").arg(keptAsText);
    }
    report += QLatin1Char('.');

    flashStatus(report);
    if (!result.notes.isEmpty())
        m_message->setToolTip(report + QStringLiteral("\n\n")
                              + result.notes.join(QLatin1Char('\n')));
}

QString MainWindow::appendImportedScripts(const ImportResult &result,
                                          const QString &sourcePath,
                                          const QString &module)
{
    Project &p = m_doc->project();
    QString firstId;
    for (int i = 0; i < result.scripts.size(); ++i) {
        const ImportedScript &imported = result.scripts.at(i);
        ScriptEntry entry;
        // The counter behind nextId restarts every launch and knows nothing
        // about the ids already in this project.
        do {
            entry.id = nextId(QStringLiteral("s"));
        } while (p.script(entry.id));
        entry.graph = imported.graph;
        // The header fields on the ImportedScript are what the file said. A
        // graph carries defaults from its own constructor, so taking them from
        // the struct is what keeps `modded class X` from regenerating as
        // `class X extends ItemBase`.
        if (!imported.className.isEmpty()) entry.graph.className = imported.className;
        if (!imported.baseClass.isEmpty()) entry.graph.baseClass = imported.baseClass;
        if (imported.modded) entry.graph.modded = true;

        entry.name = entry.graph.className.isEmpty()
                         ? QFileInfo(sourcePath).completeBaseName()
                         : entry.graph.className;
        if (entry.name.isEmpty()) entry.name = QStringLiteral("Untitled");
        entry.folder = module.isEmpty() ? entry.graph.module : module;
        if (entry.folder.isEmpty()) entry.folder = QStringLiteral("4_World");
        entry.graph.module = entry.folder;
        entry.sourcePath = sourcePath;
        // The preamble belongs to the file rather than to any one class in it,
        // so the first script carries it and writing the file back puts it in
        // front of all of them.
        if (i == 0) entry.preamble = result.preamble;

        p.scripts.append(entry);
        if (firstId.isEmpty()) firstId = entry.id;
    }
    return firstId;
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open project"), m_recent.lastFolder(),
        QStringLiteral("SUDO node projects (*.sdzn);;All files (*)"));
    if (!path.isEmpty()) openProjectPath(path);
}

void MainWindow::openProjectPath(const QString &path)
{
    if (!maybeSaveChanges(QStringLiteral("Opening another project"))) return;

    const QString source = recoveryFor(path);
    QString error;
    if (!m_doc->openProject(source, &error)) {
        QMessageBox::warning(this, QStringLiteral("Open project"), error);
        // A project that will not open is still the one the user was looking
        // for, so it keeps its place in the list rather than being forgotten
        // for being broken.
        m_recent.record(path);
        if (m_startPage) m_startPage->refresh();
        return;
    }
    const bool recovered = source != path;
    if (recovered) {
        // The document is pointed back at the real project, which has not been
        // touched. Nothing is written until the user saves.
        m_doc->project().path = path;
        m_doc->touchGraph();
    }
    m_recent.record(path);
    showEditor();
    m_view->zoomToFit();
    flashStatus(recovered
                    ? QStringLiteral("Recovered unsaved work for %1. Save it to keep "
                                     "it.").arg(QFileInfo(path).fileName())
                    : QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()));
}

bool MainWindow::saveProject()
{
    if (m_doc->projectPath().isEmpty()) return saveProjectAs();
    QString error;
    if (!m_doc->saveProject(m_doc->projectPath(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Save project"), error);
        return false;
    }
    afterSave(m_doc->projectPath());
    flashStatus(QStringLiteral("Saved."));
    return true;
}

bool MainWindow::saveProjectAs()
{
    QString start = m_doc->projectPath();
    if (start.isEmpty()) {
        const QString folder = m_recent.lastFolder().isEmpty() ? QDir::homePath()
                                                               : m_recent.lastFolder();
        start = QDir(folder).filePath(m_doc->project().name
                                      + QStringLiteral(".sdzn"));
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save project as"), start,
        QStringLiteral("SUDO node projects (*.sdzn)"));
    if (path.isEmpty()) return false;
    QString error;
    if (!m_doc->saveProject(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("Save project"), error);
        return false;
    }
    afterSave(path);
    flashStatus(QStringLiteral("Saved as %1").arg(QFileInfo(path).fileName()));
    return true;
}

void MainWindow::afterSave(const QString &path)
{
    // The sidecar describes work that is now in the file itself. Leaving it
    // would offer, on the next open, to recover the state just saved past.
    clearAutosave(path);
    m_recent.record(path);
    updateWindowTitle();
}

bool MainWindow::maybeSaveChanges(const QString &action)
{
    if (!m_doc->isModified()) return true;

    const QString name = m_doc->projectPath().isEmpty()
                             ? m_doc->project().name
                             : QFileInfo(m_doc->projectPath()).fileName();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved changes"));
    box.setText(QStringLiteral("%1 has changes that are not saved.").arg(name));
    box.setInformativeText(QStringLiteral("%1 loses them.").arg(action));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard
                           | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:
        // A failed write, or a cancelled Save as, is not permission to carry on.
        return saveProject();
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

QString MainWindow::autosavePathFor(const QString &projectPath) const
{
    if (projectPath.isEmpty()) return {};
    // Beside the project, not in a temp folder: every path in a .sdzn is
    // relative to the file itself, so a sidecar anywhere else would resolve the
    // mod folder and every imported script against the wrong place.
    return projectPath + QStringLiteral(".autosave");
}

void MainWindow::writeAutosave()
{
    const QString project = m_doc->projectPath();
    if (project.isEmpty() || !m_doc->isModified()) return;
    const QString sidecar = autosavePathFor(project);
    QString error;
    // The free function, not Document::saveProject: that one would clear the
    // modified flag and move the project's own path to the sidecar, so the next
    // Save would write the graph over the recovery file.
    if (!::saveProject(m_doc->project(), sidecar, &error)) {
        flashStatus(QStringLiteral("Could not autosave: %1").arg(error));
        return;
    }
    flashStatus(QStringLiteral("Autosaved beside the project."));
}

void MainWindow::clearAutosave(const QString &projectPath)
{
    const QString sidecar = autosavePathFor(projectPath);
    if (!sidecar.isEmpty() && QFileInfo::exists(sidecar)) QFile::remove(sidecar);
}

QString MainWindow::recoveryFor(const QString &projectPath)
{
    const QString sidecar = autosavePathFor(projectPath);
    if (sidecar.isEmpty()) return projectPath;
    const QFileInfo recovery(sidecar);
    if (!recovery.isFile()) return projectPath;

    const QFileInfo saved(projectPath);
    if (saved.isFile() && recovery.lastModified() <= saved.lastModified()) {
        // The project has been saved since the sidecar was written, so it holds
        // nothing the project does not. Removing it is the app tidying up after
        // itself, and it leaves the project untouched.
        QFile::remove(sidecar);
        return projectPath;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Unsaved work found"));
    box.setText(QStringLiteral("%1 has an autosave that is newer than the project.")
                    .arg(saved.fileName()));
    box.setInformativeText(
        QStringLiteral("Autosaved %1, project last saved %2.\n\nRecovering opens the "
                       "autosaved graph as unsaved work. The project file is not "
                       "changed until you save it yourself.")
            .arg(relativeTime(recovery.lastModified()),
                 saved.isFile() ? relativeTime(saved.lastModified())
                                : QStringLiteral("never")));
    QPushButton *recover =
        box.addButton(QStringLiteral("Recover"), QMessageBox::AcceptRole);
    QPushButton *open =
        box.addButton(QStringLiteral("Open the project"), QMessageBox::RejectRole);
    QPushButton *discard = box.addButton(QStringLiteral("Delete the autosave"),
                                         QMessageBox::DestructiveRole);
    box.setDefaultButton(recover);
    box.exec();

    if (box.clickedButton() == discard) {
        QFile::remove(sidecar);
        return projectPath;
    }
    // Keeping the file is the answer to both "open the project" and closing the
    // box: an autosave thrown away by accident cannot be asked for again.
    if (box.clickedButton() == open) return projectPath;
    return box.clickedButton() == recover ? sidecar : projectPath;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSaveChanges(QStringLiteral("Closing"))) {
        event->ignore();
        return;
    }
    // The session ended in a decision rather than a crash, so there is nothing
    // to recover on the next launch.
    clearAutosave(m_doc->projectPath());
    QMainWindow::closeEvent(event);
}

bool MainWindow::writeScriptFile(const QString &path, QString *error)
{
    const Project &p = m_doc->project();
    QVector<const ScriptEntry *> parts;
    int browsed = 0;
    for (const ScriptEntry &s : p.scripts) {
        if (!sameScriptFile(s.sourcePath, path)) continue;
        // The last gate before bytes. Callers check first and this checks
        // again, because it is the only function in the app that turns a graph
        // into a .c, and one gate that cannot be talked past is worth more than
        // four that agree with each other.
        if (!scriptIsWritable(s)) {
            browsed++;
            continue;
        }
        parts.append(&s);
    }
    if (parts.isEmpty()) {
        if (error)
            *error = browsed > 0
                         ? QStringLiteral("it was read out of another mod, so it is "
                                          "not written back to %1")
                               .arg(QDir::toNativeSeparators(path))
                         : QStringLiteral("no script in this project came from %1")
                               .arg(QDir::toNativeSeparators(path));
        return false;
    }

    // The file as it stands, so the user regions inside it survive the rewrite.
    const QString previous = readFileText(path);

    QStringList names;
    for (const ScriptEntry *s : parts) names << s->graph.className;

    QString preamble;
    for (const ScriptEntry *s : parts) {
        if (s->preamble.isEmpty()) continue;
        preamble = s->preamble;
        break;
    }

    QStringList classes;
    for (int i = 0; i < parts.size(); ++i)
        classes << generateEnforce(parts.at(i)->graph, m_doc->catalog(),
                                   m_doc->builtins(), p,
                                   classSection(previous, names.at(i), names))
                       .code;

    // Every class in one file was read out of that file, so they agree on the
    // ending. If they ever do not, bare newlines is the answer that invents
    // least.
    QString eol = parts.first()->graph.eol;
    for (const ScriptEntry *s : parts)
        if (s->graph.eol != eol) eol = QStringLiteral("\n");

    const QString text = assembleScriptFile(classes, preamble, eol);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("could not write %1")
                         .arg(QDir::toNativeSeparators(path));
        return false;
    }
    out.write(text.toUtf8());
    if (!out.commit()) {
        if (error)
            *error = QStringLiteral("could not finish writing %1. It may be open in "
                                    "another program, or read only.")
                         .arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

void MainWindow::saveScriptFile()
{
    Project &p = m_doc->project();
    ScriptEntry *script = p.active();
    if (!script) {
        flashStatus(QStringLiteral("There is no script to write."));
        return;
    }
    if (!scriptIsWritable(*script)) {
        QMessageBox::information(
            this, QStringLiteral("Save script to file"),
            QStringLiteral("%1 was read out of another mod, so this does not write "
                           "it.\n\n%2\n\nThe browser opens somebody else's code to be "
                           "read. Writing it into your own scripts would ship their "
                           "work under your name.")
                .arg(script->name, graphOrigin(script->graph)));
        return;
    }

    QString path = script->sourcePath;
    if (path.isEmpty()) {
        // Never imported from anywhere, so this is the one time it has to ask.
        // The mod's own Scripts folder is the answer often enough to be the
        // place the dialog starts.
        QString start = scriptsFolder();
        if (!start.isEmpty()) start = QDir(start).filePath(script->folder);
        if (start.isEmpty()) start = QDir::homePath();
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save script to file"),
            QDir(start).filePath(script->name + QStringLiteral(".c")),
            QStringLiteral("Enforce Script (*.c);;All files (*)"));
        if (path.isEmpty()) return;
        path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        script->sourcePath = path;
        // The link is part of the project now, so the .sdzn is behind again.
        m_doc->touchGraph();
    }

    QString error;
    if (!writeScriptFile(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("Save script"), error);
        return;
    }
    flashStatus(QStringLiteral("Wrote %1").arg(QDir::toNativeSeparators(path)));
}

void MainWindow::showGeneratedCode()
{
    const Graph *g = m_doc->activeGraph();
    if (!g) return;
    const GenResult result = generateEnforce(*g, m_doc->catalog(), m_doc->builtins(),
                                             m_doc->project());

    CodeDialog::showGenerated(this, m_doc, QStringLiteral("%1.c").arg(g->className),
                              result.code, result.warnings);
}

void MainWindow::exportScripts()
{
    const Project &project = m_doc->project();
    // A script that was imported goes back to the file it came from, wherever
    // that is. Exporting it into the folder as well would leave two copies of
    // one class in the mod, and the compiler would take both.
    //
    // Both counts come from the plan rather than from a second walk of the
    // project, so what the question promises and what the writing does cannot
    // drift apart. With no folder given the plan holds the source-bound files
    // only, which is exactly the number the question needs.
    QVector<const ScriptEntry *> browsed;
    const int intoSource = exportPlan(project, QString(), &browsed).size();
    const int intoFolder = scriptsNeedingFolder(project);

    // A project that came from the template already knows where its scripts go,
    // and picking that folder by hand every time is the step that eventually
    // lands a build in the wrong mod. Named out loud, and still overridable.
    QString dir = intoFolder > 0 ? scriptsFolder() : QString();
    if (!dir.isEmpty()) {
        QString question = QStringLiteral("Write %1 scripts to\n\n%2")
                               .arg(intoFolder)
                               .arg(QDir::toNativeSeparators(dir));
        if (intoSource > 0)
            question += QStringLiteral("\n\n%1 more go back to the files they were "
                                       "opened from.").arg(intoSource);
        if (!browsed.isEmpty())
            question += QStringLiteral("\n\n%1 stays where it is: it was read out of "
                                       "another mod and is not yours to write.")
                            .arg(browsed.size() == 1
                                     ? browsed.first()->name
                                     : QStringLiteral("%1 read only scripts")
                                           .arg(browsed.size()));
        QMessageBox box(QMessageBox::Question, QStringLiteral("Export scripts"),
                        question, QMessageBox::NoButton, this);
        QPushButton *here = box.addButton(QStringLiteral("Export here"),
                                          QMessageBox::AcceptRole);
        QPushButton *elsewhere = box.addButton(QStringLiteral("Choose folder..."),
                                               QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == elsewhere)
            dir.clear();
        else if (box.clickedButton() != here)
            return;
        // The Scripts folder is in the template, but a mod folder set by hand
        // may not have one yet.
        if (!dir.isEmpty()) QDir().mkpath(dir);
    }

    if (dir.isEmpty() && intoFolder > 0) {
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export scripts to mod folder"), scriptsFolder());
        if (dir.isEmpty()) return;
    }

    int written = 0;
    int returned = 0;
    QStringList problems;
    // One entry per file: two scripts sharing a file appear once, and that file
    // is written with both classes in it. Nothing the browser produced is in
    // here at all.
    for (const ExportTarget &target : exportPlan(project, dir)) {
        const ScriptEntry &s = *target.script;
        if (target.intoSource) {
            QString error;
            if (writeScriptFile(target.path, &error)) returned++;
            else problems << QStringLiteral("%1: %2").arg(s.name, error);
            continue;
        }

        QDir().mkpath(QFileInfo(target.path).absolutePath());

        // Regenerating over an existing file keeps its user regions.
        const QString previous = readFileText(target.path);

        const GenResult gen = generateEnforce(s.graph, m_doc->catalog(),
                                              m_doc->builtins(), project, previous);
        QSaveFile out(target.path);
        if (!out.open(QIODevice::WriteOnly) || (out.write(gen.code.toUtf8()), !out.commit())) {
            problems << QStringLiteral("%1: could not write").arg(s.name);
            continue;
        }
        written++;
    }

    if (!problems.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export scripts"),
                             problems.join(QStringLiteral("\n")));
        return;
    }
    if (written == 0 && returned == 0) {
        flashStatus(browsed.isEmpty()
                        ? QStringLiteral("There was nothing to export.")
                        : QStringLiteral("There was nothing of yours to export. "
                                         "%1 read only, out of another mod.")
                              .arg(countOf(browsed.size(), QStringLiteral("script"),
                                           QStringLiteral("scripts"))));
        return;
    }
    QString report;
    if (written > 0)
        report = QStringLiteral("Exported %1 scripts to %2")
                     .arg(written).arg(QDir::toNativeSeparators(dir));
    if (returned > 0) {
        if (!report.isEmpty()) report += QStringLiteral(", and wrote ");
        else report = QStringLiteral("Wrote ");
        report += QStringLiteral("%1 back to the %2 they were opened from")
                      .arg(returned)
                      .arg(returned == 1 ? QStringLiteral("file")
                                         : QStringLiteral("files"));
    }
    report += QLatin1Char('.');
    // Said every time rather than only when it is news: an export that quietly
    // leaves a class out is the export somebody goes looking for at build time.
    if (!browsed.isEmpty())
        report += QStringLiteral(" %1 left alone, read only out of another mod.")
                      .arg(countOf(browsed.size(), QStringLiteral("script"),
                                   QStringLiteral("scripts")));
    flashStatus(report);
}

void MainWindow::updateWindowTitle()
{
    // The start page is not looking at a project, and naming one there would
    // say the editor is showing something it is not.
    if (m_stack && m_stack->currentWidget() == m_startPage) {
        setWindowTitle(QStringLiteral("SUDO DayZ Node Mod"));
        return;
    }
    const QString name = m_doc->projectPath().isEmpty()
                             ? m_doc->project().name
                             : QFileInfo(m_doc->projectPath()).fileName();
    // The asterisk is the contracted mark for unsaved work and it goes in front
    // of the name, where it is read before the eye moves on.
    setWindowTitle(QStringLiteral("%1%2 - SUDO DayZ Node Mod")
                       .arg(m_doc->isModified() ? QStringLiteral("*") : QString(),
                            name));
}
