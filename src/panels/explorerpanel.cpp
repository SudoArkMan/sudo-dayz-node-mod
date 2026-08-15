// Mod Explorer: the files on disk behind the project.
//
// QFileSystemModel reads a folder on its own thread and only when something
// asks for it, so nothing here walks the tree. The one place that has to reach
// past what the user has opened is the filter: a name cannot match before the
// model has read it, so a live filter pulls folders in one level at a time down
// to kFilterDepth and stops.
#include "explorerpanel.h"

#include "document.h"
#include "theme.h"
#include "widgets/newscriptdialog.h"

#include <QAbstractFileIconProvider>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// A mod bottoms out four folders below the root (Missions/<Mod>.ChernarusPlus/
// db/types.xml), so this covers the shape with room to spare while keeping a
// filter from reading a whole drive.
constexpr int kFilterDepth = 6;

// The panel's members are fixed by the header, so the empty state note is a
// child looked up by name when the root changes.
const char kNoteName[] = "explorerNote";

// What the app can put in front of the user as text. Everything else is some
// other tool's format and goes to the shell rather than to the code editor.
bool isEditableText(const QString &path)
{
    static const QStringList kText = {
        QStringLiteral("c"),      QStringLiteral("cpp"),  QStringLiteral("h"),
        QStringLiteral("xml"),    QStringLiteral("json"), QStringLiteral("csv"),
        QStringLiteral("cfg"),    QStringLiteral("txt"),  QStringLiteral("md"),
        QStringLiteral("lst"),    QStringLiteral("ps1"),  QStringLiteral("bat"),
        QStringLiteral("layout"), QStringLiteral("rvmat"),
    };
    return kText.contains(QFileInfo(path).suffix().toLower());
}

// Enforce Script. A .c is the one file type this app has a better answer for
// than a text box, so it is routed apart from the rest.
bool isEnforceScript(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("c"), Qt::CaseInsensitive) == 0;
}

// A .cpp under a mod folder is a config: nested classes, properties and arrays,
// never C++. config.cpp is the one every mod has, but the same shape turns up in
// a mod.cpp or a config split per addon, and all of them read better as a tree
// than as braces.
bool isModConfig(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("cpp"), Qt::CaseInsensitive) == 0;
}

// The shell icons QFileSystemModel uses by default are drawn for a light
// explorer window and can stall on a network path. The style's own pair sits
// with the docks and costs nothing. Not a QObject, and the model does not take
// ownership, so callers keep it in a function local static.
class StyleIcons : public QAbstractFileIconProvider {
public:
    QIcon icon(IconType type) const override
    {
        return standard(type == Folder || type == Drive || type == Computer);
    }
    QIcon icon(const QFileInfo &info) const override { return standard(info.isDir()); }

private:
    static QIcon standard(bool folder)
    {
        QStyle *style = QApplication::style();
        if (!style) return {};
        return style->standardIcon(folder ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
    }
};

// The stylesheet gives QTreeView::branch a background, which is enough to make
// Qt draw the branch column itself and drop the style's expand arrow with it.
// The palette never noticed because it opens every category, but a folder that
// gives no sign it can be opened is a folder nobody opens, so this tree draws
// the arrow back. Painted rather than shipped as an icon, in theme colours.
class FileTree : public QTreeView {
public:
    using QTreeView::QTreeView;

protected:
    void drawBranches(QPainter *painter, const QRect &rect,
                      const QModelIndex &index) const override
    {
        QTreeView::drawBranches(painter, rect, index);
        const QAbstractItemModel *source = model();
        if (!source || !source->hasChildren(index)) return;

        // The arrow belongs in the indent step directly left of the name.
        const QRect cell(rect.right() - indentation() + 1, rect.top(), indentation(),
                         rect.height());
        const QPointF centre(cell.center().x() + 0.5, cell.center().y() + 0.5);
        constexpr qreal arm = 3.5;

        QPolygonF arrow;
        if (isExpanded(index))
            arrow << QPointF(centre.x() - arm, centre.y() - arm * 0.6)
                  << QPointF(centre.x() + arm, centre.y() - arm * 0.6)
                  << QPointF(centre.x(), centre.y() + arm * 0.8);
        else
            arrow << QPointF(centre.x() - arm * 0.6, centre.y() - arm)
                  << QPointF(centre.x() - arm * 0.6, centre.y() + arm)
                  << QPointF(centre.x() + arm * 0.8, centre.y());

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::textDim());
        painter->drawPolygon(arrow);
        painter->restore();
    }
};

