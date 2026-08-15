#include "mainwindow.h"

#include "analysis.h"
#include "canvas/minimapwidget.h"
#include "canvas/nodescene.h"
#include "canvas/nodeview.h"
#include "codegen.h"
#include "document.h"
#include "enforce/import.h"
#include "modtemplate.h"
#include "panels/codeviewpanel.h"
#include "panels/eventspanel.h"
#include "panels/explorerpanel.h"
#include "panels/inspectorpanel.h"
#include "panels/outlinerpanel.h"
#include "panels/palettepanel.h"
#include "panels/variablespanel.h"
#include "theme.h"
#include "widgets/codedialog.h"
#include "widgets/configeditor.h"
#include "widgets/filedialog.h"
#include "widgets/newmoddialog.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

constexpr int kKeyRole = Qt::UserRole;

// The row that turns the dragged value into a member instead of looking for a
// node to take it. Not a node key, so it can never collide with one.
const QLatin1String kPromoteKey("promote.variable");

// The row that hands the search over to the events list. Also not a node key.
const QLatin1String kAddEventKey("add.event");

// Ranked catalogue hits to test against the dragged pin, and how many survivors
// are worth listing. The catalogue holds 29k entries; testing them all would
// cost a def build each, so the menu walks the ranking and stops early.
constexpr int kFitScan = 400;
constexpr int kFitKeep = 40;

