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
//
// A fourth region was added under the recent list: what is new. It sits in the
// left column rather than the right one because the right column's two panels
// are the page's fixed furniture and the left column is where the page already
// says what has happened. It is bounded like the others, and the notes it draws
// are capped in lines, so a long release cannot push the recent list off a
// laptop screen.
#pragma once

#include "update.h"
#include "widgets/newscriptdialog.h"

#include <QVector>
#include <QWidget>

class RecentProjects;

class QAbstractButton;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QVBoxLayout;

// What picking a tile asks the window to do.
enum class StartTemplateKind {
    Script,   // Enforce text to import into a fresh project
    Project,  // a .sdzn that ships with the app, opened as it is
    // A folder of real .c files shipped with the app, imported into a fresh
    // project one file at a time. This is the kind a working template uses.
    //
    // Real files rather than a .sdzn, for a reason that is checkable: the
    // importer generates the raw-body version of every method first and takes
    // the node version only when regenerating it produces the same bytes.
    // Anything the graph cannot reproduce exactly stays as text and is written
    // back verbatim, so a template shipped as .c cannot silently degrade into
    // code that does not compile. The acceptance test is the round trip.
    //
    // Not a .sdzn either, because a Project tile opens the shipped file itself:
    // Ctrl+S on a template would write over the copy in resources/.
    Files,
};

// Which section of the gallery a tile sits in. Ten tiles in one column is a
// list; the same ten under three headings is an answer to "which of these do I
// want", which is the only question a gallery has to answer.
enum class StartTemplateGroup {
    Working,   // starts that already do something on a server
    Blank,     // a class header and nothing in it
    ToRead,    // shipped work, opened to look at
};

struct StartTemplate {
    QString id;
    QString title;
    // One line: what the tile gives you, and what it is for. A gallery of names
    // with no answer to "which of these do I want" is a gallery nobody uses.
    QString summary;
    StartTemplateKind kind = StartTemplateKind::Script;
    StartTemplateGroup group = StartTemplateGroup::Blank;
    // Script kind: the same options the New script dialog takes, so both write
    // the same skeleton and neither can drift from the other.
    NewScriptOptions script;
    QString module;       // "4_World", "5_Mission": where the script belongs
    QString projectPath;  // Project kind: the file to open
    // Files kind ------------------------------------------------------------
    // The folder under resources/templates the .c files were found in, and
    // those files in script module order (3_Game, then 4_World, then 5_Mission),
    // which is also the order DayZ compiles them in.
    QString sourceDir;
    QStringList files;
    // What the scaffolded project name and its first script folder should be.
    QString projectName;
    // Addon ids the project declares it is written against, so the mod chain
    // and the badge on every node agree with what the code actually calls. A
    // CF template that does not put CF in the chain produces a mod that fails
    // to load with no explanation.
    QStringList dependencies;
    // Same, but marked optional with the mod's own loaded define, so the code
    // behind the #ifdef compiles either way and DZ315 has something to check.
    QStringList optionalDependencies;
    // What requiredAddons[] in config.cpp has to carry for this to load. Shown
    // when the template is started, because it is the one part of a working mod
    // that lives outside the project file.
    QStringList requiredAddons;
    // A Project or Files tile whose files are not beside the executable. Shown,
    // and refused with a reason, rather than left out of a gallery that would
    // then differ between installs for no visible cause.
    bool available = true;

    // The word at the end of the tile's title row: which module or modules the
    // template writes into, or "project" for a shipped .sdzn.
    QString kicker() const;
};

// The starter set. `resourceDir` is the resources folder, for the tiles that
// open a file shipped with the app; empty means look beside the executable.
QVector<StartTemplate> startTemplates(const QString &resourceDir = QString());

// The heading a group is drawn under, and the line under that heading.
QString startTemplateGroupTitle(StartTemplateGroup group);
QString startTemplateGroupSummary(StartTemplateGroup group);

class StartPage : public QWidget {
    Q_OBJECT
public:
    // `recent` is owned by the window and outlives the page.
    //
    // `updateSettingsPath` sends the remembered answer about update checks to
    // an ini file of the caller's choosing. Empty is the app's own settings,
    // which is what the window passes; the test passes a file of its own so a
    // run never reads or writes the user's real answer.
    explicit StartPage(RecentProjects *recent, QWidget *parent = nullptr,
                       const QString &updateSettingsPath = QString());
    ~StartPage() override;

    // Restats the recent list and redraws the rows. Only a file that has
    // actually changed is read again, so this is cheap enough to run on every
    // return to the page.
    void refresh();

    // The templates column on its own, for a screenshot of the gallery.
    QWidget *gallery() const { return m_gallery; }

    // The what is new panel on its own, for a screenshot and for a test that
    // wants to measure it rather than the whole page.
    QWidget *whatsNew() const { return m_whatsNewPanel; }

    // Where the release notes for the running version are read from. Set by the
    // constructor to CHANGELOG.md beside the executable when there is one; a
    // caller can point it somewhere else, and does when there is no install.
    void setChangelogPath(const QString &path);
    QString changelogPath() const { return m_changelogPath; }

    // Stops the page opening a socket at all, whatever the settings file it was
    // handed says. The screenshot harness sets this off, so no run of the test
    // can reach the network even if it grants consent to draw a state.
    void setAutomaticUpdateCheck(bool on);

    // Puts the panel in the state an answer from GitHub would put it in. The
    // page's own check calls this; a test calls it to draw a newer release
    // without a request having been made.
    void applyUpdateOutcome(const UpdateOutcome &outcome);

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
    QWidget *buildWhatsNewPanel();
    void rebuildRecent();
    void reloadChangelog();
    void rebuildWhatsNew();
    // The once-a-day one, from the first show. Does nothing without consent.
    void maybeStartUpdateCheck();
    // The one a button press asks for.
    void startUpdateCheck();
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

    // What is new. The page owns the check rather than being driven by the
    // window, so the whole feature is one file and adding it moved nothing in
    // the window that hosts the page.
    QWidget *m_whatsNewPanel;
    QLabel *m_versionLine;
    QLabel *m_checkStatus;    // right of the version line, dim, never an error
    QLabel *m_currentNotes;
    QLabel *m_updateHeadline;
    QLabel *m_updateNotes;
    QWidget *m_updateBlock;   // headline and notes together, hidden when there is nothing
    QPushButton *m_primary;
    QPushButton *m_secondary;
    QPushButton *m_tertiary;
    UpdatePreferences *m_prefs;
    UpdateCheck *m_check;
    UpdateOutcome m_outcome;
    QVector<ChangelogEntry> m_changelog;
    QString m_changelogPath;
    QString m_version;
    bool m_autoCheck;
    bool m_updatedSinceLastRun;  // the running version is not the one last shown
};