// Name filter over the mod folder, with folders sorted ahead of files: the tree
// is read as "which folder do I want" long before it is read as a list of
// names.
class FileFilterProxy : public QSortFilterProxyModel {
public:
    explicit FileFilterProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent)
    {
        // A folder survives when something inside it matches, which is what
        // lets a hit four levels down still be reachable.
        setRecursiveFilteringEnabled(true);
    }

    void setRoot(const QModelIndex &sourceRoot)
    {
        beginFilterChange();
        m_root = sourceRoot;
        endFilterChange(Direction::Rows);
    }

    void setPattern(const QString &pattern)
    {
        beginFilterChange();
        m_pattern = pattern.trimmed();
        endFilterChange(Direction::Rows);
    }

    // Folders arriving from the model change the answer for the folders above
    // them, and an insertion alone does not make the proxy ask again.
    void refilter()
    {
        beginFilterChange();
        endFilterChange(Direction::Rows);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        QAbstractItemModel *src = sourceModel();
        // With no root there is nothing to show, and accepting rows here would
        // put whatever the model happens to point at on screen.
        if (!src || !m_root.isValid()) return false;

        const QModelIndex idx = src->index(row, 0, parent);
        if (!idx.isValid()) return false;

        // The root and the folders above it are the view's own lineage. Reject
        // one of them and the tree loses its root index and falls back to
        // showing the drive.
        if (isRootLineage(idx)) return true;

        const int depth = depthBelowRoot(idx);
        if (depth < 0) return false;
        if (m_pattern.isEmpty()) return true;
        if (matches(idx)) return true;

        // A folder that matched on its own name keeps its contents: filtering
        // for "db" and opening an empty db folder helps nobody.
        for (QModelIndex up = parent; up.isValid() && up != m_root; up = up.parent())
            if (matches(up)) return true;

        // A folder the model has not read yet cannot match, because nothing
        // inside it has a name here. It stays visible so the view expands it,
        // which is what makes the model read it, and the pass after that judges
        // it on what is actually inside. The isDir test is not decoration:
        // QFileSystemModel::canFetchMore answers for a plain file as well, and
        // without it every file in the mod would be treated as a folder waiting
        // to be read and would sit in the results.
        const QFileSystemModel *fs = files();
        if (depth < kFilterDepth && fs && fs->isDir(idx) && src->canFetchMore(idx))
            return true;

        // A match further down is the recursive filter's business.
        return false;
    }

    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override
    {
        if (const QFileSystemModel *fs = files()) {
            const bool aDir = fs->isDir(a);
            if (aDir != fs->isDir(b)) return aDir;
        }
        return QString::compare(a.data(Qt::DisplayRole).toString(),
                                b.data(Qt::DisplayRole).toString(),
                                Qt::CaseInsensitive) < 0;
    }

private:
    const QFileSystemModel *files() const
    {
        return qobject_cast<const QFileSystemModel *>(sourceModel());
    }

    bool matches(const QModelIndex &idx) const
    {
        return idx.data(Qt::DisplayRole).toString().contains(m_pattern, Qt::CaseInsensitive);
    }

    bool isRootLineage(const QModelIndex &idx) const
    {
        for (QModelIndex up = m_root; up.isValid(); up = up.parent())
            if (up == idx) return true;
        return false;
    }

    // Steps from the root down to idx, or -1 when idx is not under the root.
    int depthBelowRoot(const QModelIndex &idx) const
    {
        int depth = 0;
        for (QModelIndex up = idx; up.isValid(); up = up.parent(), ++depth)
            if (up == m_root) return depth;
        return -1;
    }

    QPersistentModelIndex m_root;
    QString m_pattern;
};