// Brings the dock a panel lives in to the front when it is stacked behind
// another. Docks are split by default, where raise does nothing, but the layout
// is the user's to rearrange and a tabbed inspector shows nothing at all.
void revealDock(QWidget *panel)
{
    auto *dock = panel ? qobject_cast<QDockWidget *>(panel->parentWidget()) : nullptr;
    // A dock closed from the View menu stays closed. Raising it would reopen it
    // and take the space back every time a node was selected.
    if (!dock || !dock->toggleViewAction()->isChecked()) return;
    dock->raise();
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

// One key per file on disk, for comparing two paths that name the same script.
// Cleaned rather than canonical: a file about to be written has no canonical
// path yet, and both sides here come from the same file system.
QString fileKey(const QString &path)
{
    if (path.isEmpty()) return {};
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    // Windows compares names without case, so PlayerInfo.c and playerinfo.c are
    // one file and must not import twice.
    return clean.toLower();
#else
    return clean;
#endif
}

bool sameFile(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty() && fileKey(a) == fileKey(b);
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
QString preambleLeadIn(const QString &preamble)
{
    QString text = preamble;
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r')))
        text.chop(1);
    return text.isEmpty() ? QString() : text + QStringLiteral("\n\n");
}

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

// The add-node search the canvas right-click opens: a search box over a result
// list, at the cursor, keyboard-driven from the first keystroke. This is the
// primary way nodes get added, so it carries the graph's own variables as well
// as the builtins and the catalogue.
//
// Deliberately not a Q_OBJECT: it lives in this file, so a callback is cheaper
// than a signal and does not need moc to see the class.
class AddNodePopup : public QWidget {
public:
    AddNodePopup(Document *doc, std::function<void(const QString &)> onPick,
                 QWidget *parent)
        : QWidget(parent, Qt::Popup), m_doc(doc), m_onPick(std::move(onPick)),
          m_search(new QLineEdit(this)), m_list(new QTreeWidget(this))
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

        m_list->setColumnCount(2);
        m_list->setHeaderHidden(true);
        m_list->setRootIsDecorated(false);
        m_list->setUniformRowHeights(true);
        m_list->setTextElideMode(Qt::ElideRight);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        layout->addWidget(m_list, 1);

        connect(m_search, &QLineEdit::textChanged, this,
                [this](const QString &text) { populate(text); });
        connect(m_list, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *item, int) { pick(item); });
        connect(m_list, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem *item, int) { pick(item); });

        // The caret stays in the search box the whole time; the arrow keys and
        // Return are handed to the list from here so a pick never needs a Tab.
        m_search->installEventFilter(this);
        resize(400, 320);
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

    void addRow(const QString &title, const QString &key, const QString &detail)
    {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, title);
        item->setData(0, kKeyRole, key);
        item->setText(1, detail);
        item->setForeground(1, theme::textDim());
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

    void populate(const QString &query)
    {
        m_list->setUpdatesEnabled(false);
        m_list->clear();
        const QString q = query.trimmed();
        const auto matches = [&q](const QString &a, const QString &b) {
            return q.isEmpty() || a.contains(q, Qt::CaseInsensitive)
                   || b.contains(q, Qt::CaseInsensitive);
        };

        // The value the wire is carrying has no member behind it yet, and this
        // is where it gets one. First row because it is the answer whenever the
        // catalogue has nothing that fits. Exec pins carry no value to store.
        if (m_fitting && m_fitType.kind != PinKind::Exec
            && matches(tr("Promote to variable"), QString()))
            addRow(tr("Promote to variable"), kPromoteKey, tr("new member"));

        // Where a graph starts. Searching for a node by name is no use to
        // someone who does not know EEItemAttached exists, so the way into the
        // ranked event list sits on the canvas menu, above everything a name
        // search can find. A wire looking for a pin is not asking this.
        if (!m_fitting && matches(tr("Add event..."), tr("override")))
            addRow(tr("Add event..."), kAddEventKey, tr("what this class can hook"));

        // Variables lead: they are the one node family that exists only in this
        // graph, so no amount of catalogue searching would turn them up.
        const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
        if (g) {
            for (const GraphVariable &v : g->variables) {
                if (!matches(v.name, v.type)) continue;
                // Get has only an output and Set takes the value on an input,
                // so which of the two is offered follows the drag's direction.
                // The defs are only built when there is a pin to test them
                // against; outside connect mode both rows always show.
                if (!m_fitting
                    || defFits(m_doc->builtins().variableDef(v, false, m_doc->catalog())))
                    addRow(tr("Get %1").arg(v.name),
                           QStringLiteral("var.get.%1").arg(v.id), v.type);
                if (!m_fitting
                    || defFits(m_doc->builtins().variableDef(v, true, m_doc->catalog())))
                    addRow(tr("Set %1").arg(v.name),
                           QStringLiteral("var.set.%1").arg(v.id), v.type);
            }
        }

        if (m_doc) {
            for (const NodeDef &def : m_doc->builtins().all())
                if (matches(def.title, def.subtitle + ' ' + def.category) && defFits(def))
                    addRow(def.title, def.key,
                           def.subtitle.isEmpty() ? def.category : def.subtitle);
        }

        if (!q.isEmpty() && m_doc) {
            SearchOptions opts;
            opts.limit = m_fitting ? kFitScan : 60;
            int kept = 0;
            for (const SearchHit &hit : m_doc->catalog().search(q, opts)) {
                if (m_fitting) {
                    if (kept >= kFitKeep) break;
                    // defFor is memoised, so walking the ranking costs one def
                    // build per entry per session and nothing after that.
                    if (!defFits(m_doc->catalog().defFor(hit.key))) continue;
                    ++kept;
                }
                addRow(hit.title, hit.key,
                       hit.subtitle.isEmpty() ? hit.category : hit.subtitle);
            }
        }

        if (m_list->topLevelItemCount() == 0) {
            auto *empty = new QTreeWidgetItem(m_list);
            empty->setText(0, m_fitting
                                  ? tr("Nothing here takes that pin.")
                                  : tr("No match. Try a shorter term."));
            empty->setFlags(Qt::NoItemFlags);
            empty->setForeground(0, theme::textDim());
        } else {
            m_list->setCurrentItem(m_list->topLevelItem(0));
        }
        m_list->setUpdatesEnabled(true);
    }

    Document *m_doc;
    std::function<void(const QString &)> m_onPick;
    QLineEdit *m_search;
    QTreeWidget *m_list;
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
    m_tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    tabLayout->addWidget(m_tabs, 1);

    m_tabList = new QToolButton(tabRow);
    m_tabList->setText(QStringLiteral("▾"));
    m_tabList->setToolTip(QStringLiteral("All scripts"));
    m_tabList->setAutoRaise(true);
    tabLayout->addWidget(m_tabList);

    layout->addWidget(tabRow);
    layout->addWidget(m_view, 1);
    setCentralWidget(central);

    buildMenus();
    buildToolBar();
    buildDocks();
    buildStatusBar();

    connect(m_tabList, &QToolButton::clicked, this, &MainWindow::showTabList);
    connect(m_tabs, &QTabBar::customContextMenuRequested, this,
            [this](const QPoint &) { showTabList(); });

    connect(m_doc, &Document::graphChanged, this, &MainWindow::onGraphChanged);
    connect(m_doc, &Document::projectChanged, this, &MainWindow::onProjectChanged);
    connect(m_doc, &Document::activeScriptChanged, this, [this]() {
        m_scene->rebuild();
        runAnalysis();
        refreshTabs();
        m_view->zoomToFit();
    });
    connect(m_doc, &Document::modifiedChanged, this, [this](bool) { updateWindowTitle(); });
    connect(m_tabs, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_palette, &PalettePanel::nodeRequested,
            this, &MainWindow::onPaletteNodeRequested);
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
        CodeDialog::editNodeCode(this, m_doc, id);
    });

    refreshTabs();
    m_scene->rebuild();
    runAnalysis();
    updateWindowTitle();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("New mod..."),
                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
                    this, &MainWindow::newMod);
    file->addAction(QStringLiteral("New project"), QKeySequence::New,
                    this, &MainWindow::newProject);
    file->addAction(QStringLiteral("Open project..."), QKeySequence::Open,
                    this, &MainWindow::openProject);
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
    file->addAction(QStringLiteral("Edit mod config..."), this,
                    &MainWindow::editModConfig);
    file->addSeparator();
    file->addAction(QStringLiteral("Exit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    edit->addAction(QStringLiteral("Undo"), QKeySequence::Undo, m_doc, &Document::undo);
    QAction *redo = edit->addAction(QStringLiteral("Redo"), m_doc, &Document::redo);
    // The single-sequence overload takes only the first platform binding, which
    // on Windows is Ctrl+Y, leaving the contracted Ctrl+Shift+Z unbound.
    redo->setShortcuts(QKeySequence::Redo);
    edit->addSeparator();
    edit->addAction(QStringLiteral("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D),
                    this, [this]() { m_scene->duplicateSelection(); });
    edit->addAction(QStringLiteral("Delete"), QKeySequence::Delete,
                    this, [this]() { m_scene->deleteSelectedNodes(); });
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

    QDockWidget *outlinerDock =
        makeDock(QStringLiteral("Graph Outliner"), m_outliner, Qt::LeftDockWidgetArea);
    QDockWidget *paletteDock =
        makeDock(QStringLiteral("Node Palette"), m_palette, Qt::LeftDockWidgetArea);
    QDockWidget *eventsDock =
        makeDock(QStringLiteral("Events"), m_events, Qt::LeftDockWidgetArea);
    QDockWidget *explorerDock =
        makeDock(QStringLiteral("Mod Explorer"), m_explorer, Qt::LeftDockWidgetArea);
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

    splitDockWidget(outlinerDock, paletteDock, Qt::Vertical);
    // Events sits under the palette, not under the explorer: the two node
    // sources belong together, and files are the thing you come back to least.
    splitDockWidget(paletteDock, eventsDock, Qt::Vertical);
    splitDockWidget(eventsDock, explorerDock, Qt::Vertical);
    splitDockWidget(varsDock, inspectorDock, Qt::Vertical);
    splitDockWidget(inspectorDock, minimapDock, Qt::Vertical);

    // Four panels in one column. Events takes the largest share because it is
    // the only one that is read rather than searched: its whole job is showing
    // hooks the user could not have named. The palette keeps enough rows for a
    // search result, and the outliner and the explorer are scanned down a list.
    resizeDocks({outlinerDock, paletteDock, eventsDock, explorerDock},
                {120, 170, 520, 110}, Qt::Vertical);
    resizeDocks({varsDock, inspectorDock, minimapDock}, {240, 300, 160}, Qt::Vertical);
    resizeDocks({outlinerDock, varsDock}, {320, 380}, Qt::Horizontal);
    // Tall enough to read a method without scrolling. resizeDocks only gets a
    // say once the window has a size, so the panel carries its own minimum.
    m_codeView->setMinimumHeight(180);
    resizeDocks({codeDock}, {320}, Qt::Vertical);
    // Same reason, and the number that matters here is rows: the lifecycle
    // hooks a mod starts from have to be on screen without a scroll.
    m_events->setMinimumHeight(320);

    // Clicking a line in the generated file selects the node behind it, and
    // selecting a node marks the lines it produced.
    connect(m_codeView, &CodeViewPanel::nodeActivated, this, [this](const QString &id) {
        revealNode(m_doc, m_view, m_inspector, id);
    });
    connect(m_doc, &Document::selectionChanged, this, [this]() {
        const QStringList sel = m_doc->selection();
        if (sel.size() == 1) m_codeView->revealNode(sel.first());
    });

    // A .c is a graph, a .cpp is a config tree, a .sdzn is a project, and
    // everything else is text, so the explorer says which of the four it found
    // and the main window opens it the right way.
    connect(m_explorer, &ExplorerPanel::fileActivated, this, &MainWindow::openModFile);
    connect(m_explorer, &ExplorerPanel::scriptActivated, this, &MainWindow::openModScript);
    connect(m_explorer, &ExplorerPanel::configActivated, this, &MainWindow::openModConfig);
    connect(m_explorer, &ExplorerPanel::projectActivated,
            this, &MainWindow::openProjectPath);
    syncExplorerRoot();

    // Dock toggles go under View, which buildMenus left open for them.
    for (QAction *a : menuBar()->actions()) {
        if (a->text() != QLatin1String("&View") || !a->menu()) continue;
        for (QDockWidget *d : {outlinerDock, paletteDock, eventsDock, explorerDock,
                               varsDock, inspectorDock, minimapDock, codeDock})
            a->menu()->addAction(d->toggleViewAction());
    }
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
        m_tabs->setTabToolTip(i, QStringLiteral("%1/%2.c").arg(s.folder, s.name));
        if (s.id == p.activeId) active = i;
    }
    m_tabs->setCurrentIndex(active);
}

void MainWindow::onTabChanged(int index)
{
    if (index < 0) return;
    const QString id = m_tabs->tabData(index).toString();
    if (!id.isEmpty()) m_doc->setActiveScript(id);
}

void MainWindow::editSelectedCode()
{
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
    m_analysis = analyzeGraph(*g, m_doc->catalog(), m_doc->builtins());
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

    QMenu menu(this);
    for (const ScriptEntry &s : p.scripts) {
        QAction *entry = menu.addAction(QStringLiteral("%1/%2").arg(s.folder, s.name));
        entry->setCheckable(true);
        entry->setChecked(s.id == p.activeId);
        const QString id = s.id;
        connect(entry, &QAction::triggered, this, [this, id]() { m_doc->setActiveScript(id); });
    }
    menu.exec(m_tabList->mapToGlobal(QPoint(0, m_tabList->height())));
}

void MainWindow::newProject()
{
    m_doc->resetToNew();
    m_view->zoomToFit();
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
    if (m_doc->isModified()) {
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this, QStringLiteral("New mod"),
            QStringLiteral("The open project has unsaved changes. Starting a new mod "
                           "closes it."),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) return;
        if (answer == QMessageBox::Save) {
            saveProject();
            if (m_doc->isModified()) return;
        }
    }

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

    refreshTabs();
    m_scene->rebuild();
    runAnalysis();
    updateWindowTitle();
    syncExplorerRoot();
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
        if (!sameFile(s.sourcePath, path)) continue;
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
    const QString module = moduleForPath(path);
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
                         ? QFileInfo(path).completeBaseName()
                         : entry.graph.className;
        entry.folder = module.isEmpty() ? entry.graph.module : module;
        if (entry.folder.isEmpty()) entry.folder = QStringLiteral("4_World");
        entry.graph.module = entry.folder;
        entry.sourcePath = source;
        // The preamble belongs to the file rather than to any one class in it,
        // so the first script carries it and writing the file back puts it in
        // front of all of them.
        if (i == 0) entry.preamble = result.preamble;

        p.scripts.append(entry);
        if (firstId.isEmpty()) firstId = entry.id;
    }

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

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open project"), QString(),
        QStringLiteral("SUDO node projects (*.sdzn);;All files (*)"));
    if (!path.isEmpty()) openProjectPath(path);
}

