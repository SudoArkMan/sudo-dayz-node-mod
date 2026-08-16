// Mod browser: the installed mods, and what is inside them.
//
// Reading somebody else's mod is how most people learn this engine, and the
// answer to "how did they do that" is a folder full of pbos you cannot open. So
// this lists every mod on the machine, and opening one shows its classes as
// graphs.
//
// The panel is honest about coverage on purpose. The importer models most
// Enforce, not all of it, and a mod whose methods mostly stay text is worth
// knowing about before you go hunting for a graph that is not there. Every row
// carries the share of its methods that became nodes.
//
// It never writes to a mod folder. Scripts are extracted to the app's own
// cache, and each graph it hands out is marked read only.
#pragma once

#include <QHash>
#include <QWidget>

class Document;
class ModLibrary;
class ModOpenJob;
struct Graph;
struct ModEntry;

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class ModBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit ModBrowserPanel(Document *doc, QWidget *parent = nullptr);
    ~ModBrowserPanel() override;

    ModLibrary *library() const { return m_library; }

    // Folder of the mod currently shown, or empty.
    QString currentMod() const { return m_openFolder; }
    // True while an import is still being spent a few files at a time. The job
    // outlives the run so its classes stay clickable, so the timer driving it is
    // what says whether there is more to do.
    bool isOpening() const;

public slots:
    void refresh(bool force = false);
    // Adds a folder of mods to the library and rescans. Asks for one when the
    // path is empty, which is what the toolbar button does.
    void addFolder(const QString &folder = QString());
    // Selects a mod by folder and starts reading it. False when the scan has
    // not turned that folder up, or the filter is hiding its row.
    bool selectMod(const QString &folder);
    // Hands over the class on row `row` as if it had been activated, which is
    // otherwise a double-click. False when the open has produced no such row. A
    // headless check of the window has no hands, and a picture of the browser
    // without the graph it produces is half the feature.
    bool openClassAt(int row);

signals:
    // A class the user activated. Connect this directly: the graph is passed by
    // const reference and Graph is not registered as a queued-connection type.
    // The graph carries the read only mark, so a window that opens it must not
    // offer to save it back over the mod.
    void graphRequested(const QString &name, const Graph &graph);
    // One line for the window's status bar.
    void statusChanged(const QString &text);
    // The mod finished importing and its class list is complete. `ok` is false
    // for a mod whose pbos gave up nothing, where the status line holds why.
    void openFinished(const QString &folder, bool ok);

private slots:
    void onFilterChanged();
    void onModsChanged();
    void onScanProgress(int done, int total);
    void onScanFinished(int found, bool cancelled);
    void onModSelected();
    void onClassActivated(QTreeWidgetItem *item, int column);
    void onShowNotes();
    void stepOpen();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildModList();
    void startOpen(const ModEntry &mod);
    void stopOpen();
    void refreshClassList();
    void setStatus(const QString &text);
    void elideStatus();
    // Drops the columns a narrow dock has no room for.
    void fitColumns();
    QString filterText() const;

    Document *m_doc;
    ModLibrary *m_library;

    QLineEdit *m_filter;
    QPushButton *m_refresh;
    QPushButton *m_addFolder;
    QTreeWidget *m_mods;
    QTreeWidget *m_classes;
    QLabel *m_status;
    QString m_statusText;
    QPushButton *m_notes;
    QTimer *m_openTimer;

    // The import in flight. Held by pointer because ModOpenJob carries whole
    // graphs and this panel is copied around by nothing.
    ModOpenJob *m_job = nullptr;
    QString m_openFolder;
    // Coverage per mod folder, remembered so a row keeps its number after the
    // user has looked at another mod and come back.
    QHash<QString, int> m_modelled;
};
