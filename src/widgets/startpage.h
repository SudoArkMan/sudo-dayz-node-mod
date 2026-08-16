// The page the app opens on: what you were working on, what you can start, and
// what the tool can write for you.
//
// A page rather than a modal. The editor behind it is real and stays built, so
// coming back to it is a switch and not a rebuild, and nothing here is a dead
// end the user has to dismiss before the app becomes usable.
//
// Three columns, in the order the questions get asked: the project you already
// have, a new one, and a shape to start it from.
#pragma once

#include "widgets/newscriptdialog.h"

#include <QVector>
#include <QWidget>

class RecentProjects;

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

signals:
    void openRequested(const QString &path);
    void browseRequested();
    void newProjectRequested();
    void newModRequested();
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
    QWidget *buildRecentColumn();
    QWidget *buildStartColumn();
    QWidget *buildTemplatesColumn();
    void rebuildRecent();
    QString selectedPath() const;

    RecentProjects *m_recent;
    QLabel *m_lockup;
    QListWidget *m_list;
    QLabel *m_listNote;   // stands in for the list while there is nothing in it
    QWidget *m_gallery;
    QWidget *m_firstAction;  // where the keyboard lands when there are no rows
    QVector<StartTemplate> m_templates;
};