void MainWindow::openProjectPath(const QString &path)
{
    QString error;
    if (!m_doc->openProject(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("Open project"), error);
        return;
    }
    m_view->zoomToFit();
    flashStatus(QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::saveProject()
{
    if (m_doc->projectPath().isEmpty()) {
        saveProjectAs();
        return;
    }
    QString error;
    if (!m_doc->saveProject(m_doc->projectPath(), &error))
        QMessageBox::warning(this, QStringLiteral("Save project"), error);
    else
        flashStatus(QStringLiteral("Saved."));
}

void MainWindow::saveProjectAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save project as"), m_doc->project().name + ".sdzn",
        QStringLiteral("SUDO node projects (*.sdzn)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_doc->saveProject(path, &error))
        QMessageBox::warning(this, QStringLiteral("Save project"), error);
    else
        updateWindowTitle();
}

bool MainWindow::writeScriptFile(const QString &path, QString *error)
{
    const Project &p = m_doc->project();
    QVector<const ScriptEntry *> parts;
    for (const ScriptEntry &s : p.scripts)
        if (sameFile(s.sourcePath, path)) parts.append(&s);
    if (parts.isEmpty()) {
        if (error)
            *error = QStringLiteral("no script in this project came from %1")
                         .arg(QDir::toNativeSeparators(path));
        return false;
    }

    // The file as it stands, so the user regions inside it survive the rewrite.
    const QString previous = readFileText(path);

    QStringList names;
    for (const ScriptEntry *s : parts) names << s->graph.className;

    QString text;
    for (const ScriptEntry *s : parts) {
        if (s->preamble.isEmpty()) continue;
        text = preambleLeadIn(s->preamble);
        break;
    }
    for (int i = 0; i < parts.size(); ++i) {
        const GenResult gen =
            generateEnforce(parts.at(i)->graph, m_doc->catalog(), m_doc->builtins(), p,
                            classSection(previous, names.at(i), names));
        if (i > 0) text += QLatin1Char('\n');
        text += gen.code;
    }

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
    // A script that was imported goes back to the file it came from, wherever
    // that is. Exporting it into the folder as well would leave two copies of
    // one class in the mod, and the compiler would take both.
    int intoFolder = 0;
    int intoSource = 0;
    for (const ScriptEntry &s : m_doc->project().scripts)
        (s.sourcePath.isEmpty() ? intoFolder : intoSource)++;

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
    // Two scripts can share one file, and that file is written once with both
    // classes in it.
    QSet<QString> filesDone;
    for (const ScriptEntry &s : m_doc->project().scripts) {
        if (!s.sourcePath.isEmpty()) {
            const QString key = fileKey(s.sourcePath);
            if (filesDone.contains(key)) continue;
            filesDone.insert(key);
            QString error;
            if (writeScriptFile(s.sourcePath, &error)) returned++;
            else problems << QStringLiteral("%1: %2").arg(s.name, error);
            continue;
        }

        const QString folder = QDir(dir).filePath(s.folder);
        QDir().mkpath(folder);
        const QString path = QDir(folder).filePath(s.name + ".c");

        // Regenerating over an existing file keeps its user regions.
        const QString previous = readFileText(path);

        const GenResult gen = generateEnforce(s.graph, m_doc->catalog(),
                                              m_doc->builtins(), m_doc->project(),
                                              previous);
        QSaveFile out(path);
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
        flashStatus(QStringLiteral("There was nothing to export."));
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
    flashStatus(report + QLatin1Char('.'));
}

void MainWindow::updateWindowTitle()
{
    const QString name = m_doc->projectPath().isEmpty()
                             ? m_doc->project().name
                             : QFileInfo(m_doc->projectPath()).fileName();
    setWindowTitle(QStringLiteral("%1%2 - SUDO DayZ Node Mod")
                       .arg(name, m_doc->isModified() ? QStringLiteral(" *") : QString()));
}
