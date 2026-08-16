#include "widgets/startpage.h"

#include "branding.h"
#include "recentprojects.h"
#include "theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <utility>

namespace {

// The row's parts. Only the name is a display role: the path and the summary
// line are drawn by the delegate, and letting the view lay them out as one
// string is what makes them one colour and one size.
constexpr int kPathRole = Qt::UserRole;
constexpr int kMetaRole = Qt::UserRole + 1;
constexpr int kMissingRole = Qt::UserRole + 2;

constexpr int kRowPadding = 7;
constexpr int kCardPadding = 10;

// The lockup's height on the page. Tall enough for "visual scripting tools" to
// stay legible after the trim, short enough that the columns start above the
// fold on a laptop screen.
constexpr int kLockupHeight = 54;

qreal ratioFor(const QWidget *widget)
{
    if (widget && widget->window() && widget->window()->windowHandle())
        return widget->window()->windowHandle()->devicePixelRatio();
    const QScreen *screen = QGuiApplication::primaryScreen();
    return screen ? screen->devicePixelRatio() : 1.0;
}

QWidget *ruleLine(QWidget *parent)
{
    auto *line = new QWidget(parent);
    line->setFixedHeight(1);
    QPalette palette = line->palette();
    palette.setColor(QPalette::Window, theme::border());
    line->setPalette(palette);
    line->setAutoFillBackground(true);
    return line;
}

QLabel *columnHeading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setFont(theme::uiFont(10, true));
    return label;
}

// One card, used for both the actions and the template tiles.
//
// A button rather than a clickable frame: focus, the space bar, the enter key
// and the disabled state all come with QAbstractButton, and a frame that wants
// to be reachable from the keyboard ends up reimplementing every one of them.
class StartCard : public QAbstractButton {
public:
    StartCard(QString title, QString summary, QWidget *parent = nullptr)
        : QAbstractButton(parent), m_title(std::move(title)),
          m_summary(std::move(summary))
    {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        // Minimum rather than Fixed: a Fixed policy caps the item's height at
        // its size hint, and the hint cannot know the column width the summary
        // will actually wrap to.
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setToolTip(m_summary);
    }

    // A word at the end of the title row. Used by the gallery for where a
    // template's script lands, which is the second question every tile raises.
    void setKicker(const QString &text)
    {
        m_kicker = text;
        update();
    }

    QSize sizeHint() const override
    {
        const int width = qMax(200, QFontMetrics(theme::uiFont(9, true))
                                        .horizontalAdvance(m_title) + kCardPadding * 2);
        return QSize(width, heightForWidth(width));
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const QFontMetrics titleMetrics(theme::uiFont(9, true));
        const QFontMetrics bodyMetrics(theme::uiFont(8));
        const int inner = qMax(40, width - kCardPadding * 2);
        const QRect wrapped = bodyMetrics.boundingRect(
            QRect(0, 0, inner, 1000), Qt::TextWordWrap, m_summary);
        return kCardPadding * 2 + titleMetrics.height() + 3 + wrapped.height();
    }

protected:
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool on = isEnabled();
        QColor fill = theme::panelBg();
        if (on && isDown()) fill = theme::accent().darker(210);
        else if (on && underMouse()) fill = theme::headerBg();

        QColor edge = theme::border();
        if (on && hasFocus()) edge = theme::accent();

        const QRectF face = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(edge, hasFocus() ? 1.4 : 1.0));
        painter.setBrush(fill);
        painter.drawRoundedRect(face, 3, 3);

        const QRect inner = rect().adjusted(kCardPadding, kCardPadding,
                                            -kCardPadding, -kCardPadding);
        painter.setFont(theme::uiFont(9, true));
        const QFontMetrics titleMetrics(painter.font());
        const QRect titleRect(inner.left(), inner.top(), inner.width(),
                              titleMetrics.height());

        int titleWidth = inner.width();
        if (!m_kicker.isEmpty()) {
            const QFont kickerFont = theme::monoFont(8);
            const QFontMetrics kickerMetrics(kickerFont);
            painter.setFont(kickerFont);
            painter.setPen(theme::textDim());
            painter.drawText(titleRect, Qt::AlignRight | Qt::AlignVCenter, m_kicker);
            titleWidth -= kickerMetrics.horizontalAdvance(m_kicker) + 10;
            painter.setFont(theme::uiFont(9, true));
        }

        painter.setPen(on ? theme::text() : theme::textDim());
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                         titleMetrics.elidedText(m_title, Qt::ElideRight,
                                                 qMax(40, titleWidth)));

        painter.setFont(theme::uiFont(8));
        painter.setPen(theme::textDim());
        painter.drawText(QRect(inner.left(), titleRect.bottom() + 3, inner.width(),
                               inner.bottom() - titleRect.bottom() - 3),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_summary);
    }

