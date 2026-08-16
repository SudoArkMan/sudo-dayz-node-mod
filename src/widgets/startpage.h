// The page the app opens on: what you were working on, what you can start, and
// what the tool can write for you.
//
// A page rather than a modal. The editor behind it is real and stays built, so
// coming back to it is a switch and not a rebuild, and nothing here is a dead
// end the user has to dismiss before the app becomes usable.
//
// Three regions, in the order the questions get asked: the project you already
// have, a new one, and a shape to start it from. Each one is a bounded panel,
// and the recent list is the only region whose height is not known in advance,
// so it takes the left column on its own while the two fixed regions stack in
// the right one. That is what keeps the page whole on a first run, where the
// left panel holds one line and the right column is still full.
#pragma once

#include "widgets/newscriptdialog.h"

#include <QVector>
#include <QWidget>

class RecentProjects;

class QAbstractButton;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;

// What picking a tile asks the window to do.
enum class StartTemplateKind {
    Script,   // Enforce text to import into a fresh project
    Project,  // a .sdzn that ships with the app, opened as it is
};

struct StartTemplate {
    QString id;
    QString title;
    // One line: what the tile gives you, and what it is for. A gallery of names
    // with no answer to "which of these do I want" is a gallery nobody uses.
    QString summary;
    StartTemplateKind kind = StartTemplateKind::Script;
    // Script kind: the same options the New script dialog takes, so both write
    // the same skeleton and neither can drift from the other.
    NewScriptOptions script;
    QString module;       // "4_World", "5_Mission": where the script belongs
    QString projectPath;  // Project kind: the file to open
    // A Project tile whose file is not beside the executable. Shown, and
    // refused with a reason, rather than left out of a gallery that would then
    // differ between installs for no visible cause.
    bool available = true;
};

// The starter set. `resourceDir` is the resources folder, for the tiles that
// open a file shipped with the app; empty means look beside the executable.
QVector<StartTemplate> startTemplates(const QString &resourceDir = QString());

class StartPage : public QWidget {
    Q_OBJECT
public:
    // `recent` is owned by the window and outlives the page.
    explicit StartPage(RecentProjects *recent, QWidget *parent = nullptr);

    // Restats the recent list and redraws the rows. Only a file that has
    // actually changed is read again, so this is cheap enough to run on every
    // return to the page.
    void refresh();

    // The templates column on its own, for a screenshot of the gallery.
    QWidget *gallery() const { return m_gallery; }

    // The second line of the Read a mod card. The window pushes what the mod
    // library has actually found, because "266 mods installed on this machine"
    // is an invitation and "read a mod" on its own is a label.
    void setModLibraryLine(const QString &text);

signals:
    void openRequested(const QString &path);
    void browseRequested();
    void newProjectRequested();
    void newModRequested();
    // Somebody else's mod, read rather than started. The route into this app is
    // as often "show me how they did it" as it is "start something", so it sits
    // beside New and Open rather than behind a dock tab.
    void browseModsRequested();
    // Connect directly. StartTemplate is not registered as a metatype, so a
    // queued connection would drop it.
    void templateRequested(const StartTemplate &tpl);
    void statusMessage(const QString &text);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void openSelected();
    void showRecentMenu(const QPoint &at);
    void removeSelected();

private:
    QWidget *buildRecentPanel();
    QWidget *buildStartPanel();
    QWidget *buildTemplatesPanel();
    void rebuildRecent();
    QString selectedPath() const;

    RecentProjects *m_recent;
    QLabel *m_lockup;
    // Holds the two columns. The recent panel's share of the width depends on
    // whether it has anything in it, so the stretch is set from rebuildRecent.
    QHBoxLayout *m_columns;
    QWidget *m_recentPanel;   // aligned to the top while it is empty, see rebuildRecent
    QListWidget *m_list;
    QWidget *m_empty;         // stands in for the list while there is nothing in it
    QLabel *m_missingNote;    // footer, only while some entry has moved
    QWidget *m_gallery;
    QWidget *m_firstAction;  // where the keyboard lands when there are no rows
    QAbstractButton *m_readModCard;  // a StartCard, whose summary the window sets
    QVector<StartTemplate> m_templates;
};
