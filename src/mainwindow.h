// The Script Canvas window: menu bar, toolbar, script tabs, canvas, docks,
// and the errors/warnings status bar.
#pragma once

#include "analysis.h"

#include <QMainWindow>
#include <QPointF>

struct PinRef;
class Document;
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

public slots:
    void openProject();
    void openProjectPath(const QString &path);
    void saveProject();
    void saveProjectAs();
    void newProject();
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

private:
    Document *m_doc;
    NodeScene *m_scene;
    NodeView *m_view;
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
    // The counts are the contracted status line and keep their own label, so a
    // transient note can never sit where the diagnostics summary should be.
    QLabel *m_status;
    QLabel *m_message;
    QTimer *m_statusResetTimer;
    AnalysisResult m_analysis;
    // Where the next node lands. Set by the canvas right-click, consumed by
    // whichever surface the user picks a node from; unset means the view centre.
    QPointF m_pendingAddPos;
    bool m_hasPendingAdd = false;

    void buildMenus();
    void buildToolBar();
    void buildDocks();
    void buildStatusBar();
    void buildLayoutActions(QMenu *menu, QToolBar *bar);
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
};
