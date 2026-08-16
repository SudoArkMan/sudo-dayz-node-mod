#include "testpanel.h"

#include "document.h"
#include "modlibrary.h"
#include "testrun.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

// Enough log for a full binarize pass and the server's start-up spam without
// letting a session that ran all afternoon eat the process.
constexpr int kLogLines = 6000;

// What a picker row carries. The folder is what -mod= gets and the name is what
// project.cfg's Mods line gets, so both ride the row rather than being worked
// out again when the dialog is accepted.
constexpr int kNameRole = Qt::UserRole;
constexpr int kFolderRole = Qt::UserRole + 1;
constexpr int kLabelRole = Qt::UserRole + 2;

// One line naming what an entry is, for the chain line under the buttons.
QString chainMark(const ModRef &mod)
{
    if (mod.missing) return QStringLiteral(" (not installed)");
    if (mod.origin == ModOrigin::Extra)
        return mod.serverOnly ? QStringLiteral(" (added, server only)")
                              : QStringLiteral(" (added for this run)");
    if (mod.serverOnly) return QStringLiteral(" (server only)");
    return {};
}

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

    m_modsAction = new QAction(QStringLiteral("Choose mods..."), this);
    m_modsAction->setToolTip(
        QStringLiteral("Load other installed mods beside this one, for a mod "
                       "written against another mod."));
    connect(m_modsAction, &QAction::triggered, this, &TestPanel::chooseMods);

    // Four control rows, two wrapped paragraphs and a splitter add up to 224px
    // of demanded height, and this panel is tabbed behind the generated file, so
    // that number was the floor under the whole bottom dock and the canvas paid
    // it at every window size. Inside a scroll area the panel asks for what it
    // can use and scrolls for the rest, which is what lets the window be 800
    // tall at all.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    scroll->setWidget(body);
    outer->addWidget(scroll);
    // The controls plus a row of the checklist. Below this the panel scrolls.
    scroll->setMinimumHeight(132);

    auto *layout = new QVBoxLayout(body);
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

    auto *modeRow = new QHBoxLayout;
    modeRow->setSpacing(4);
    modeRow->addWidget(new QLabel(QStringLiteral("Run"), this));
    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("Offline"), int(LaunchMode::Offline));
    m_mode->addItem(QStringLiteral("Dev server"), int(LaunchMode::DevServer));
    m_mode->setToolTip(
        QStringLiteral("Offline is one process on the mission and comes up in a "
                       "fraction of the time. A dev server is the only one of "
                       "the two that is a server."));
    const int startAt = m_mode->findData(int(m_run->mode()));
    if (startAt >= 0) m_mode->setCurrentIndex(startAt);
    modeRow->addWidget(m_mode);
    modeRow->addSpacing(8);
    modeRow->addWidget(new QLabel(QStringLiteral("Mission"), this));
    m_mission = new QComboBox(this);
    // Filled by refresh from what is actually on disk. It matters to both
    // modes: offline loads it and the server is pointed at it.
    m_mission->setPlaceholderText(QStringLiteral("none found"));
    m_mission->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    modeRow->addWidget(m_mission);
    modeRow->addStretch(1);
    layout->addLayout(modeRow);

    // The chain, next to the button that changes it. Both launches load it and
    // both write it into Workbench, so it is worth reading before a run rather
    // than after one that came up without the mod it hooks.
    auto *chainRow = new QHBoxLayout;
    chainRow->setSpacing(4);
    chainRow->addWidget(buttonFor(m_modsAction, this));
    m_chainLine = new QLabel(this);
    m_chainLine->setWordWrap(true);
    chainRow->addWidget(m_chainLine, 1);
    layout->addLayout(chainRow);

    // Under the selector and across the dock, which is the widest line
    // available. This dock is wide and short, so it is height that is scarce:
    // the names of the six go here and the reasons go in the tooltip and into
    // the log when offline is chosen, rather than a paragraph taking the space
    // the checklist needs.
    m_offlineNotes = new QLabel(this);
    m_offlineNotes->setWordWrap(true);
    m_offlineNotes->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::textDim().name()));
    layout->addWidget(m_offlineNotes);

    connect(m_mode, &QComboBox::currentIndexChanged, this, &TestPanel::chooseMode);
    connect(m_mission, &QComboBox::currentIndexChanged, this,
            &TestPanel::chooseMission);

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
    // Ten rows is what the list opens at, through the splitter's sizes below,
    // and not a floor under it. This panel is tabbed behind the generated file,
    // so a floor here is a floor under the whole bottom dock and the canvas is
    // what pays for it. The floor is the header and a row: enough to say what
    // the list is, and it scrolls from there.
    m_checks->setMinimumHeight(
        m_checks->header()->sizeHint().height()
        + QFontMetrics(m_checks->font()).lineSpacing() + 6);
    split->addWidget(m_checks);

    m_log = new QPlainTextEdit(split);
    m_log->setReadOnly(true);
    m_log->setFont(theme::monoFont());
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setPlaceholderText(
        QStringLiteral("Command lines and their output land here."));
    // Two lines, for the same reason: the log is read by scrolling anyway, and
    // whatever it insists on here comes off the canvas.
    m_log->setMinimumHeight(QFontMetrics(m_log->font()).lineSpacing() * 2 + 8);
    split->addWidget(m_log);

    // The checklist leads until something has been run. The log grows into the
    // space the user gives it, which is what the splitter is for.
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 3);
    split->setSizes({160, 96});
    layout->addWidget(split, 1);

    applyMode();
}

