#include "testpanel.h"

#include "document.h"
#include "testrun.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

// Enough log for a full binarize pass and the server's start-up spam without
// letting a session that ran all afternoon eat the process.
constexpr int kLogLines = 6000;

QString stateWord(PrereqState state)
{
    switch (state) {
    case PrereqState::Ok: return QStringLiteral("ok");
    case PrereqState::Warning: return QStringLiteral("check");
    case PrereqState::Missing: break;
    }
    return QStringLiteral("missing");
}

QColor stateColor(PrereqState state)
{
    switch (state) {
    case PrereqState::Ok: return theme::text();
    case PrereqState::Warning: return theme::warningColor();
    case PrereqState::Missing: break;
    }
    return theme::errorColor();
}

QToolButton *buttonFor(QAction *action, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // The action carries the enabled state, so the button follows the same
    // rules the menu entry does without either side repeating them.
    button->setDefaultAction(action);
    return button;
}

} // namespace

TestPanel::TestPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_run(new TestRun(this))
{
    buildUi();

    connect(m_run, &TestRun::log, this, &TestPanel::appendLog);
    connect(m_run, &TestRun::busyChanged, this, [this]() {
        updateEnabled();
        // A finished build has left a PBO on disk, and the checklist is the
        // only thing that says so.
        refresh();
    });
    connect(m_doc, &Document::projectChanged, this, &TestPanel::refresh);

    refresh();
}

void TestPanel::buildUi()
{
    m_linkAction = new QAction(QStringLiteral("Set up work drive"), this);
    m_linkAction->setToolTip(
        QStringLiteral("Junction the mod folder to P: so binarize and Workbench "
                       "can resolve its paths."));
    m_linkAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(m_linkAction, &QAction::triggered, this, &TestPanel::setUpWorkDrive);

    m_buildAction = new QAction(QStringLiteral("Build PBO"), this);
    m_buildAction->setToolTip(
        QStringLiteral("Pack the mod into P:\\Mods and write the mod chain into "
                       "the Workbench project."));
    m_buildAction->setShortcut(QKeySequence(Qt::Key_F9));
    connect(m_buildAction, &QAction::triggered, this, &TestPanel::buildPbo);

    m_launchAction = new QAction(QStringLiteral("Launch test"), this);
    m_launchAction->setToolTip(
        QStringLiteral("Start the diag server, then the diag client connecting "
                       "to it, both with file patching on."));
    m_launchAction->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_launchAction, &QAction::triggered, this, &TestPanel::launchTest);

    m_stopAction = new QAction(QStringLiteral("Stop"), this);
    m_stopAction->setToolTip(
        QStringLiteral("Kill what this app started, server first."));
    m_stopAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    connect(m_stopAction, &QAction::triggered, this, &TestPanel::stopTest);

    m_recheckAction = new QAction(QStringLiteral("Re-check"), this);
    m_recheckAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F5));
    connect(m_recheckAction, &QAction::triggered, this, &TestPanel::refresh);

    m_toolsAction = new QAction(QStringLiteral("Set DayZ Tools folder..."), this);
    connect(m_toolsAction, &QAction::triggered, this, &TestPanel::chooseToolsFolder);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto *runRow = new QHBoxLayout;
    runRow->setSpacing(4);
    runRow->addWidget(buttonFor(m_linkAction, this));
    runRow->addWidget(buttonFor(m_buildAction, this));
    runRow->addWidget(buttonFor(m_launchAction, this));
    runRow->addWidget(buttonFor(m_stopAction, this));
    runRow->addStretch(1);
    layout->addLayout(runRow);

    auto *optionRow = new QHBoxLayout;
    optionRow->setSpacing(4);
    m_clean = new QCheckBox(QStringLiteral("Clean build"), this);
    m_clean->setToolTip(
        QStringLiteral("Wipe the Addons folder before packing. Worth it when a "
                       "renamed file is still turning up in game."));
    optionRow->addWidget(m_clean);
    optionRow->addStretch(1);
    optionRow->addWidget(buttonFor(m_recheckAction, this));
    optionRow->addWidget(buttonFor(m_toolsAction, this));
    layout->addLayout(optionRow);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *split = new QSplitter(Qt::Vertical, this);

    m_checks = new QTreeWidget(split);
    m_checks->setColumnCount(3);
    m_checks->setHeaderLabels({ QStringLiteral("State"), QStringLiteral("Check"),
                                QStringLiteral("Detail") });
    m_checks->setRootIsDecorated(false);
    m_checks->setUniformRowHeights(true);
    m_checks->setSelectionMode(QAbstractItemView::SingleSelection);
    m_checks->header()->setStretchLastSection(true);
    // Ten rows without a scroll. The whole value of the list is being able to
    // see at a glance which one is red.
    m_checks->setMinimumHeight(160);
    split->addWidget(m_checks);

    m_log = new QPlainTextEdit(split);
    m_log->setReadOnly(true);
    m_log->setFont(theme::monoFont());
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setPlaceholderText(
        QStringLiteral("Command lines and their output land here."));
    split->addWidget(m_log);

    // The checklist leads until something has been run. The log grows into the
    // space the user gives it, which is what the splitter is for.
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);
}

void TestPanel::reveal()
{
    auto *dock = qobject_cast<QDockWidget *>(parentWidget());
    // A dock closed from the View menu stays closed. Reopening it would take
    // the canvas space back from under the user.
    if (!dock || !dock->toggleViewAction()->isChecked()) return;
    dock->raise();
}

