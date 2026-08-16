// The Script Canvas window: menu bar, toolbar, script tabs, canvas, docks,
// and the errors/warnings status bar.
#pragma once

#include "analysis.h"
#include "recentprojects.h"

#include <QList>
#include <QMainWindow>
#include <QPointF>
#include <QPointer>

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
    void exportScripts();
    // Writes the active script back to the .c it was imported from, asking for
    // a file only when it never came from one.
    void saveScriptFile();
    // The mod's own config.cpp, in the config editor. The file is three folders
    // deep and is edited far more often than it is found, so it gets a menu
    // entry rather than a walk through the explorer.
    void editModConfig();
    void showGeneratedCode();

private slots:
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
    // A tile from the start page: a project that ships with the app, or a
    // skeleton imported into a project of its own.
    void startFromTemplate(const StartTemplate &tpl);
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
    QTimer *m_autosaveTimer = nullptr;
    QTabBar *m_tabs;
    QToolButton *m_tabList;
    PalettePanel *m_palette;
    EventsPanel *m_events;
    OutlinerPanel *m_outliner;
    VariablesPanel *m_variables;
    InspectorPanel *m_inspector;
    MiniMapWidget *m_minimap;
    CodeViewPanel *m_codeView;
    ExplorerPanel *m_explorer;
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
    // Every script in the project, for when the tab bar cannot show them all.
    void showTabList();
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