// The folder a new file or folder belongs in when `path` was the target.
QString folderOf(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

// A usable name, or an empty string when the user cancelled or typed something
// the folder cannot take. Free functions have no tr() context of their own.
QString askForName(QWidget *parent, const QString &title, const QString &label,
                   const QString &initial, const QDir &dir)
{
    bool accepted = false;
    const QString typed = QInputDialog::getText(parent, title, label, QLineEdit::Normal,
                                                initial, &accepted);
    if (!accepted) return {};

    const QString name = typed.trimmed();
    if (name.isEmpty() || name == initial) return {};
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(parent, title,
                             QStringLiteral("A name cannot contain a path separator."));
        return {};
    }
    // Windows compares names without case, so changing readme.md to README.md
    // would otherwise be refused for clashing with itself.
    if (name.compare(initial, Qt::CaseInsensitive) != 0 && dir.exists(name)) {
        QMessageBox::warning(
            parent, title,
            QStringLiteral("%1 already exists in %2.").arg(name, dir.dirName()));
        return {};
    }
    return name;
}

// Rename and delete both say what went wrong. A context menu entry that does
// nothing at all reads as a frozen panel.
void renamePath(QWidget *parent, const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) return;

    const QString title = QStringLiteral("Rename");
    QDir dir = info.absoluteDir();
    const QString name = askForName(parent, title, QStringLiteral("New name"),
                                    info.fileName(), dir);
    if (name.isEmpty()) return;

    if (!dir.rename(info.fileName(), name))
        QMessageBox::warning(parent, title,
                             QStringLiteral("Could not rename %1. It may be open in "
                                            "another program, or read only.")
                                 .arg(info.fileName()));
}