void TestPanel::applyMode()
{
    const bool offline = m_run->mode() == LaunchMode::Offline;
    m_launchAction->setText(offline ? QStringLiteral("Launch offline")
                                    : QStringLiteral("Launch dev server"));
    m_launchAction->setToolTip(
        offline ? QStringLiteral("Start one diag process on the mission, with "
                                 "file patching on.")
                : QStringLiteral("Start the diag server, then the diag client "
                                 "connecting to it, both with file patching on."));
}

void TestPanel::chooseMode()
{
    m_run->setMode(LaunchMode(m_mode->currentData().toInt()));
    applyMode();
    // The mission row changes severity with the mode, so the checklist is read
    // again rather than left saying what the other mode would have said.
    refresh();

    // Choosing the fast loop is the moment to say what it costs, and the log is
    // where there is room to say it with the reasons attached. Once per choice,
    // not once per launch, so pressing F5 all afternoon stays quiet.
    if (m_run->mode() != LaunchMode::Offline) return;
    appendLog(QStringLiteral("--- Offline. What this run will not show:"));
    for (const OfflineLimit &limit : offlineLimits(m_run->paths().modChain))
        appendLog(QStringLiteral("  %1").arg(limit.line()));
}

void TestPanel::chooseMission()
{
    m_run->setMission(m_mission->currentData().toString());
    refresh();
}

void TestPanel::ensureLibrary()
{
    if (m_library) return;
    m_library = new ModLibrary(this);
    // Whatever the mod browser has already scanned. This instance only ever
    // reads: it does not import, it does not open a pbo's data, and it never
    // writes inside a mod folder.
    m_library->loadCache();
}

