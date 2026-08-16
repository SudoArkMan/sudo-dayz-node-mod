#include "modbrowser.h"

#include "document.h"
#include "modlibrary.h"
#include "theme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kFolderRole = Qt::UserRole;
constexpr int kClassRole = Qt::UserRole + 1;
// A count reads better as "22%" than as 22, but a column of "9%" and "100%"
// sorts as text into the wrong order, so the number is carried beside the text
// and the row sorts on that.
constexpr int kSortRole = Qt::UserRole + 2;

class Row : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant mine = data(column, kSortRole);
        const QVariant theirs = other.data(column, kSortRole);
        if (mine.isValid() && theirs.isValid()) return mine.toDouble() < theirs.toDouble();
        return QTreeWidgetItem::operator<(other);
    }
};

// The share of methods that became nodes, as the column shows it. A mod nobody
// has opened yet has no number, and an empty cell says that better than a zero.
void setPercent(QTreeWidgetItem *item, int column, int percent)
{
    item->setText(column, QStringLiteral("%1%").arg(percent));
    item->setData(column, kSortRole, percent);
    item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
}

// How many files the import spends per tick. The whole point of stepping is
// that the panel keeps painting, so this is small enough that one tick stays
// under a frame on the mods that lower slowly.
constexpr int kFilesPerTick = 4;

QString countLabel(int n, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

} // namespace

ModBrowserPanel::ModBrowserPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_library(new ModLibrary(this)),
      m_filter(new QLineEdit(this)), m_refresh(new QPushButton(tr("Rescan"), this)),
      m_addFolder(new QPushButton(tr("Add folder"), this)),
      m_mods(new QTreeWidget(this)), m_classes(new QTreeWidget(this)),
      m_status(new QLabel(this)), m_notes(new QPushButton(tr("Notes"), this)),
      m_openTimer(new QTimer(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(4);
    m_filter->setPlaceholderText(tr("Filter mods"));
    m_filter->setClearButtonEnabled(true);
    tools->addWidget(m_filter, 1);
    tools->addWidget(m_refresh);
    tools->addWidget(m_addFolder);
    layout->addLayout(tools);

    auto *split = new QSplitter(Qt::Vertical, this);

    m_mods->setColumnCount(4);
    m_mods->setHeaderLabels({tr("Mod"), tr("Author"), tr("Scripts"), tr("Modelled")});
    m_mods->setRootIsDecorated(false);
    m_mods->setUniformRowHeights(true);
    m_mods->setSortingEnabled(true);
    m_mods->sortByColumn(0, Qt::AscendingOrder);
    m_mods->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mods->header()->setStretchLastSection(false);
    m_mods->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_mods->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_mods->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_mods->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    // A scroll area asks for several rows and a scrollbar, and two of them
    // stacked here set the floor under the whole left column: the window cannot
    // be shorter than the sum of what its docks demand. One row and a header is
    // a floor, not a target, and the weights in the window decide the rest.
    m_mods->setMinimumHeight(44);
    split->addWidget(m_mods);

    m_classes->setColumnCount(3);
    m_classes->setHeaderLabels({tr("Class"), tr("Extends"), tr("Modelled")});
    m_classes->setRootIsDecorated(false);
    m_classes->setUniformRowHeights(true);
    m_classes->setSortingEnabled(true);
    m_classes->sortByColumn(0, Qt::AscendingOrder);
    m_classes->header()->setStretchLastSection(false);
    m_classes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_classes->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_classes->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_classes->setMinimumHeight(44);
    split->addWidget(m_classes);

    // Even. The mod list is scrolled once to pick a mod and the class list is
    // what you work in after that, so 3:2 in the mod list's favour left the
    // class list at one row on a short window while the mod list had four.
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    auto *foot = new QHBoxLayout;
    foot->setSpacing(4);
    m_status->setWordWrap(false);
    // A dock is narrow and this line is long. Without this the label demands
    // the width of its own text and drags the whole dock wider with it.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette dim = m_status->palette();
    dim.setColor(QPalette::WindowText, theme::textDim());
    m_status->setPalette(dim);
    foot->addWidget(m_status, 1);
    m_notes->setEnabled(false);
    foot->addWidget(m_notes);
    layout->addLayout(foot);

    m_openTimer->setInterval(0);
    m_openTimer->setSingleShot(false);

    connect(m_filter, &QLineEdit::textChanged, this, &ModBrowserPanel::onFilterChanged);
    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(true); });
    connect(m_addFolder, &QPushButton::clicked, this, [this] { addFolder(); });
    connect(m_notes, &QPushButton::clicked, this, &ModBrowserPanel::onShowNotes);
    connect(m_mods, &QTreeWidget::itemSelectionChanged, this, &ModBrowserPanel::onModSelected);
    connect(m_classes, &QTreeWidget::itemActivated, this, &ModBrowserPanel::onClassActivated);
    connect(m_classes, &QTreeWidget::itemDoubleClicked, this, &ModBrowserPanel::onClassActivated);
    connect(m_openTimer, &QTimer::timeout, this, &ModBrowserPanel::stepOpen);

    connect(m_library, &ModLibrary::modsChanged, this, &ModBrowserPanel::onModsChanged);
    connect(m_library, &ModLibrary::scanProgress, this, &ModBrowserPanel::onScanProgress);
    connect(m_library, &ModLibrary::scanFinished, this, &ModBrowserPanel::onScanFinished);

    // The cache is what makes opening this dock instant on the second run. A
    // scan still starts behind it, so a mod installed since last time turns up
    // without anybody having to ask for it.
    m_library->loadCache();
    rebuildModList();
    refresh(false);
}