void TestPanel::refresh()
{
    m_run->refresh(m_doc->project());
    const QVector<PrereqCheck> checks = m_run->check();

    m_checks->clear();
    int missing = 0;
    int warnings = 0;
    for (const PrereqCheck &c : checks) {
        if (c.state == PrereqState::Missing) ++missing;
        if (c.state == PrereqState::Warning) ++warnings;

        auto *item = new QTreeWidgetItem(m_checks);
        item->setText(0, stateWord(c.state));
        item->setText(1, c.label);
        item->setText(2, c.detail);
        const QColor colour = stateColor(c.state);
        for (int col = 0; col < 3; ++col) item->setForeground(col, colour);
        const QString tip = c.fix.isEmpty()
                                ? c.detail
                                : QStringLiteral("%1\n\n%2").arg(c.detail, c.fix);
        for (int col = 0; col < 3; ++col) item->setToolTip(col, tip);
    }
    m_checks->resizeColumnToContents(0);
    m_checks->resizeColumnToContents(1);

    if (missing == 0 && warnings == 0)
        m_summary->setText(QStringLiteral("Ready to build and run."));
    else if (missing == 0)
        m_summary->setText(QStringLiteral("Ready, with %1 to look at.")
                               .arg(warnings == 1 ? QStringLiteral("one thing")
                                                  : QStringLiteral("%1 things")
                                                        .arg(warnings)));
    else
        m_summary->setText(QStringLiteral("%1 of %2 checks are not satisfied. Hover "
                                          "a row for what to do about it.")
                               .arg(missing).arg(checks.size()));
    m_summary->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg((missing > 0 ? theme::errorColor() : theme::textDim()).name()));

    updateEnabled();
}

void TestPanel::updateEnabled()
{
    const bool busy = m_run->isBusy();
    const bool haveMod = !m_run->paths().modFolder.isEmpty();
    m_linkAction->setEnabled(!busy && haveMod);
    m_buildAction->setEnabled(!busy && haveMod);
    m_launchAction->setEnabled(!m_run->isTestRunning() && !m_run->isBuilding()
                               && haveMod);
    m_stopAction->setEnabled(busy);
    m_recheckAction->setEnabled(true);
    m_clean->setEnabled(!busy);
}

void TestPanel::appendLog(const QString &line)
{
    m_log->appendPlainText(line);
}

void TestPanel::report(const RunStep &step)
{
    if (step.command.isValid()) appendLog(QStringLiteral("> %1").arg(step.command.display()));
    if (!step.output.trimmed().isEmpty())
        for (const QString &line : step.output.split(QLatin1Char('\n')))
            appendLog(QStringLiteral("  %1").arg(line.trimmed()));
    const QString verdict = step.detail.isEmpty()
                                ? step.title
                                : QStringLiteral("%1: %2").arg(step.title, step.detail);
    appendLog(step.ok ? verdict : QStringLiteral("! %1").arg(verdict));
}

void TestPanel::setUpWorkDrive()
{
    reveal();
    appendLog(QStringLiteral("--- Set up work drive"));
    int failed = 0;
    const QVector<RunStep> steps = m_run->linkWorkDrive();
    for (const RunStep &step : steps) {
        report(step);
        if (!step.ok) ++failed;
    }
    refresh();
    emit statusMessage(failed == 0
                           ? QStringLiteral("Work drive is set up.")
                           : QStringLiteral("%1 of %2 links failed. See the Test dock.")
                                 .arg(failed).arg(steps.size()));
}

void TestPanel::buildPbo()
{
    reveal();
    appendLog(QStringLiteral("--- Build PBO"));

    // The chain is written first because the build is the point where the
    // project's dependencies become something the engine can load, and a stale
    // Mods line is invisible until a test session comes up without the mod.
    const RunStep chain = m_run->writeModChain();
    report(chain);

    QString error;
    if (!m_run->startBuild(m_clean->isChecked(), &error)) {
        appendLog(QStringLiteral("! %1").arg(error));
        emit statusMessage(error);
        return;
    }
    emit statusMessage(QStringLiteral("Building the PBO."));
}

void TestPanel::launchTest()
{
    reveal();
    appendLog(QStringLiteral("--- Launch test"));
    QString error;
    if (!m_run->startTest(&error)) {
        appendLog(QStringLiteral("! %1").arg(error));
        emit statusMessage(error);
        return;
    }
    emit statusMessage(QStringLiteral("Server starting, client to follow."));
}

void TestPanel::stopTest()
{
    reveal();
    appendLog(QStringLiteral("--- Stop"));
    for (const RunStep &step : m_run->stop()) report(step);
    emit statusMessage(QStringLiteral("Stopped."));
}

void TestPanel::chooseToolsFolder()
{
    const QString current = m_run->paths().dayzTools;
    const QString picked = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Where DayZ Tools is installed"), current);
    if (picked.isEmpty()) return;

    const QString builder =
        QDir(picked).filePath(QStringLiteral("Bin/AddonBuilder/AddonBuilder.exe"));
    if (!QFileInfo::exists(builder)) {
        appendLog(QStringLiteral("! No Bin\\AddonBuilder\\AddonBuilder.exe under %1. "
                                 "Pick the DayZ Tools folder itself.")
                      .arg(QDir::toNativeSeparators(picked)));
        emit statusMessage(QStringLiteral("That folder has no AddonBuilder in it."));
        return;
    }
    TestRun::setDayZToolsPath(picked);
    appendLog(QStringLiteral("DayZ Tools set to %1")
                  .arg(QDir::toNativeSeparators(picked)));
    refresh();
}