void deletePath(QWidget *parent, const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) return;

    const QString title = QStringLiteral("Delete");
    const bool folder = info.isDir();
    const QString question =
        folder ? QStringLiteral("Delete the folder %1 and everything in it?")
               : QStringLiteral("Delete %1?");
    if (QMessageBox::question(parent, title, question.arg(info.fileName()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    const QString target = info.absoluteFilePath();
    // The recycle bin where the platform offers one, because a mod folder is
    // hours of work and this menu sits one row away from Open.
    bool done = QFile::moveToTrash(target);
    if (!done) done = folder ? QDir(target).removeRecursively() : QFile::remove(target);
    if (!done)
        QMessageBox::warning(parent, title,
                             QStringLiteral("Could not delete %1. It may be open in "
                                            "another program, or read only.")
                                 .arg(info.fileName()));
}

} // namespace

ExplorerPanel::ExplorerPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_model(new QFileSystemModel(this)),
      m_proxy(new FileFilterProxy(this)), m_tree(new FileTree(this)),
      m_filter(new QLineEdit(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour.
    setAttribute(Qt::WA_StyledBackground, true);

    static StyleIcons icons;
    m_model->setIconProvider(&icons);
    m_model->setReadOnly(true);
    m_proxy->setSourceModel(m_model);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    m_filter->setPlaceholderText(tr("Filter files"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);

    auto *note = new QLabel(this);
    note->setObjectName(QLatin1String(kNoteName));
    note->setWordWrap(true);
    note->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    note->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
    layout->addWidget(note, 1);

    m_tree->setModel(m_proxy);
    // One column of name. A dock is never wide enough for size, type and date,
    // and the header above them only eats another row.
    m_tree->setHeaderHidden(true);
    for (int column = 1; column < m_model->columnCount(); ++column)
        m_tree->setColumnHidden(column, true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    // A mission folder is four levels down. At the style's own indent the names
    // down there spend half a dock on white space.
    m_tree->setIndentation(14);
    // Renaming is a menu entry with its own checks, so a stray double click
    // cannot start one in place.
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_proxy->sort(0, Qt::AscendingOrder);
    layout->addWidget(m_tree, 1);

    // activated covers the double click and the Return key together. Fusion
    // does not activate on a single click, so this stays a deliberate gesture.
    connect(m_tree, &QTreeView::activated, this, &ExplorerPanel::onActivated);
    // QTreeView::activated wants a double click on Windows, and a single click
    // on a file here reads as "open it". onActivated already ignores folders,
    // so the usual expand behaviour is untouched.
    connect(m_tree, &QTreeView::clicked, this, &ExplorerPanel::onActivated);
    connect(m_tree, &QTreeView::customContextMenuRequested, this,
            &ExplorerPanel::onContextMenu);
    connect(m_filter, &QLineEdit::textChanged, this, &ExplorerPanel::onFilterChanged);

    // Folders land from the model's own thread, a few milliseconds apart. One
    // pass per arrival would refilter the tree dozens of times over a mod, so
    // the arrivals are collected and answered once they stop.
    auto *settle = new QTimer(this);
    settle->setSingleShot(true);
    settle->setInterval(40);
    connect(settle, &QTimer::timeout, this, [this]() {
        if (m_root.isEmpty() || m_filter->text().trimmed().isEmpty()) return;
        static_cast<FileFilterProxy *>(m_proxy)->refilter();
        m_tree->expandRecursively(m_tree->rootIndex(), kFilterDepth);
    });
    connect(m_model, &QFileSystemModel::directoryLoaded, this,
            [this, settle](const QString &) {
                if (m_root.isEmpty() || m_filter->text().trimmed().isEmpty()) return;
                settle->start();
            });

    setModRoot(QString());
}

void ExplorerPanel::setModRoot(const QString &path)
{
    const QString clean = path.isEmpty() ? QString() : QDir::cleanPath(path);
    const bool usable = !clean.isEmpty() && QFileInfo(clean).isDir();

    // The main window re-syncs the root on every project change, and a script
    // being added is not a reason to collapse a tree the user just opened.
    if (usable && clean == m_root && m_tree->rootIndex().isValid()) return;

    m_root = clean;
    auto *proxy = static_cast<FileFilterProxy *>(m_proxy);

    if (!usable) {
        if (QLabel *note = findChild<QLabel *>(QLatin1String(kNoteName))) {
            note->setText(
                clean.isEmpty()
                    ? tr("This project has no mod folder. Use File > New mod to "
                         "scaffold one.")
                    : tr("The mod folder is not there any more:\n\n%1")
                          .arg(QDir::toNativeSeparators(clean)));
            note->setVisible(true);
        }
        m_filter->setVisible(false);
        m_tree->setVisible(false);
        // Rooted nowhere and filtered to nothing, so a tree from the last
        // project cannot linger behind the note.
        proxy->setRoot(QModelIndex());
        m_tree->setRootIndex(QModelIndex());
        m_model->setRootPath(QString());
        return;
    }

    const QModelIndex sourceRoot = m_model->setRootPath(clean);
    proxy->setRoot(sourceRoot);
    const QModelIndex proxyRoot = m_proxy->mapFromSource(sourceRoot);
    if (!proxyRoot.isValid()) {
        // Never leave the view without a root: it would answer by showing the
        // drive the mod happens to live on.
        m_root.clear();
        setModRoot(QString());
        return;
    }

    m_tree->setRootIndex(proxyRoot);
    m_tree->collapseAll();
    m_tree->scrollToTop();
    if (QLabel *note = findChild<QLabel *>(QLatin1String(kNoteName)))
        note->setVisible(false);
    m_filter->setVisible(true);
    m_tree->setVisible(true);
    onFilterChanged(m_filter->text());
}

void ExplorerPanel::onActivated(const QModelIndex &index)
{
    if (!index.isValid()) return;
    const QModelIndex source = m_proxy->mapToSource(index);
    if (!source.isValid()) return;

    // The view already opens and closes a folder on a double click. Toggling it
    // here as well would undo that in the same gesture.
    if (m_model->isDir(source)) return;

    const QString path = m_model->filePath(source);
    if (QFileInfo(path).suffix().compare(QLatin1String("sdzn"), Qt::CaseInsensitive) == 0) {
        emit projectActivated(path);
        return;
    }
    if (isEnforceScript(path)) {
        emit scriptActivated(path);
        return;
    }
    if (isModConfig(path)) {
        emit configActivated(path);
        return;
    }
    if (isEditableText(path)) {
        emit fileActivated(path);
        return;
    }
    // A .paa, a .p3d, a .pbo: the user's own tool knows what to do with it.
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ExplorerPanel::onContextMenu(const QPoint &pos)
{
    if (m_root.isEmpty()) return;

    const QModelIndex index = m_tree->indexAt(pos);
    if (index.isValid()) m_tree->setCurrentIndex(index);
    // Empty space below the last row still has a target: the mod folder itself.
    const QString path = index.isValid() ? selectedPath() : m_root;
    if (path.isEmpty()) return;

    const QFileInfo info(path);
    const QString folder = folderOf(path);

    QMenu menu(this);
    if (index.isValid()) {
        if (!info.isDir()) {
            // Re-read at trigger time: the index the menu was opened on is not
            // worth carrying across a modal.
            menu.addAction(tr("Open"), this,
                           [this]() { onActivated(m_tree->currentIndex()); });
            // Opening a script gives you the graph, and the graph cannot show
            // everything a file can hold: a #ifdef, a stray brace, whatever the
            // importer had to keep as text. The config tree has the same limit
            // for the same reason. This is the way back to the file.
            if (isEnforceScript(path) || isModConfig(path))
                menu.addAction(tr("Edit as text"), this,
                               [this, path]() { emit fileActivated(path); });
        }
        menu.addAction(tr("Reveal in Explorer"), this,
                       [this, path]() { revealInFileManager(path); });
        menu.addAction(tr("Copy path"), this, [path]() {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path));
        });
        menu.addSeparator();
        menu.addAction(tr("Rename..."), this, [this, path]() { renamePath(this, path); });
        menu.addAction(tr("Delete"), this, [this, path]() { deletePath(this, path); });
        menu.addSeparator();
    }

    // Above New file because it is the one that gets used: a .c in a mod folder
    // starts the same way every time, and the empty page is the exception.
    menu.addAction(tr("New script..."), this, [this, folder]() {
        const Catalog *catalog = m_doc ? &m_doc->catalog() : nullptr;
        const QString path = NewScriptDialog::run(this, folder, catalog);
        // The skeleton is a class with a method in it, so it opens the way any
        // other script does: as the graph you are about to build on.
        if (!path.isEmpty()) emit scriptActivated(path);
    });

    menu.addAction(tr("New file..."), this, [this, folder]() {
        const QDir dir(folder);
        const QString name = askForName(this, tr("New file"), tr("File name"),
                                        QString(), dir);
        if (name.isEmpty()) return;
        const QString target = dir.absoluteFilePath(name);
        QFile file(target);
        // NewOnly rather than WriteOnly: a name that appeared between the
        // dialog and here must not be truncated to make room.
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            QMessageBox::warning(this, tr("New file"),
                                 tr("Could not create %1 in %2.").arg(name, folder));
            return;
        }
        file.close();
        // A file is made to be written in, so the one the app can edit opens.
        // Even an empty .c goes to the editor rather than to the importer:
        // there is no class in it yet to draw, and New script is the entry
        // point for one that has.
        if (isEditableText(target)) emit fileActivated(target);
    });

    menu.addAction(tr("New folder..."), this, [this, folder]() {
        QDir dir(folder);
        const QString name = askForName(this, tr("New folder"), tr("Folder name"),
                                        QString(), dir);
        if (name.isEmpty()) return;
        if (!dir.mkdir(name))
            QMessageBox::warning(this, tr("New folder"),
                                 tr("Could not create %1 in %2.").arg(name, folder));
    });

    menu.addSeparator();
    menu.addAction(tr("Refresh"), this, [this]() {
        // A watcher keeps the tree current for anything done on this machine.
        // Refresh is for the rest: a network drive, an archive unpacked by
        // another tool. QFileSystemModel only re-reads a folder once its root
        // path has moved off it, so the way to ask is to leave and come back.
        const QString root = m_root;
        m_model->setRootPath(QString());
        m_root.clear();
        setModRoot(root);
    });

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void ExplorerPanel::onFilterChanged(const QString &text)
{
    static_cast<FileFilterProxy *>(m_proxy)->setPattern(text);
    if (m_root.isEmpty()) return;

    if (text.trimmed().isEmpty()) {
        m_tree->collapseAll();
        return;
    }
    // Expanding is what makes the model read the level below, so this both
    // shows the matches and feeds the filter the folders it has not seen. The
    // folders that arrive from it come back through directoryLoaded.
    m_tree->expandRecursively(m_tree->rootIndex(), kFilterDepth);
}

void ExplorerPanel::revealInFileManager(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) return;

#ifdef Q_OS_WIN
    // Explorer wants the path inside the argument, quoted there. Passing
    // "/select," and the path as an argument list would have QProcess quote the
    // pair as one token and Explorer would open the wrong folder.
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"")
                                    .arg(QDir::toNativeSeparators(info.absoluteFilePath())));
    if (explorer.startDetached()) return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(folderOf(info.absoluteFilePath())));
}

QString ExplorerPanel::selectedPath() const
{
    const QModelIndex index = m_tree->currentIndex();
    if (!index.isValid()) return {};
    const QModelIndex source = m_proxy->mapToSource(index);
    return source.isValid() ? m_model->filePath(source) : QString();
}