ModBrowserPanel::~ModBrowserPanel()
{
    stopOpen();
}

// ------------------------------------------------------------------ the list

QString ModBrowserPanel::filterText() const
{
    return m_filter->text().trimmed();
}

void ModBrowserPanel::refresh(bool force)
{
    if (m_library->roots().isEmpty()) {
        setStatus(tr("No mod folder to scan. Use Add folder to point at one."));
        return;
    }
    m_library->refresh(force);
}

void ModBrowserPanel::addFolder(const QString &folder)
{
    QString path = folder;
    if (path.isEmpty())
        path = QFileDialog::getExistingDirectory(this, tr("Folder holding mods"));
    if (path.isEmpty()) return;
    if (!m_library->addRoot(path)) {
        setStatus(tr("That folder is already in the library, or is not a folder."));
        return;
    }
    refresh(false);
}

bool ModBrowserPanel::selectMod(const QString &folder)
{
    for (int i = 0; i < m_mods->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_mods->topLevelItem(i);
        if (item->data(0, kFolderRole).toString() != folder) continue;
        m_mods->setCurrentItem(item);
        m_mods->scrollToItem(item);
        // setCurrentItem fires the selection change, but only when the row was
        // not already current, so the open is started here rather than left to
        // the signal.
        if (m_openFolder != folder) {
            if (const ModEntry *entry = m_library->mod(folder)) startOpen(*entry);
        }
        return true;
    }
    return false;
}

bool ModBrowserPanel::isOpening() const
{
    return m_openTimer->isActive();
}

bool ModBrowserPanel::openClassAt(int row)
{
    if (row < 0 || row >= m_classes->topLevelItemCount()) return false;
    QTreeWidgetItem *item = m_classes->topLevelItem(row);
    m_classes->setCurrentItem(item);
    onClassActivated(item, 0);
    return true;
}

void ModBrowserPanel::onFilterChanged()
{
    rebuildModList();
}

void ModBrowserPanel::onModsChanged()
{
    rebuildModList();
}

void ModBrowserPanel::onScanProgress(int done, int total)
{
    if (total <= 0) return;
    setStatus(tr("Scanning mods, %1 of %2").arg(done).arg(total));
}

void ModBrowserPanel::onScanFinished(int found, bool cancelled)
{
    if (cancelled) {
        setStatus(tr("Scan cancelled. Showing what was already known."));
        return;
    }
    int withScripts = 0;
    for (const ModEntry &entry : m_library->mods())
        if (entry.hasScripts()) ++withScripts;
    setStatus(tr("%1 installed, %2 of them ship script")
                  .arg(countLabel(found, tr("mod"), tr("mods")))
                  .arg(withScripts));
}

