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
#include <QResizeEvent>
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

// The app sheet names a colour for QLabel, and a sheet beats setPalette, so a
// label that wants the muted grey has to ask for it the same way.
void dimLabel(QLabel *label)
{
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
}

// A scrolling view whose height asks for what is in it.
//
// QScrollArea's own hint is its widget's, taken once and then capped at two
// dozen lines whatever the widget says afterwards, so a panel sized from it
// comes out tall enough for a gallery three times the one that ships. The
// minimum stays QScrollArea's, which is what lets the page shrink: the tiles
// scroll rather than the window growing to fit them.
class TileScroll : public QScrollArea {
public:
    using QScrollArea::QScrollArea;

    QSize sizeHint() const override
    {
        const QWidget *inner = widget();
        if (!inner) return QScrollArea::sizeHint();
        const QSize want = inner->sizeHint();
        return QSize(want.width() + frameWidth() * 2, want.height() + frameWidth() * 2);
    }

protected:
    // A scroll area's widget is not in any layout of ours, so nothing carries
    // its change of mind outwards: the tiles settle on a height, and the panel
    // is still sized from what they asked for before they had a width.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == widget() && event->type() == QEvent::LayoutRequest)
            updateGeometry();
        return QScrollArea::eventFilter(watched, event);
    }
};

// One region of the page: a heading, a hairline, and a body.
//
// All three regions get one. With a container on the recent list alone, that
// column reads as a panel and the other two as stacks floating on the ground,
// which is the same page whatever is in them.
class StartPanel : public QFrame {
public:
    StartPanel(const QString &title, QWidget *parent)
        : QFrame(parent), m_body(new QVBoxLayout)
    {
        setObjectName(QStringLiteral("startPanel"));
        // Id selector: the rule has to land on the panel and not on the cards
        // and lists inside it, which carry their own faces.
        setStyleSheet(QStringLiteral("#startPanel { background: %1; "
                                     "border: 1px solid %2; border-radius: 3px; }")
                          .arg(theme::panelBg().name(), theme::border().name()));

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 9, 12, 12);
        layout->setSpacing(8);

        auto *heading = new QLabel(title, this);
        heading->setFont(theme::uiFont(10, true));
        layout->addWidget(heading);
        layout->addWidget(ruleLine(this));

        m_body->setContentsMargins(0, 0, 0, 0);
        m_body->setSpacing(8);
        layout->addLayout(m_body, 1);
    }

    // Where a caller puts the region's content.
    QVBoxLayout *body() const { return m_body; }