private:
    QString m_title;
    QString m_summary;
    QString m_kicker;
};

// Three lines per row: the project, where it is, and what is in it.
//
// The background is left to the style so hover and selection match every other
// list in the app; only the text is drawn here.
class RecentDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        Q_UNUSED(option);
        Q_UNUSED(index);
        const int name = QFontMetrics(theme::uiFont(9, true)).height();
        const int small = QFontMetrics(theme::uiFont(8)).height();
        return QSize(240, kRowPadding * 2 + name + small * 2 + 4);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        // Drawn by the style with no text on it, so the stylesheet's hover and
        // selection fills are the ones the rest of the app uses.
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const bool selected = opt.state & QStyle::State_Selected;
        const bool missing = index.data(kMissingRole).toBool();

        QColor nameColour = theme::text();
        QColor dimColour = theme::textDim();
        if (missing && !selected) nameColour = theme::textDim();
        if (selected) {
            // The selection fill is the accent, which the muted grey does not
            // survive. Both lines step up rather than the sub-lines dropping
            // out of view on the one row the user is looking at.
            dimColour = theme::text();
            dimColour.setAlpha(190);
        }

        const QRect inner = opt.rect.adjusted(8, kRowPadding, -8, -kRowPadding);
        const QFontMetrics nameMetrics(theme::uiFont(9, true));
        const QFontMetrics smallMetrics(theme::uiFont(8));

        painter->save();
        painter->setFont(theme::uiFont(9, true));
        painter->setPen(nameColour);
        QRect line(inner.left(), inner.top(), inner.width(), nameMetrics.height());
        painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight, inner.width()));

        painter->setFont(theme::uiFont(8));
        painter->setPen(dimColour);
        line = QRect(inner.left(), line.bottom() + 2, inner.width(),
                     smallMetrics.height());
        // Elided in the middle: the drive and the file name are the two ends,
        // and both of them are how a project is recognised.
        painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                          smallMetrics.elidedText(index.data(kPathRole).toString(),
                                                  Qt::ElideMiddle, inner.width()));

        if (missing && !selected) painter->setPen(theme::warningColor());
        line = QRect(inner.left(), line.bottom() + 2, inner.width(),
                     smallMetrics.height());
        painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                          smallMetrics.elidedText(index.data(kMetaRole).toString(),
                                                  Qt::ElideRight, inner.width()));
        painter->restore();
    }
};

// resources/ sits next to the executable when installed and a few levels up in
// a build tree, the same walk modTemplateAvailable does.
QString findResourceDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/resources"),
        appDir + QStringLiteral("/../resources"),
        appDir + QStringLiteral("/../../resources"),
        appDir + QStringLiteral("/../../../resources"),
        appDir + QStringLiteral("/../../../../resources"),
    };
    for (const QString &candidate : candidates)
        if (QFileInfo(candidate).isDir()) return QDir::cleanPath(candidate);
    return {};
}

QString countLine(const RecentProject &entry)
{
    QStringList parts;
    if (!entry.modName.isEmpty()) parts << entry.modName;
    if (entry.countsKnown()) {
        parts << QStringLiteral("%1 script%2").arg(entry.scriptCount)
                     .arg(entry.scriptCount == 1 ? QString() : QStringLiteral("s"));
        parts << QStringLiteral("%1 node%2").arg(entry.nodeCount)
                     .arg(entry.nodeCount == 1 ? QString() : QStringLiteral("s"));
    }
    parts << QStringLiteral("opened %1").arg(relativeTime(entry.lastOpened));
    return parts.join(QStringLiteral(", "));
}

} // namespace