void ModBrowserPanel::rebuildModList()
{
    const QSignalBlocker blocker(m_mods);
    const QString keep = m_openFolder;
    const QString filter = filterText();

    m_mods->setSortingEnabled(false);
    m_mods->clear();

    QTreeWidgetItem *restore = nullptr;
    for (const ModEntry &entry : m_library->mods()) {
        if (!filter.isEmpty() && !entry.name.contains(filter, Qt::CaseInsensitive)
            && !entry.folderName.contains(filter, Qt::CaseInsensitive)
            && !entry.author.contains(filter, Qt::CaseInsensitive))
            continue;

        auto *item = new Row(m_mods);
        item->setText(0, entry.name);
        item->setText(1, entry.author);
        item->setText(2, QString::number(entry.scriptCount()));
        item->setData(2, kSortRole, entry.scriptCount());
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, kFolderRole, entry.folder);

        const auto known = m_modelled.constFind(entry.folder);
        if (known != m_modelled.constEnd()) setPercent(item, 3, *known);

        QStringList tip;
        tip << entry.folder;
        if (!entry.overview.isEmpty()) tip << entry.overview;
        if (entry.authorIsSigner)
            tip << tr("Author taken from the key that signed the pbo; this mod ships no mod.cpp.");
        for (const ModPbo &pbo : entry.pbos)
            if (!pbo.readable && !pbo.error.isEmpty())
                tip << tr("%1: %2").arg(QFileInfo(pbo.path).fileName(), pbo.error);
        item->setToolTip(0, tip.join(QLatin1Char('\n')));

        if (!entry.hasScripts()) {
            // Still listed: a model or texture pack is a real mod and hiding it
            // would make the count disagree with the folder.
            item->setForeground(0, theme::textDim());
            item->setForeground(1, theme::textDim());
        }
        if (entry.folder == keep) restore = item;
    }

    m_mods->setSortingEnabled(true);
    if (restore) {
        const QSignalBlocker keepQuiet(m_mods);
        m_mods->setCurrentItem(restore);
    }
    if (m_mods->topLevelItemCount() == 0 && !filter.isEmpty())
        setStatus(tr("Nothing matches that filter."));
}

// ---------------------------------------------------------------- opening one

void ModBrowserPanel::onModSelected()
{
    QTreeWidgetItem *item = m_mods->currentItem();
    if (!item) return;
    const QString folder = item->data(0, kFolderRole).toString();
    if (folder.isEmpty() || folder == m_openFolder) return;

    const ModEntry *entry = m_library->mod(folder);
    if (!entry) return;
    startOpen(*entry);
}

void ModBrowserPanel::startOpen(const ModEntry &mod)
{
    stopOpen();
    m_classes->clear();
    // The notes belong to the job that was just dropped, and a button that
    // opens the previous mod's notes is worse than one that is greyed out.
    m_notes->setEnabled(false);
    m_openFolder = mod.folder;

    if (!mod.hasScripts()) {
        setStatus(tr("%1 ships no script, so there is nothing to open here.").arg(mod.name));
        emit openFinished(mod.folder, false);
        return;
    }
    if (!m_doc || !m_doc->catalog().isLoaded()) {
        setStatus(tr("The node catalogue is not loaded, so scripts cannot be read yet."));
        emit openFinished(mod.folder, false);
        return;
    }

    setStatus(tr("Reading %1...").arg(mod.name));
    // The defaults are what a person can look through. A mod that trips them
    // says so on the status line rather than quietly showing part of itself.
    ModOpenOptions opts;
    m_job = new ModOpenJob(beginOpen(mod, opts));
    if (m_job->done()) {
        stepOpen();  // finishes and reports whatever beginOpen had to say
        return;
    }
    m_openTimer->start();
}

void ModBrowserPanel::stopOpen()
{
    m_openTimer->stop();
    delete m_job;
    m_job = nullptr;
}

