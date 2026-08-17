// The Script Canvas window: menu bar, toolbar, script tabs, canvas, docks,
// and the errors/warnings status bar.
#pragma once

#include "analysis.h"
#include "recentprojects.h"

#include <QList>
#include <QMainWindow>
#include <QPointF>
#include <QPointer>

struct Graph;
struct ImportResult;
struct PinRef;
struct StartTemplate;
class Document;
class StartPage;
class QDockWidget;
class QStackedWidget;
class NodeScene;
class NodeView;
class PalettePanel;
class EventsPanel;
class OutlinerPanel;
class VariablesPanel;
class InspectorPanel;
class MiniMapWidget;
class CodeViewPanel;
class ExplorerPanel;
class ModBrowserPanel;
class TestPanel;
class QTabBar;
class QLabel;
class QMenu;
class QTimer;
class QToolBar;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Document *doc, QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Watches the read only bar for its own resize: the window's arrives before
    // the layout has handed the bar its new width, and the text is elided to it.
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Dock heights are shared out here rather than in buildDocks, because
    // resizeDocks divides the space the window has and the window has none
    // until it is shown.
    void showEvent(QShowEvent *event) override;
    // The last chance to keep unsaved work, so it is the one place that has to
    // be able to refuse the close.
    void closeEvent(QCloseEvent *event) override;

public slots:
    void openProject();
    void openProjectPath(const QString &path);
    // Both report whether the project reached disk: a cancelled Save as is a
    // refusal, and treating it as a success is how a close prompt throws away
    // the work it just offered to keep.
    bool saveProject();
    bool saveProjectAs();
    void newProject();
    // The start page, in place of the editor. The editor is not torn down; it
    // keeps its graph, its docks and its scroll position and comes back as it
    // was.
    void showStartPage();
    void showEditor();
    // Scaffolds a mod folder from the bundled template and starts a project
    // inside it. "New project" stays the bare graph with no folder behind it.
    void newMod();
    // Points an existing project at a mod folder it was not scaffolded with.
    void setModFolder();
    // The Mod Browser, in front and holding the keyboard. Both of these end in
    // the same panel: the first asks for a path on disk, the second opens the
    // library of what is installed. They raise the dock rather than toggling it,
    // because a menu entry that hides the panel when it is already showing is a
    // menu entry that appears to do nothing.
    void browseInstalledMods();
    void openModFromDisk();
    // Puts what the mod library has found on the start page's Read a mod card.
    void updateModLibraryLine();
    void exportScripts();
    // Closes the tab in front. A script lives only inside the project, so this
    // takes it out of the project; the .c it was imported from is left alone.
    void closeActiveScript();
    // Puts the last closed script back where it was, newest first. The stack is
    // this session's, so it empties when a project is opened or started.
    void reopenClosedScript();
    // Writes the active script back to the .c it was imported from, asking for
    // a file only when it never came from one.
    void saveScriptFile();
    // The mod's own config.cpp, in the config editor. The file is three folders
    // deep and is edited far more often than it is found, so it gets a menu
    // entry rather than a walk through the explorer.
    void editModConfig();
    void showGeneratedCode();

private slots:
    // A class the Mod Browser handed over, out of somebody else's mod. It joins
    // the project as a read only script: it can be read, framed and generated
    // on screen, and no export writes it. Connected directly, because Graph is
    // not a registered metatype and a queued connection would drop it.
    void openBrowsedGraph(const QString &name, const Graph &graph);
    // A text file from the Mod Explorer, in its own editor window.
    void openModFile(const QString &path);
    // A .c from the Mod Explorer, imported and opened as a graph. Falls back to
    // openModFile when the importer cannot read it, so no file in the tree is
    // unopenable.
    void openModScript(const QString &path);
    // A .cpp from the Mod Explorer, opened as a class tree. Falls back to
    // openModFile the same way, since a config the parser chokes on is exactly
    // the one the user needs to see.
    void openModConfig(const QString &path);
    // The raw / comment nodes are the ones whose real content is text the user
    // typed, so they are the ones with something to open an editor on.
    void editSelectedCode();
    void onGraphChanged();
    void onProjectChanged();
    void onTabChanged(int index);
    void onPaletteNodeRequested(const QString &key);
    void onContextAdd(const QPointF &scenePos);
    // Places an event, unless the graph already overrides that method: two
    // overrides of one method do not compile, so the second request goes to
    // the node the user already has instead of adding a twin.
    void placeEventNode(const QString &key);
    // Unreal's custom event: a method this script declares. Asks for a name,
    // declares `void <name>()` and drops its entry node, as one undo step.
    void addCustomEvent();
    // A tile from the start page: a project that ships with the app, a
    // skeleton imported into a project of its own, or a folder of working
    // scripts imported the same way a mod folder's own files are.
    void startFromTemplate(const StartTemplate &tpl);
    // The Files kind. Split out because it is the only one that reads more than
    // one file and the only one that declares dependencies.
    void startFromTemplateFiles(const StartTemplate &tpl);
    // Writes the open project to its sidecar. Does nothing for a project with
    // no file behind it: the sidecar lives beside the project so that its
    // relative paths resolve, and there is nowhere to put one otherwise.
    void writeAutosave();