QVector<StartTemplate> startTemplates(const QString &resourceDir)
{
    const QString resources = resourceDir.isEmpty() ? findResourceDir() : resourceDir;
    QVector<StartTemplate> templates;

    StartTemplate item;
    item.id = QStringLiteral("item.empty");
    item.title = QStringLiteral("Empty item script");
    item.summary = QStringLiteral("A class of your own with a constructor and a "
                                  "destructor and nothing in them yet, for code no "
                                  "vanilla class owns.");
    item.kind = StartTemplateKind::Script;
    item.script.className = QStringLiteral("MyItem");
    item.script.kind = ScriptKind::NewClass;
    item.module = QStringLiteral("4_World");
    templates.append(item);

    StartTemplate modded;
    modded.id = QStringLiteral("modded.itembase");
    modded.title = QStringLiteral("Modded ItemBase");
    modded.summary = QStringLiteral("Reopens ItemBase and overrides EEInit with its "
                                    "super call in place, which is where an item's "
                                    "own setup goes.");
    modded.kind = StartTemplateKind::Script;
    modded.script.className = QStringLiteral("ItemBase");
    modded.script.baseClass = QStringLiteral("ItemBase");
    modded.script.kind = ScriptKind::ModdedClass;
    modded.module = QStringLiteral("4_World");
    templates.append(modded);

    StartTemplate mission;
    mission.id = QStringLiteral("modded.missionserver");
    mission.title = QStringLiteral("Modded MissionServer");
    mission.summary = QStringLiteral("Reopens MissionServer and overrides OnInit, on "
                                     "the mission layer where server side setup has "
                                     "to live.");
    mission.kind = StartTemplateKind::Script;
    mission.script.className = QStringLiteral("MissionServer");
    mission.script.baseClass = QStringLiteral("MissionServer");
    mission.script.kind = ScriptKind::ModdedClass;
    mission.module = QStringLiteral("5_Mission");
    templates.append(mission);

    StartTemplate showcase;
    showcase.id = QStringLiteral("project.showcase");
    showcase.title = QStringLiteral("Node showcase");
    showcase.summary = QStringLiteral("The demo project, already wired: an event, a "
                                      "branch, graph variables and a server only "
                                      "guard, to read rather than to build.");
    showcase.kind = StartTemplateKind::Project;
    if (!resources.isEmpty())
        showcase.projectPath =
            QDir(resources).absoluteFilePath(QStringLiteral("Showcase.sdzn"));
    showcase.available = !showcase.projectPath.isEmpty()
                         && QFileInfo(showcase.projectPath).isFile();
    templates.append(showcase);

    return templates;
}

StartPage::StartPage(RecentProjects *recent, QWidget *parent)
    : QWidget(parent), m_recent(recent), m_lockup(nullptr), m_list(nullptr),
      m_listNote(nullptr), m_gallery(nullptr), m_firstAction(nullptr),
      m_templates(startTemplates())
{
    setObjectName(QStringLiteral("startPage"));
    // The lockup is drawn on the pack's own ground, so the page has to be that
    // colour or the art sits in a visible box of its own.
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, theme::windowBg());
    setPalette(palette);
    setAutoFillBackground(true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 20, 24, 20);
    outer->setSpacing(14);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    m_lockup = new QLabel(this);
    m_lockup->setPixmap(branding::lockup(kLockupHeight, ratioFor(this)));
    m_lockup->setFixedHeight(kLockupHeight);
    header->addWidget(m_lockup, 0, Qt::AlignLeft | Qt::AlignVCenter);
    header->addStretch(1);

    auto *version = new QLabel(
        QStringLiteral("v%1").arg(QCoreApplication::applicationVersion()), this);
    version->setFont(theme::monoFont(8));
    QPalette dim = version->palette();
    dim.setColor(QPalette::WindowText, theme::textDim());
    version->setPalette(dim);
    header->addWidget(version, 0, Qt::AlignRight | Qt::AlignBottom);
    outer->addLayout(header);
    outer->addWidget(ruleLine(this));

    auto *columns = new QHBoxLayout;
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(20);
    columns->addWidget(buildRecentColumn(), 4);
    columns->addWidget(buildStartColumn(), 3);
    columns->addWidget(buildTemplatesColumn(), 4);
    outer->addLayout(columns, 1);

    rebuildRecent();
}