private:
    QVBoxLayout *m_body;
};

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
        // Shrinkable on purpose. A Minimum policy takes its floor from the size
        // hint, and the hint of a card that has not been laid out yet is the
        // five or six lines its summary wraps to at 200 pixels wide. A column of
        // those puts the whole page's minimum height above the window, and a
        // window that has been grown to fit a transient measurement never
        // shrinks back. The floor is minimumSizeHint below instead.
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
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

    // The card's own height is measured from this, so the layout has to be told
    // the answer has changed.
    void setSummary(const QString &text)
    {
        if (m_summary == text) return;
        m_summary = text;
        setToolTip(m_summary);
        updateGeometry();
        update();
    }

    // Measured at the card's own width once it has one. The narrow figure is
    // only for a card that has never been laid out.
    //
    // The width matters as much as the height here: a column's preferred height
    // is its preferred height at its own preferred WIDTH, so a card that keeps
    // asking for 200 pixels makes the column reserve the five or six lines a
    // summary wraps to at 200 pixels, three times what it draws at.
    QSize sizeHint() const override
    {
        const int unlaid = qMax(200, QFontMetrics(theme::uiFont(9, true))
                                         .horizontalAdvance(m_title)
                                     + kCardPadding * 2);
        const int measured = width() > unlaid ? width() : unlaid;
        return QSize(measured, heightForWidth(measured));
    }

    // Title, and one line of the summary to say there is more. A card squeezed
    // this far elides rather than the page growing a scroll bar.
    QSize minimumSizeHint() const override
    {
        const QFontMetrics titleMetrics(theme::uiFont(9, true));
        const QFontMetrics bodyMetrics(theme::uiFont(8));
        return QSize(140, kCardPadding * 2 + titleMetrics.height() + 3
                              + bodyMetrics.lineSpacing());
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

    // The hint depends on the width, so a width the card did not have when the
    // column was last measured has to be handed back to the column.
    void resizeEvent(QResizeEvent *event) override
    {
        QAbstractButton::resizeEvent(event);
        if (event->oldSize().width() != event->size().width()) updateGeometry();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool on = isEnabled();
        // The card sits on a panel, so its face is the step above the panel
        // body. Filling it with the panel colour would leave the border doing
        // the whole job of saying where the card is.
        QColor fill = theme::headerBg();
        if (on && isDown()) fill = theme::accent().darker(210);
        else if (on && underMouse()) fill = theme::headerBg().lighter(118);

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
    : QWidget(parent), m_recent(recent), m_lockup(nullptr), m_columns(nullptr),
      m_recentPanel(nullptr), m_list(nullptr), m_empty(nullptr),
      m_missingNote(nullptr), m_gallery(nullptr),
      m_firstAction(nullptr), m_readModCard(nullptr), m_templates(startTemplates())
{
    setObjectName(QStringLiteral("startPage"));
    // The lockup is drawn on the pack's own ground, so the page has to be that
    // colour or the art sits in a visible box of its own.
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, theme::windowBg());
    setPalette(palette);
    setAutoFillBackground(true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 18, 24, 18);
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
    dimLabel(version);
    header->addWidget(version, 0, Qt::AlignRight | Qt::AlignBottom);
    outer->addLayout(header);
    outer->addWidget(ruleLine(this));

    // Two columns rather than three. The recent list is the one region that
    // grows with use, so it gets a column to itself and the two whose contents
    // are known in advance stack in the other, which fills the right column at
    // any window size the app runs at.
    m_columns = new QHBoxLayout;
    m_columns->setContentsMargins(0, 0, 0, 0);
    m_columns->setSpacing(16);
    m_recentPanel = buildRecentPanel();
    m_columns->addWidget(m_recentPanel, 5);

    auto *right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(14);
    // Start takes its own height and templates take the rest, so a tile added
    // later lands in room that is already there.
    right->addWidget(buildStartPanel(), 0);
    right->addWidget(buildTemplatesPanel(), 1);
    m_columns->addLayout(right, 4);

    // The panels take the height they have content for and the page keeps the
    // rest, rather than three regions stretched to the bottom of whatever
    // window this is with a third of each one empty inside. Weighted towards
    // the bottom so the block sits above the middle, where a page is read from.
    outer->addStretch(2);
    outer->addLayout(m_columns, 0);
    outer->addStretch(3);

    rebuildRecent();
}

QWidget *StartPage::buildRecentPanel()
{
    auto *panel = new StartPanel(QStringLiteral("Recent projects"), this);

    m_list = new QListWidget(panel);
    m_list->setItemDelegate(new RecentDelegate(m_list));
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    // The panel is the container. Left at the app sheet's framed ground the
    // list would be a second box drawn inside the first one.
    m_list->setStyleSheet(
        QStringLiteral("QListWidget { background: transparent; border: none; }"));
    panel->body()->addWidget(m_list, 1);

    // A first run is the page most people see first, so the empty list says
    // what will be here and where to start rather than leaving a blank panel.
    m_empty = new QWidget(panel);
    auto *emptyLayout = new QVBoxLayout(m_empty);
    // The inset does the job a maximum width would: a wrapped label measured at
    // one width and then held to a narrower one loses its last line.
    emptyLayout->setContentsMargins(48, 0, 48, 0);
    emptyLayout->setSpacing(6);
    emptyLayout->addStretch(1);

    auto *headline = new QLabel(QStringLiteral("Nothing opened yet"), m_empty);
    headline->setFont(theme::uiFont(10, true));
    headline->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(headline);

    auto *hint = new QLabel(
        QStringLiteral("Projects you open are listed here, newest first, with the "
                       "mod they belong to and what is in them. Start one on the "
                       "right, or open a .sdzn you already have."),
        m_empty);
    hint->setFont(theme::uiFont(9));
    hint->setWordWrap(true);
    hint->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    dimLabel(hint);
    emptyLayout->addWidget(hint);
    // Weighted, not even: text sitting on the true centre of a tall panel reads
    // as low, and the panel is at its tallest exactly when this is showing.
    emptyLayout->addStretch(2);
    panel->body()->addWidget(m_empty, 1);

    m_missingNote = new QLabel(panel);
    m_missingNote->setFont(theme::uiFont(8));
    m_missingNote->setWordWrap(true);
    m_missingNote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    dimLabel(m_missingNote);
    panel->body()->addWidget(m_missingNote);

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

    return panel;
}

QWidget *StartPage::buildStartPanel()
{
    auto *panel = new StartPanel(QStringLiteral("Start"), this);
    QVBoxLayout *body = panel->body();

    auto *newProject = new StartCard(
        QStringLiteral("New project"),
        QStringLiteral("A bare graph with nothing on disk behind it. Save it "
                       "wherever you want it."),
        panel);
    connect(newProject, &QAbstractButton::clicked,
            this, &StartPage::newProjectRequested);
    body->addWidget(newProject);
    m_firstAction = newProject;

    auto *newMod = new StartCard(
        QStringLiteral("New mod"),
        QStringLiteral("Writes the whole mod folder from the bundled template, "
                       "config and all, then opens a project inside it."),
        panel);
    connect(newMod, &QAbstractButton::clicked, this, &StartPage::newModRequested);
    body->addWidget(newMod);

    auto *open = new StartCard(
        QStringLiteral("Open project"),
        QStringLiteral("Any .sdzn on this machine. The dialog starts in the folder "
                       "you used last."),
        panel);
    connect(open, &QAbstractButton::clicked, this, &StartPage::browseRequested);
    body->addWidget(open);

    // Last of the four, because it is the one that starts nothing. It is here at
    // all because reading somebody else's mod is how most people learn this
    // engine, and it used to be a dock tab nobody could find.
    auto *readMod = new StartCard(
        QStringLiteral("Read a mod"),
        QStringLiteral("Any installed mod, or a .pbo from disk, as graphs to read."),
        panel);
    connect(readMod, &QAbstractButton::clicked, this, &StartPage::browseModsRequested);
    body->addWidget(readMod);
    m_readModCard = readMod;

    return panel;
}

void StartPage::setModLibraryLine(const QString &text)
{
    // static_cast rather than qobject_cast: StartCard is local to this file and
    // declares no meta object of its own, so a qobject_cast would only ever be
    // checking that this is a button.
    if (m_readModCard) static_cast<StartCard *>(m_readModCard)->setSummary(text);
}

QWidget *StartPage::buildTemplatesPanel()
{
    auto *panel = new StartPanel(QStringLiteral("Templates"), this);

    auto *scroll = new TileScroll(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    // The app sheet gives a scroll area the panel colour, which is the colour
    // already behind it here.
    scroll->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    m_gallery = new QWidget(scroll);
    // Painted rather than left to show the panel through, so grabbing the
    // gallery on its own gives a picture with a background on it.
    QPalette ground = m_gallery->palette();
    ground.setColor(QPalette::Window, theme::panelBg());
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
    panel->body()->addWidget(scroll, 1);
    return panel;
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
    const bool any = m_list->count() > 0;
    m_list->setVisible(any);
    m_empty->setVisible(!any);
    if (!any && hadFocus && m_firstAction)
        m_firstAction->setFocus(Qt::OtherFocusReason);

    // An empty panel does not need the wider half of the page, and giving the
    // width back is what keeps a first run from reading as a page of nothing
    // with the working half squeezed into a third of it.
    if (m_columns) m_columns->setStretch(0, any ? 5 : 3);

    // Nor does it need the height. The two columns are laid out side by side, so
    // left to itself this panel is stretched to whatever Start and Templates add
    // up to, and two recent projects then sit at the top of a box with three
    // hundred empty pixels under them. Capped at the list's own rows and aligned
    // to the top, the panel is the size of what is in it, and it grows with the
    // list up to about the height the other column has anyway.
    const int rowHeight = qMax(1, m_list->sizeHintForRow(0));
    m_list->setMaximumHeight(qBound(rowHeight, m_list->count() * rowHeight + 4, 520));
    if (m_columns && m_recentPanel)
        m_columns->setAlignment(m_recentPanel, Qt::AlignTop);

    m_missingNote->setText(
        missing > 0 ? QStringLiteral("%1 of these is not where it was. Right click it "
                                     "to drop it from the list.").arg(missing)
                    : QString());
    m_missingNote->setVisible(missing > 0);
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
