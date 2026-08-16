// Test: the dock that runs the mod.
//
// Three bands, in the order the questions get asked. What is not ready, and
// why. What to press. What happened when you pressed it.
//
// The checklist is the important half. Every reason a build or a launch cannot
// work is a stat call away, so there is no excuse for finding out from a game
// that closed itself, and each row carries the fix rather than only the fault.
//
// The log shows the command line before its output, always, because the first
// thing anyone asks about a failed build is what was actually run.
//
// All the work is in TestRun. This file is a view over it: no paths are built
// here and no process is started here.
#pragma once

#include <QWidget>

class Document;
class TestRun;
struct RunStep;

class QAction;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;

class TestPanel : public QWidget {
    Q_OBJECT
public:
    explicit TestPanel(Document *doc, QWidget *parent = nullptr);

    // The main window puts these on its Test menu, and the panel's own buttons
    // are built on the same objects. One place decides what is allowed, so a
    // menu entry can never offer what a button has greyed out.
    QAction *linkAction() const { return m_linkAction; }
    QAction *buildAction() const { return m_buildAction; }
    QAction *launchAction() const { return m_launchAction; }
    QAction *stopAction() const { return m_stopAction; }
    QAction *recheckAction() const { return m_recheckAction; }
    QAction *toolsAction() const { return m_toolsAction; }

public slots:
    // Re-reads the project and the machine and repaints the checklist. Called
    // whenever the mod folder can have changed, and by the Re-check button.
    void refresh();

signals:
    // For the main window's status line, so a failure is visible with the dock
    // closed.
    void statusMessage(const QString &text);

private slots:
    void setUpWorkDrive();
    void buildPbo();
    void launchTest();
    void stopTest();
    void chooseToolsFolder();

private:
    void buildUi();
    // Brings this dock to the front when it is tabbed behind another. Pressing
    // a shortcut should put the log where the answer will appear.
    void reveal();
    void appendLog(const QString &line);
    // Command first, then output, then the verdict. Same shape for every step,
    // so a log reads the same whichever button produced it.
    void report(const RunStep &step);
    void updateEnabled();

    Document *m_doc;
    TestRun *m_run;
    QTreeWidget *m_checks;
    QLabel *m_summary;
    QPlainTextEdit *m_log;
    QCheckBox *m_clean;
    QAction *m_linkAction;
    QAction *m_buildAction;
    QAction *m_launchAction;
    QAction *m_stopAction;
    QAction *m_recheckAction;
    QAction *m_toolsAction;
};