QWidget *StartPage::buildRecentColumn()
{
    auto *column = new QWidget(this);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(columnHeading(QStringLiteral("Recent projects"), column));

    m_list = new QListWidget(column);
    m_list->setItemDelegate(new RecentDelegate(m_list));
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_list, 1);

    m_listNote = new QLabel(column);
    m_listNote->setWordWrap(true);
    m_listNote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_listNote->setContentsMargins(2, 4, 2, 0);
    QPalette dim = m_listNote->palette();
    dim.setColor(QPalette::WindowText, theme::textDim());
    m_listNote->setPalette(dim);
    layout->addWidget(m_listNote);
    // Takes the space when the list is hidden, which is the first run. Without
    // it the column has nothing that wants to grow and the heading drifts to
    // the middle of an empty page.
    layout->addStretch(0);

    // Activation only, not doubleClicked as well: a double click emits both, and
    // opening the project twice would ask twice about anything the first open
    // had to prompt for.
    connect(m_list, &QListWidget::itemActivated, this, &StartPage::openSelected);
    connect(m_list, &QWidget::customContextMenuRequested,
            this, &StartPage::showRecentMenu);

    // The list is where the keyboard starts, so the key that clears a row has
    // to work from there without a trip through the menu.
    auto *forget = new QAction(QStringLiteral("Remove from list"), m_list);
    forget->setShortcut(QKeySequence::Delete);
    forget->setShortcutContext(Qt::WidgetShortcut);
    connect(forget, &QAction::triggered, this, &StartPage::removeSelected);
    m_list->addAction(forget);

    return column;
}

QWidget *StartPage::buildStartColumn()
{
    auto *column = new QWidget(this);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(columnHeading(QStringLiteral("Start"), column));

    auto *newProject = new StartCard(
        QStringLiteral("New project"),
        QStringLiteral("A bare graph with nothing on disk behind it. Save it "
                       "wherever you want it."),
        column);
    connect(newProject, &QAbstractButton::clicked,
            this, &StartPage::newProjectRequested);
    layout->addWidget(newProject);
    m_firstAction = newProject;

    auto *newMod = new StartCard(
        QStringLiteral("New mod"),
        QStringLiteral("Writes the whole mod folder from the bundled template, "
                       "config and all, then opens a project inside it."),
        column);
    connect(newMod, &QAbstractButton::clicked, this, &StartPage::newModRequested);
    layout->addWidget(newMod);

    auto *open = new StartCard(
        QStringLiteral("Open project"),
        QStringLiteral("Any .sdzn on this machine. The dialog starts in the folder "
                       "you used last."),
        column);
    connect(open, &QAbstractButton::clicked, this, &StartPage::browseRequested);
    layout->addWidget(open);

    layout->addStretch(1);
    return column;
}

QWidget *StartPage::buildTemplatesColumn()
{
    auto *column = new QWidget(this);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(columnHeading(QStringLiteral("Templates"), column));

    auto *scroll = new QScrollArea(column);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);

    m_gallery = new QWidget(scroll);
    // Painted rather than left to show the page through, so grabbing the
    // column on its own gives a picture with a background on it.
    QPalette ground = m_gallery->palette();
    ground.setColor(QPalette::Window, theme::windowBg());
    m_gallery->setPalette(ground);
    m_gallery->setAutoFillBackground(true);
    auto *tiles = new QVBoxLayout(m_gallery);
    tiles->setContentsMargins(0, 0, 0, 0);
    tiles->setSpacing(8);

    for (const StartTemplate &tpl : m_templates) {
        auto *tile = new StartCard(tpl.title, tpl.summary, m_gallery);
        tile->setKicker(tpl.kind == StartTemplateKind::Project
                            ? QStringLiteral("project")
                            : tpl.module);
        tile->setEnabled(tpl.available);
        if (!tpl.available)
            tile->setToolTip(QStringLiteral("%1 is not installed beside the app.")
                                 .arg(QFileInfo(tpl.projectPath).fileName()));
        connect(tile, &QAbstractButton::clicked, this,
                [this, tpl]() { emit templateRequested(tpl); });
        tiles->addWidget(tile);
    }
    tiles->addStretch(1);

    scroll->setWidget(m_gallery);
    layout->addWidget(scroll, 1);
    return column;
}

void StartPage::refresh()
{
    if (m_recent) m_recent->refresh();
    rebuildRecent();
}