void ModBrowserPanel::stepOpen()
{
    if (!m_job) {
        m_openTimer->stop();
        return;
    }
    const bool more = openModStep(*m_job, m_doc->catalog(), m_doc->builtins(),
                                  m_doc->project(), kFilesPerTick);
    refreshClassList();

    const ModOpenResult &result = m_job->result();
    if (more) {
        setStatus(tr("Reading %1, %2 of %3 files")
                      .arg(result.modName)
                      .arg(m_job->filesDone())
                      .arg(m_job->filesTotal()));
        return;
    }

    m_openTimer->stop();
    m_modelled.insert(result.folder, result.modelledPercent());
    if (QTreeWidgetItem *item = m_mods->currentItem()) {
        if (item->data(0, kFolderRole).toString() == result.folder)
            setPercent(item, 3, result.modelledPercent());
    }
    m_notes->setEnabled(!result.notes.isEmpty());

    if (!result.ok) {
        setStatus(result.error);
        emit openFinished(result.folder, false);
        return;
    }

    QStringList parts;
    parts << tr("%1 of %2 files read")
                 .arg(result.filesImported)
                 .arg(result.filesExtracted);
    parts << tr("%1 of %2 methods became nodes (%3%)")
                 .arg(result.methodsAsNodes)
                 .arg(result.methodsAsNodes + result.methodsAsText)
                 .arg(result.modelledPercent());
    if (result.statementsTotal > 0)
        parts << tr("%1 of %2 statements lowered")
                     .arg(result.statementsLowered)
                     .arg(result.statementsTotal);
    if (!result.notes.isEmpty())
        parts << (result.notesDropped > 0
                      ? tr("%1 notes and %2 more").arg(result.notes.size())
                            .arg(result.notesDropped)
                      : countLabel(result.notes.size(), tr("note"), tr("notes")));
    if (result.truncated) parts << tr("stopped at the file cap");
    setStatus(parts.join(QStringLiteral(", ")));
    emit openFinished(result.folder, true);
}

void ModBrowserPanel::refreshClassList()
{
    if (!m_job) return;
    const ModOpenResult &result = m_job->result();
    if (result.classes.size() == m_classes->topLevelItemCount()) return;

    const QSignalBlocker blocker(m_classes);
    m_classes->setSortingEnabled(false);
    for (int i = m_classes->topLevelItemCount(); i < result.classes.size(); ++i) {
        const ModClassView &view = result.classes.at(i);
        auto *item = new Row(m_classes);
        item->setText(0, view.className);
        item->setText(1, view.modded ? tr("modded %1").arg(view.baseClass) : view.baseClass);
        setPercent(item, 2, view.modelledPercent());
        item->setData(0, kClassRole, i);

        QString entry = view.entry;
        entry.replace('\\', '/');
        QStringList tip;
        tip << QStringLiteral("%1/%2").arg(view.pbo, entry);
        tip << tr("%1 of %2 methods became nodes, %3 kept their text")
                   .arg(view.methodsAsNodes)
                   .arg(view.methodsAsNodes + view.methodsAsText)
                   .arg(view.methodsAsText);
        if (view.methodsEmpty > 0)
            tip << tr("%1 had no body to model").arg(view.methodsEmpty);
        tip << tr("Read only. This came out of somebody else's mod.");
        item->setToolTip(0, tip.join(QLatin1Char('\n')));
    }
    m_classes->setSortingEnabled(true);
}

void ModBrowserPanel::onClassActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || !m_job) return;
    const int index = item->data(0, kClassRole).toInt();
    const ModOpenResult &result = m_job->result();
    if (index < 0 || index >= result.classes.size()) return;
    const ModClassView &view = result.classes.at(index);
    emit graphRequested(view.className, view.graph);
}

void ModBrowserPanel::onShowNotes()
{
    if (!m_job) return;
    const ModOpenResult &result = m_job->result();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("What could not be modelled in %1").arg(result.modName));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setFont(theme::monoFont());
    QStringList lines = result.notes;
    if (result.notesDropped > 0)
        lines << tr("and %1 more, not kept: this archive refuses entries faster than "
                    "there is anything new to say about them")
                     .arg(result.notesDropped);
    text->setPlainText(lines.join(QLatin1Char('\n')));
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.resize(720, 420);
    dialog.exec();
}

void ModBrowserPanel::setStatus(const QString &text)
{
    m_statusText = text;
    m_status->setToolTip(text);
    elideStatus();
    emit statusChanged(text);
}

void ModBrowserPanel::elideStatus()
{
    const QFontMetrics metrics(m_status->font());
    m_status->setText(metrics.elidedText(m_statusText, Qt::ElideRight,
                                         qMax(40, m_status->width())));
}

void ModBrowserPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    elideStatus();
    fitColumns();
}

void ModBrowserPanel::fitColumns()
{
    // Four columns in a dock this narrow leaves the name, which is the column
    // you are reading, about thirty pixels and an ellipsis. The name and the
    // share modelled are what the list is for; the rest go when there is no
    // room for them, and the row's tooltip still carries the author.
    const int w = width();
    m_mods->setColumnHidden(1, w < 440);
    m_mods->setColumnHidden(2, w < 360);
    m_classes->setColumnHidden(1, w < 400);
}