private:
    Document *m_doc;
    NodeScene *m_scene;
    NodeView *m_view;
    // The editor and the start page, one shown at a time. The stack is the
    // central widget, so switching costs a page change and nothing is rebuilt.
    QStackedWidget *m_stack = nullptr;
    QWidget *m_editor = nullptr;
    StartPage *m_startPage = nullptr;
    RecentProjects m_recent;
    // Docks and toolbar put away while the start page is up, so they can come
    // back exactly as they were. Held as guarded pointers because the View menu
    // can close a dock while the page is showing.
    QList<QPointer<QDockWidget>> m_putAwayDocks;
    bool m_putAwayToolBar = false;
    bool m_docksSized = false;
    QTimer *m_autosaveTimer = nullptr;
    QTabBar *m_tabs;
    QToolButton *m_tabList;
    // Lives in the File menu and is re-labelled every time the project changes,
    // so it names the script it would bring back.
    QAction *m_reopenAction = nullptr;
    PalettePanel *m_palette;
    EventsPanel *m_events;
    OutlinerPanel *m_outliner;
    VariablesPanel *m_variables;
    InspectorPanel *m_inspector;
    MiniMapWidget *m_minimap;
    CodeViewPanel *m_codeView;
    ExplorerPanel *m_explorer;
    ModBrowserPanel *m_modBrowser = nullptr;
    // Shown over the canvas while the active script came out of another mod.
    // The graph is honest about being read only or the user finds out by
    // exporting it, which is the one moment it must not be a surprise.
    QLabel *m_readOnlyBar = nullptr;
    // Null until buildDocks runs. syncExplorerRoot is called from in there, and
    // it refreshes this panel too.
    TestPanel *m_testRun = nullptr;
    // Opened by buildMenus so the bar keeps its order, filled by buildDocks
    // once the panel that owns the actions exists.
    QMenu *m_testMenu = nullptr;
    // The counts are the contracted status line and keep their own label, so a
    // transient note can never sit where the diagnostics summary should be.
    QLabel *m_status;
    QLabel *m_message;
    QTimer *m_statusResetTimer;
    AnalysisResult m_analysis;
    // The brand mark on the right of the toolbar, the flexible gap that holds
    // it there, and the height and screen ratio its pixmap was last cut for.
    QToolBar *m_toolBar = nullptr;
    QWidget *m_toolBarGap = nullptr;
    QLabel *m_cornerMark = nullptr;
    int m_cornerHeight = 0;
    qreal m_cornerRatio = 0.0;
    // Where the next node lands. Set by the canvas right-click, consumed by
    // whichever surface the user picks a node from; unset means the view centre.
    QPointF m_pendingAddPos;
    bool m_hasPendingAdd = false;

    void buildMenus();
    void buildToolBar();
    void buildDocks();
    void buildTestMenu();
    void buildStatusBar();
    // Splits the dock columns over the window's real height. Called on the
    // first show and never again, so a layout the user has dragged stays theirs.
    void applyDockSizes();
    // The height of the bottom row. Split out because the share it needs depends
    // on whether the Mod Browser is the tab in front, which changes long after
    // the one-time sizing above has run.
    void applyBottomRowSize();
    // Opens the Mod Browser on the mod named by SUDO_UI_BROWSE and hands its
    // first class to the canvas, waiting out the scan and the import. For the
    // headless UI check only: with the variable unset, which is every normal
    // run, this returns at once.
    void browseForScreenshot();
    // Drops the menu named by SUDO_UI_MENU open, so the menu bar can be checked
    // the way a user meets it. Same deal as browseForScreenshot: unset in every
    // normal run, and then this returns at once.
    void openMenuForScreenshot();
    // Opens the canvas popup named by SUDO_UI_POPUP (add, connect, connect-exec
    // or event) through the code path a user reaches it by, then paints it into
    // the window so a grab of the window shows it. main.cpp composites QMenu
    // windows and these are not menus, and the drag-out menu is the one surface
    // that cannot be photographed any other way. Unset in every normal run.
    void openPopupForScreenshot();
    // The read only bar over the canvas, and the dim tab text that goes with it.
    void updateReadOnlyBar();
    // True when the active script came out of another author's mod, in which
    // case it says so on the status line. Every command that would change the
    // graph asks this first: a browsed graph is there to be read.
    bool refuseReadOnlyEdit();
    void buildLayoutActions(QMenu *menu, QToolBar *bar);
    // Recuts the corner mark for the current toolbar height and hides it when
    // the bar has no room for it beside the actions.
    void updateCornerMark();
    void refreshTabs();
    void runAnalysis();
    void updateStatusCounts();
    // Clears the message's tooltip, so a caller with more to say than fits on
    // one line sets it afterwards and the next flash does not inherit it.
    void flashStatus(const QString &text);
    void updateWindowTitle();
    // Writes one .c from every script in the project that came from it, the
    // file's preamble in front. False and a reason when nothing could be
    // written; the file is left as it was.
    bool writeScriptFile(const QString &path, QString *error);
    // Roots the Mod Explorer at the project's mod folder. Called whenever that
    // folder can have changed, which includes loading a different project.
    void syncExplorerRoot();
    // Where Export scripts writes without being asked: <modRoot>/<prefix>/Scripts.
    // Empty when the project has no mod folder.
    QString scriptsFolder() const;
    // The project's own config.cpp under that folder. Empty when there is no mod
    // folder, or when the file is not there yet.
    QString modConfigPath() const;
    // The add-node search the canvas right-click opens, at the cursor.
    void showAddNodeSearch(const QPointF &scenePos);
    // The Events dock's list, at the cursor. Reached from the same right-click,
    // because the canvas is where the question is asked.
    void showEventSearch(const QPointF &scenePos);
    // The same search opened by letting a wire go over empty canvas, narrowed
    // to nodes that pin could connect to, with promoting it to a member first.
    void showConnectSearch(const PinRef &from, const QPointF &scenePos);
    // Every script in the project, filtered as you type, for when the tab bar
    // cannot show them all. The bar scrolls past about eight tabs and a bar you
    // have to scroll is not a way to find anything.
    void showTabList();
    // The right-click menu on one tab: what to close, and the list above.
    void showTabMenu(const QPoint &pos);
    // The same menu without showing it, so the screenshot hook can open it the
    // way the right-click does. Heap allocated and deletes itself on close.
    QMenu *buildTabMenu(int index);
    // Closes a run of tabs as one gesture, named by script id.
    //
    // A script read out of another author's mod costs nothing to close: the Mod
    // Browser has it whenever it is wanted again. Everything else is the user's
    // and is asked about, once for the gesture rather than once per tab. Ids
    // that no longer resolve are skipped, so a stale menu closes nothing by
    // surprise.
    void closeScripts(const QStringList &ids);
    // Enables Reopen closed script and puts the name of what it would bring back
    // on it, so the entry is never an offer with nothing behind it.
    void updateReopenAction();
    // Save, Discard, Cancel over unsaved work, where `action` names what is
    // about to happen. False means the user cancelled and the caller must stop:
    // a Cancel that is treated as a yes destroys exactly the work the prompt
    // was protecting.
    bool maybeSaveChanges(const QString &action);
    // Called after the project reaches disk: the sidecar is stale, the recent
    // list has a new front entry, and the title has lost its asterisk.
    void afterSave(const QString &path);
    // Turns one imported file into script entries on the project, returning the
    // id of the first. `sourcePath` is the .c it came from, empty for a
    // skeleton that was never on disk.
    QString appendImportedScripts(const ImportResult &result,
                                  const QString &sourcePath, const QString &module);
    // <project>.sdzn.autosave, or empty when there is no project file.
    QString autosavePathFor(const QString &projectPath) const;
    void clearAutosave(const QString &projectPath);
    // Offers a sidecar newer than the project. Returns the file to load, which
    // is the project itself unless the user asked for the recovery. Nothing is
    // ever written back over the project here: a recovered graph is loaded as
    // unsaved work and stays that way until the user saves it.
    QString recoveryFor(const QString &projectPath);
};