void StartPage::rebuildRecent()
{
    if (!m_list || !m_recent) return;

    const QString wasSelected = selectedPath();
    m_list->clear();

    int missing = 0;
    for (const RecentProject &entry : m_recent->entries()) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::DisplayRole, entry.name);
        item->setData(kPathRole, QDir::toNativeSeparators(entry.path));
        item->setData(kMissingRole, entry.missing);
        if (entry.missing) {
            ++missing;
            // Named as a move rather than as a loss, because that is what it
            // nearly always is and the two need different answers from the
            // user. Short enough to survive the elide at any column width.
            item->setData(kMetaRole, QStringLiteral("Not at this path. It has moved, "
                                                    "or its drive is not mounted."));
        } else {
            item->setData(kMetaRole, countLine(entry));
        }
        item->setData(Qt::ToolTipRole, QDir::toNativeSeparators(entry.path));
    }

    if (m_list->count() > 0) {
        // Selection follows the project the user had picked, so a refresh under
        // them does not move the highlight to the top of the list.
        int row = 0;
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(kPathRole).toString() != wasSelected) continue;
            row = i;
            break;
        }
        m_list->setCurrentRow(row);
    }

    // Removing the last row takes the list off the page, and with it the widget
    // the keyboard was on.
    const bool hadFocus = m_list->hasFocus();
    m_list->setVisible(m_list->count() > 0);
    if (m_list->count() == 0 && hadFocus && m_firstAction)
        m_firstAction->setFocus(Qt::OtherFocusReason);

    if (m_list->count() == 0) {
        m_listNote->setText(QStringLiteral("Nothing opened yet. Start a project on "
                                           "the right, or open one you already have."));
    } else if (missing > 0) {
        m_listNote->setText(QStringLiteral("%1 of these is not where it was. Right "
                                           "click it to drop it from the list.")
                                .arg(missing));
    } else {
        m_listNote->clear();
    }
    m_listNote->setVisible(!m_listNote->text().isEmpty());
}

QString StartPage::selectedPath() const
{
    if (!m_list) return {};
    const QListWidgetItem *item = m_list->currentItem();
    if (!item) return {};
    return item->data(kPathRole).toString();
}

void StartPage::openSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty()) return;
    if (m_list->currentItem()->data(kMissingRole).toBool()) {
        emit statusMessage(QStringLiteral("%1 is not there any more.")
                               .arg(QFileInfo(path).fileName()));
        return;
    }
    emit openRequested(QDir::fromNativeSeparators(path));
}

void StartPage::removeSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty() || !m_recent) return;
    m_recent->remove(QDir::fromNativeSeparators(path));
    rebuildRecent();
    emit statusMessage(QStringLiteral("%1 removed from the list. The file was not "
                                      "touched.").arg(QFileInfo(path).fileName()));
}

void StartPage::showRecentMenu(const QPoint &at)
{
    QListWidgetItem *item = m_list->itemAt(at);
    if (!item) return;
    m_list->setCurrentItem(item);

    const QString native = item->data(kPathRole).toString();
    const QString path = QDir::fromNativeSeparators(native);
    const bool missing = item->data(kMissingRole).toBool();

    QMenu menu(this);
    QAction *open = menu.addAction(QStringLiteral("Open"));
    open->setEnabled(!missing);
    QAction *folder = menu.addAction(QStringLiteral("Open containing folder"));
    // The folder outliving the project is the ordinary case for a project that
    // was moved, so this stays live as long as the folder is there.
    folder->setEnabled(QFileInfo(QFileInfo(path).absolutePath()).isDir());
    QAction *copy = menu.addAction(QStringLiteral("Copy path"));
    menu.addSeparator();
    QAction *forget = menu.addAction(QStringLiteral("Remove from list"));
    QAction *forgetMissing = nullptr;
    if (m_recent) {
        int gone = 0;
        for (const RecentProject &entry : m_recent->entries())
            if (entry.missing) ++gone;
        if (gone > 0)
            forgetMissing = menu.addAction(
                QStringLiteral("Remove all %1 missing entries").arg(gone));
    }

    QAction *picked = menu.exec(m_list->viewport()->mapToGlobal(at));
    if (!picked) return;
    if (picked == open) {
        openSelected();
    } else if (picked == folder) {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    } else if (picked == copy) {
        QGuiApplication::clipboard()->setText(native);
        emit statusMessage(QStringLiteral("Path copied."));
    } else if (picked == forget) {
        removeSelected();
    } else if (picked == forgetMissing && m_recent) {
        const int gone = m_recent->removeMissing();
        rebuildRecent();
        emit statusMessage(QStringLiteral("%1 missing %2 removed from the list.")
                               .arg(gone)
                               .arg(gone == 1 ? QStringLiteral("entry")
                                              : QStringLiteral("entries")));
    }
}

void StartPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // The page is reached from a menu and from startup, and in both cases the
    // hands are already on the keyboard. Landing focus on the recent list makes
    // the arrow keys and enter work with no click first.
    if (m_list && m_list->isVisible() && m_list->count() > 0) {
        if (!m_list->currentItem()) m_list->setCurrentRow(0);
        m_list->setFocus(Qt::OtherFocusReason);
    } else if (m_firstAction) {
        m_firstAction->setFocus(Qt::OtherFocusReason);
    }
}