void TestPanel::chooseMods()
{
    ensureLibrary();

    // The order matters and it is the user's last word where no dependency edge
    // decides, so the picks are a list rather than a set.
    QVector<ExtraMod> picked = extraModsOf(m_doc->project());

    QString selfName;
    QStringList declared;
    for (const ModRef &mod : m_run->paths().modChain) {
        if (mod.origin == ModOrigin::Self) selfName = mod.name;
        if (mod.origin == ModOrigin::Dependency) declared << mod.name;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Mods to load alongside"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *intro = new QLabel(
        QStringLiteral("Pick the mods this one has to be tested with. A class "
                       "reopened from another mod does not exist unless that mod "
                       "is loaded, so a session without it proves nothing. The "
                       "chain is ordered from what each mod requires, and where "
                       "nothing on disk says, from the order in this list."),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const QString dim =
        QStringLiteral("color: %1;").arg(theme::textDim().name());
    auto *declaredLine = new QLabel(
        declared.isEmpty()
            ? QStringLiteral("This project declares no dependencies of its own.")
            : QStringLiteral("Declared by this project and always loaded: %1. "
                             "Those are a property of the mod and are changed "
                             "with its dependencies, not here.")
                  .arg(declared.join(QStringLiteral(", "))),
        &dialog);
    declaredLine->setWordWrap(true);
    declaredLine->setStyleSheet(dim);
    layout->addWidget(declaredLine);

    auto *filterBox = new QLineEdit(&dialog);
    filterBox->setPlaceholderText(QStringLiteral("Filter by name or folder"));
    filterBox->setClearButtonEnabled(true);
    layout->addWidget(filterBox);

    auto *tree = new QTreeWidget(&dialog);
    // Two columns, not three. A column of folders would be 254 rows of the same
    // Workshop path, and a column of folder names would repeat the mod's name
    // for all but a handful of them. The folder name only earns its place where
    // it differs, which is where it goes: "Community Framework (@CF)". The whole
    // path is on the row's tooltip.
    tree->setColumnCount(2);
    tree->setHeaderLabels({ QStringLiteral("Mod"),
                            QStringLiteral("Server only") });
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(true);
    layout->addWidget(tree, 1);

    auto *moveRow = new QHBoxLayout;
    moveRow->setSpacing(4);
    auto *up = new QToolButton(&dialog);
    up->setText(QStringLiteral("Move up"));
    auto *down = new QToolButton(&dialog);
    down->setText(QStringLiteral("Move down"));
    const QString moveTip =
        QStringLiteral("Only matters for a mod whose config is packed, where "
                       "there is nothing on disk to order it by. Anything that "
                       "declares what it needs is sorted from that instead.");
    up->setToolTip(moveTip);
    down->setToolTip(moveTip);
    moveRow->addWidget(up);
    moveRow->addWidget(down);
    moveRow->addStretch(1);
    auto *status = new QLabel(&dialog);
    status->setStyleSheet(dim);
    moveRow->addWidget(status);
    layout->addLayout(moveRow);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    const auto indexOfPick = [&picked](const QString &name) {
        for (int i = 0; i < picked.size(); ++i)
            if (picked.at(i).name.compare(name, Qt::CaseInsensitive) == 0) return i;
        return -1;
    };

    const auto applyFilter = [tree, filterBox]() {
        const QString want = filterBox->text().trimmed();
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            const bool hit =
                want.isEmpty()
                || item->text(0).contains(want, Qt::CaseInsensitive)
                || item->data(0, kNameRole).toString().contains(want, Qt::CaseInsensitive)
                || item->data(0, kFolderRole).toString().contains(want,
                                                                 Qt::CaseInsensitive);
            item->setHidden(!hit);
        }
    };

    const auto updateStatus = [&]() {
        if (m_library->isScanning()) {
            status->setText(QStringLiteral("Scanning the installed mods..."));
            return;
        }
        if (tree->topLevelItemCount() == 0) {
            status->setText(QStringLiteral("No installed mods found. Add a folder "
                                           "in the mod browser and they turn up "
                                           "here."));
            return;
        }
        status->setText(QStringLiteral("%1 installed, %2 chosen.")
                            .arg(tree->topLevelItemCount())
                            .arg(picked.size()));
    };

    // Chosen first, in chain order, then everything else by name. 254 rows in
    // one alphabetical run is not a list anybody scrolls to find the three they
    // ticked, and the top group is also the order the picks are stored in.
    std::function<void()> fill = [&]() {
        const QSignalBlocker block(tree);
        tree->clear();

        QVector<ExtraMod> rows = picked;
        QSet<QString> seen;
        for (const ExtraMod &mod : picked) seen.insert(mod.name.toLower());
        for (const ModEntry &mod : m_library->mods()) {
            if (mod.folderName.isEmpty()) continue;
            // The project's own mod is already pinned to the end of the chain,
            // and picking it here would be asking for it twice.
            if (!selfName.isEmpty()
                && mod.folderName.compare(selfName, Qt::CaseInsensitive) == 0)
                continue;
            if (seen.contains(mod.folderName.toLower())) continue;
            seen.insert(mod.folderName.toLower());
            ExtraMod row;
            row.name = mod.folderName;
            row.folder = mod.folder;
            row.label = mod.name.isEmpty() ? mod.folderName : mod.name;
            rows << row;
        }
        std::sort(rows.begin() + picked.size(), rows.end(),
                  [](const ExtraMod &a, const ExtraMod &b) {
                      return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
                  });

        for (const ExtraMod &row : rows) {
            const int at = indexOfPick(row.name);
            auto *item = new QTreeWidgetItem(tree);
            // A folder that is not there is the state that blocks a launch, so
            // it is said here, where it can be taken out again.
            const bool installed = !row.folder.isEmpty()
                                   && QFileInfo(row.folder).isDir();
            QString text = row.label;
            if (QStringLiteral("@") + row.label != row.name)
                text += QStringLiteral(" (%1)").arg(row.name);
            if (!installed) text += QStringLiteral(", not installed here");
            item->setText(0, text);
            item->setData(0, kNameRole, row.name);
            item->setData(0, kFolderRole, row.folder);
            item->setData(0, kLabelRole, row.label);
            item->setCheckState(0, at >= 0 ? Qt::Checked : Qt::Unchecked);
            item->setCheckState(1, at >= 0 && picked.at(at).serverOnly
                                       ? Qt::Checked
                                       : Qt::Unchecked);
            if (!installed)
                for (int col = 0; col < 2; ++col)
                    item->setForeground(col, theme::errorColor());
            item->setToolTip(0, QStringLiteral("%1\n%2")
                                    .arg(row.name,
                                         installed
                                             ? QDir::toNativeSeparators(row.folder)
                                             : QStringLiteral("not installed here")));
            item->setToolTip(1, QStringLiteral("Loaded by the server and not by "
                                               "the client, through -serverMod=. "
                                               "An offline run is given none of "
                                               "that chain."));
        }
        // Capped. One mod out of 254 with a very long name would otherwise push
        // its own tick box half a screen away from the name it belongs to.
        tree->resizeColumnToContents(0);
        tree->setColumnWidth(0, qMin(tree->columnWidth(0), 380));
        applyFilter();
        updateStatus();
    };

    connect(tree, &QTreeWidget::itemChanged, &dialog,
            [&](QTreeWidgetItem *item, int column) {
                if (!item) return;
                const QSignalBlocker block(tree);
                // Marking something server only is a way of choosing it, so it
                // does not have to be chosen twice.
                if (column == 1 && item->checkState(1) == Qt::Checked)
                    item->setCheckState(0, Qt::Checked);
                if (column == 0 && item->checkState(0) != Qt::Checked)
                    item->setCheckState(1, Qt::Unchecked);

                ExtraMod mod;
                mod.name = item->data(0, kNameRole).toString();
                mod.folder = item->data(0, kFolderRole).toString();
                mod.label = item->data(0, kLabelRole).toString();
                mod.serverOnly = item->checkState(1) == Qt::Checked;
                const int at = indexOfPick(mod.name);
                if (item->checkState(0) == Qt::Checked) {
                    // Kept where it already was. A row that jumped to the top
                    // under the cursor on every tick would make the order the
                    // list is for unreadable.
                    if (at >= 0) picked[at] = mod;
                    else picked.append(mod);
                } else if (at >= 0) {
                    picked.remove(at);
                }
                updateStatus();
            });

    connect(filterBox, &QLineEdit::textChanged, &dialog,
            [&](const QString &) { applyFilter(); });

    const auto move = [&](int by) {
        QTreeWidgetItem *item = tree->currentItem();
        if (!item) return;
        const int at = indexOfPick(item->data(0, kNameRole).toString());
        if (at < 0 || at + by < 0 || at + by >= picked.size()) return;
        picked.swapItemsAt(at, at + by);
        fill();
        // The row moved, so the selection follows it rather than staying on
        // whatever took its place.
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, kNameRole).toString()
                == picked.at(at + by).name)
                tree->setCurrentItem(tree->topLevelItem(i));
    };
    connect(up, &QToolButton::clicked, &dialog, [&]() { move(-1); });
    connect(down, &QToolButton::clicked, &dialog, [&]() { move(1); });

    // A scan finishing while the dialog is open fills the list rather than
    // leaving it empty until it is reopened. The connection dies with the
    // dialog, so nothing here outlives the modal loop.
    connect(m_library, &ModLibrary::modsChanged, &dialog, [&]() { fill(); });
    connect(m_library, &ModLibrary::scanFinished, &dialog,
            [&](int, bool) { updateStatus(); });

    fill();
    // Only when there is nothing to show. A cache written by the mod browser is
    // the normal case and re-reading 254 pbo headers to confirm it would be a
    // second scan for no answer.
    if (m_library->mods().isEmpty() && !m_library->isScanning()) {
        m_library->refresh();
        updateStatus();
    }

    dialog.resize(760, 520);
    if (dialog.exec() != QDialog::Accepted) return;

    setExtraMods(m_doc->project(), picked);
    // The project now holds something the .sdzn does not, which is the same
    // state as an edited graph, and this is how the rest of the app says so.
    m_doc->touchGraph();
    refresh();

    reveal();
    appendLog(QStringLiteral("--- Mods to load alongside"));
    for (const ModRef &mod : m_run->paths().modChain)
        appendLog(QStringLiteral("  %1%2  [%3]")
                      .arg(mod.name, chainMark(mod), mod.from));
    for (const QString &note : m_run->paths().chainNotes)
        appendLog(QStringLiteral("  %1").arg(note));
    const QString missingWhy = m_run->missingChainReason();
    if (!missingWhy.isEmpty()) appendLog(QStringLiteral("! %1").arg(missingWhy));
    emit statusMessage(
        picked.isEmpty()
            ? QStringLiteral("Nothing extra is loaded beside this mod.")
            : QStringLiteral("%1 loading beside this mod.")
                  .arg(picked.size() == 1
                           ? QStringLiteral("One mod")
                           : QStringLiteral("%1 mods").arg(picked.size())));
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
    const TestRunPaths &paths = m_run->paths();
    const QVector<PrereqCheck> checks = m_run->check();

    // Blocked while it is refilled: setting the index would otherwise come back
    // as a user choice and refresh again, and again.
    {
        const QSignalBlocker block(m_mission);
        m_mission->clear();
        for (const QString &path : paths.missions)
            m_mission->addItem(QFileInfo(path).fileName(), path);
        const int at = m_mission->findData(paths.mission);
        if (at >= 0) m_mission->setCurrentIndex(at);
        // One mission is not a choice, and no mission is not one either.
        m_mission->setEnabled(m_mission->count() > 1);
        m_mission->setToolTip(
            paths.mission.isEmpty()
                ? QStringLiteral("No mission under the mod folder.")
                : QStringLiteral("%1\n%2")
                      .arg(QDir::toNativeSeparators(paths.mission),
                           paths.missionFrom));
    }

    // The chain, in the order the engine will load it, because left to right is
    // the whole of what -mod= means. Each entry says whether it is the project's
    // own dependency or something added for this run, since one is a property of
    // the mod and the other of this test.
    QStringList chainParts;
    QStringList chainTips;
    for (const ModRef &mod : paths.modChain) {
        chainParts << mod.name + chainMark(mod);
        chainTips << QStringLiteral("%1\n    %2\n    what it needs: %3")
                         .arg(mod.name, mod.from,
                              mod.factsFrom.isEmpty() ? QStringLiteral("not read")
                                                      : mod.factsFrom);
    }
    const QString missingWhy = m_run->missingChainReason();
    m_chainLine->setText(
        chainParts.isEmpty()
            ? QStringLiteral("Nothing to load yet.")
            : QStringLiteral("Loads in this order: %1.")
                  .arg(chainParts.join(QStringLiteral(", "))));
    m_chainLine->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg((missingWhy.isEmpty() ? theme::textDim() : theme::errorColor())
                     .name()));
    QStringList chainTip = chainTips;
    if (!paths.chainNotes.isEmpty()) chainTip << paths.chainNotes;
    m_chainLine->setToolTip(chainTip.join(QStringLiteral("\n\n")));

    // Rebuilt every time because the last entry depends on the mod chain, and
    // the chain changes when a dependency is added. The short halves go on the
    // one line there is room for; the reasons are a hover away and go into the
    // log the moment offline is chosen, which is where they will be read.
    const QVector<OfflineLimit> limits = offlineLimits(paths.modChain);
    QStringList shortForm;
    QStringList fullForm;
    for (const OfflineLimit &limit : limits) {
        QString what = limit.what;
        if (!what.isEmpty()) what[0] = what.at(0).toLower();
        shortForm << what;
        fullForm << limit.line();
    }
    // "or" on the last one, so the line reads as a sentence rather than as a
    // list that ran out.
    if (shortForm.size() > 1)
        shortForm.last().prepend(QStringLiteral("or "));
    m_offlineNotes->setText(QStringLiteral("An offline run will not show: %1.")
                                .arg(shortForm.join(QStringLiteral(", "))));
    m_offlineNotes->setToolTip(fullForm.join(QStringLiteral("\n\n")));

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
    // The chain a running session came up with is already decided, and changing
    // it while that session is on screen would read as if it had applied to it.
    m_modsAction->setEnabled(!busy);
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
    const bool offline = m_run->mode() == LaunchMode::Offline;
    appendLog(offline ? QStringLiteral("--- Launch offline")
                      : QStringLiteral("--- Launch dev server"));
    QString error;
    if (!m_run->startTest(&error)) {
        appendLog(QStringLiteral("! %1").arg(error));
        emit statusMessage(error);
        return;
    }
    emit statusMessage(offline
                           ? QStringLiteral("Offline session starting.")
                           : QStringLiteral("Server starting, client to follow."));
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
