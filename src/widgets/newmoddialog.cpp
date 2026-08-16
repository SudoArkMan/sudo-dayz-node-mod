#include "newmoddialog.h"

#include "theme.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

namespace {

// Spelled the way the template's own folders are, because the map name goes
// straight into Missions/<prefix>.<map>.
const char *const kMaps[] = {"ChernarusPlus", "Enoch", "sakhal"};

const char *const kLocationKey = "newMod/location";
const char *const kAuthorKey = "newMod/author";

// Marks the display name as the user's own, so typing in the prefix stops
// overwriting it.
const char *const kEditedProperty = "userEdited";

// The maps that were picked before the mission was switched off.
const char *const kLastMapsProperty = "lastMaps";

// The prefix has to survive being a class name and a PBO name, so it carries
// underscores the launcher has no reason to show.
QString displayNameFor(const QString &prefix)
{
    QString name = prefix;
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    return name.simplified();
}

QString joinPath(const QString &parentDir, const QString &name)
{
    if (parentDir.isEmpty() || name.isEmpty()) return QString();
    return QDir::cleanPath(parentDir + QLatin1Char('/') + name);
}

QString shown(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

// Hidden and system entries count: scaffolding into a folder that holds a .git
// or a desktop.ini is still scaffolding into somebody's existing work.
bool directoryIsEmpty(const QString &path)
{
    return QDir(path).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                              | QDir::System);
}

QStringList selectedMaps(const QListWidget *list)
{
    QStringList maps;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem *item = list->item(i);
        if (item->isSelected()) maps << item->text();
    }
    return maps;
}

// The stylesheet carries the diagnostic colours on a property selector, so a
// repolish is what actually recolours the label.
void setSeverity(QLabel *label, const char *severity)
{
    label->setProperty("severity", QString::fromLatin1(severity));
    label->style()->unpolish(label);
    label->style()->polish(label);
}

// Past a couple of names the list is worth more in the details pane than in a
// message box, which wraps a long line into something that reads as a wall.
const int kNamesInline = 2;

QString fileNames(const QStringList &paths)
{
    QStringList names;
    for (const QString &path : paths) names << QFileInfo(path).fileName();
    return names.join(QStringLiteral(", "));
}

// What creating the mod did to the work drive, in the words the report uses.
// Always says something: a junction the user did not ask for is a change to
// their disk, and one made silently is nearly as bad as one never made.
QStringList workDriveLines(const WorkDriveAction &action)
{
    if (!action.attempted()) return {};

    QStringList lines;
    if (!action.movedTo.isEmpty())
        lines << QObject::tr("Moved the folder that was in the way to %1. Nothing "
                             "was deleted.").arg(shown(action.movedTo));
    if (action.ok) {
        lines << QObject::tr("Work drive: %1 now points at the mod folder, which is "
                             "what lets binarize and Workbench find it.")
                     .arg(shown(action.link.link));
        return lines;
    }

    lines << QObject::tr("Work drive: %1").arg(action.link.message());
    // The reason mklink itself gave, when it was reached at all. A link that
    // failed with nothing on screen is the thing that costs an evening.
    if (!action.error.isEmpty() && action.error != action.link.message())
        lines << action.error;
    if (!action.link.fix().isEmpty()) lines << action.link.fix();
    return lines;
}

// The one case where a folder in the way can be cleared without losing
// anything, and the only one the user is ever asked about. Every other row of
// the decision table is reported and left exactly as it was found.
//
// The ask names the folder, what was compared, and where it would go, because
// agreeing to move a folder you cannot see is not agreeing to anything.
// Nothing is deleted whichever answer comes back.
bool offerToMoveAside(QWidget *parent, ModTemplateResult &result)
{
    const WorkDriveLink link = result.workDrive.link;
    if (!link.canMoveAside()) return false;

    const QString aside = asideNameFor(link.link);
    QMessageBox box(QMessageBox::Question, QObject::tr("Work drive link"),
                    QObject::tr("%1 is already a real folder.").arg(shown(link.link)),
                    QMessageBox::NoButton, parent);
    box.setInformativeText(
        QObject::tr("%1\n\nMoving it to %2 clears the way for the link. That is a "
                    "rename, so nothing is deleted and you can put it back.")
            .arg(link.message(), shown(aside)));
    QPushButton *move =
        box.addButton(QObject::tr("Move it aside and link"), QMessageBox::AcceptRole);
    QPushButton *leave =
        box.addButton(QObject::tr("Leave it alone"), QMessageBox::RejectRole);
    // Leaving it alone is the default, because the safe answer should be the
    // one a stray Return key gives.
    box.setDefaultButton(leave);
    box.exec();
    if (box.clickedButton() != move) return false;

    result.workDrive =
        moveAsideAndLinkModFolder(link.link, link.target, result.modRoot);
    return true;
}

void showReport(QWidget *parent, const ModTemplateResult &result)
{
    const QString root = shown(result.modRoot);
    QStringList lines;
    lines << (result.created.size() == 1
                  ? QObject::tr("One file written to %1.").arg(root)
                  : QObject::tr("%1 files written to %2.")
                        .arg(result.created.size())
                        .arg(root));

    const QString scripts = QDir(result.modRoot).relativeFilePath(result.scriptsRoot);
    if (!scripts.isEmpty() && !scripts.startsWith(QLatin1String("..")))
        lines << QObject::tr("Generated scripts go in %1.").arg(shown(scripts));

    if (!result.skipped.isEmpty())
        lines << (result.skipped.size() <= kNamesInline
                      ? QObject::tr("Not copied: %1.").arg(fileNames(result.skipped))
                      : QObject::tr("%1 files were not copied.")
                            .arg(result.skipped.size()));

    lines << workDriveLines(result.workDrive);

    // An unlinked mod still builds nothing, so the box says so rather than
    // wearing the information icon over a warning.
    const bool linked = !result.workDrive.attempted() || result.workDrive.ok;
    QMessageBox box(linked ? QMessageBox::Information : QMessageBox::Warning,
                    QObject::tr("Mod created"),
                    QObject::tr("%1 is ready.").arg(QFileInfo(result.modRoot).fileName()),
                    QMessageBox::Ok, parent);
    box.setInformativeText(lines.join(QLatin1Char('\n')));
    if (result.skipped.size() > kNamesInline) {
        QStringList detail;
        for (const QString &path : result.skipped) detail << shown(path);
        box.setDetailedText(detail.join(QLatin1Char('\n')));
    }
    // Left alone, the box takes its width from the short first line and wraps
    // the path across two of them. The spacer puts a floor under the width so
    // the folder reads as one string. It goes in last because setDetailedText
    // rebuilds the layout and would drop it.
    if (auto *grid = qobject_cast<QGridLayout *>(box.layout()))
        grid->addItem(new QSpacerItem(380, 0, QSizePolicy::Minimum, QSizePolicy::Fixed),
                      grid->rowCount(), 0, 1, grid->columnCount());
    box.exec();
}

} // namespace

NewModDialog::NewModDialog(QWidget *parent)
    : QDialog(parent), m_prefix(new QLineEdit(this)),
      m_displayName(new QLineEdit(this)), m_author(new QLineEdit(this)),
      m_location(new QLineEdit(this)),
      m_includeMissions(new QCheckBox(tr("Include a mission"), this)),
      m_maps(new QListWidget(this)), m_preview(new QLabel(this)),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                     this))
{
    setWindowTitle(tr("New mod"));
    setModal(true);

    m_prefix->setPlaceholderText(tr("SUDO_Link"));
    m_prefix->setToolTip(tr("Becomes the folder name, the PBO name and the prefix on "
                            "every class the mod declares."));
    m_displayName->setPlaceholderText(tr("SUDO Link"));
    m_displayName->setToolTip(tr("The name the launcher shows."));
    m_author->setPlaceholderText(tr("Your handle"));
    m_location->setPlaceholderText(tr("The folder the mod folder is created in"));

    auto *browse = new QPushButton(tr("Browse"), this);
    browse->setAutoDefault(false);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(6);
    form->addRow(tr("Mod prefix"), m_prefix);
    form->addRow(tr("Display name"), m_displayName);
    form->addRow(tr("Author"), m_author);

    auto *locationRow = new QHBoxLayout;
    locationRow->setSpacing(6);
    locationRow->addWidget(m_location, 1);
    locationRow->addWidget(browse);
    form->addRow(tr("Location"), locationRow);

    for (const char *map : kMaps) m_maps->addItem(QString::fromLatin1(map));
    m_maps->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_maps->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Three rows is the whole list, so a scroll bar here would only be a hint
    // that something is hidden when nothing is.
    const int rowHeight = QFontMetrics(m_maps->font()).height() + 6;
    m_maps->setFixedHeight(rowHeight * 3 + 2);

    auto *mapsHint = new QLabel(tr("Ctrl-click to pick more than one."), this);
    mapsHint->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));

    // Carried in the form rather than under it so the checkbox, the list and
    // the four fields all start on the same left edge.
    auto *missionBox = new QWidget(this);
    auto *missionLayout = new QVBoxLayout(missionBox);
    missionLayout->setContentsMargins(0, 4, 0, 0);
    missionLayout->setSpacing(4);
    missionLayout->addWidget(m_includeMissions);
    missionLayout->addWidget(m_maps);
    missionLayout->addWidget(mapsHint);
    form->addRow(QString(), missionBox);

    m_preview->setWordWrap(true);
    m_preview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Four lines are held open whatever the text says, so the dialog does not
    // resize under the pointer every time the preview changes length. That is
    // the folder, what goes in it, and the work drive line, with one spare for
    // a path long enough to wrap.
    m_preview->setMinimumHeight(QFontMetrics(m_preview->font()).lineSpacing() * 4 + 4);

    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Create mod"));
    m_buttons->button(QDialogButtonBox::Ok)->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addLayout(form);
    layout->addSpacing(2);
    layout->addWidget(m_preview);
    layout->addWidget(m_buttons);

    connect(m_prefix, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (!m_displayName->property(kEditedProperty).toBool())
            m_displayName->setText(displayNameFor(text.trimmed()));
        validateInput();
    });
    // textEdited fires for typing and not for the line above, which is what
    // separates the user's own name from the one derived for them. Clearing the
    // field hands it back to the prefix.
    connect(m_displayName, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_displayName->setProperty(kEditedProperty, !text.trimmed().isEmpty());
    });
    connect(m_location, &QLineEdit::textChanged, this, &NewModDialog::validateInput);
    connect(browse, &QPushButton::clicked, this, &NewModDialog::browse);
    // A disabled list goes on painting its selection, which reads as a live
    // choice sitting behind a control that is switched off. The maps follow the
    // checkbox instead, and come back the way they were left.
    connect(m_includeMissions, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) {
            m_maps->setProperty(kLastMapsProperty, selectedMaps(m_maps));
            m_maps->clearSelection();
        } else {
            QStringList wanted = m_maps->property(kLastMapsProperty).toStringList();
            if (wanted.isEmpty()) wanted << QString::fromLatin1(kMaps[0]);
            for (int i = 0; i < m_maps->count(); ++i)
                m_maps->item(i)->setSelected(wanted.contains(m_maps->item(i)->text()));
        }
        validateInput();
    });
    connect(m_maps, &QListWidget::itemSelectionChanged, this,
            &NewModDialog::validateInput);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_includeMissions, &QCheckBox::toggled, mapsHint, &QLabel::setEnabled);

    QSettings settings;
    m_location->setText(shown(settings.value(QLatin1String(kLocationKey)).toString()));
    // The author is the same person for every mod they will ever make here.
    m_author->setText(settings.value(QLatin1String(kAuthorKey)).toString());

    mapsHint->setEnabled(false);
    setMinimumWidth(470);
    m_prefix->setFocus();
    validateInput();
}

ModTemplateOptions NewModDialog::options() const
{
    ModTemplateOptions options;
    options.prefix = m_prefix->text().trimmed();
    options.displayName = m_displayName->text().trimmed();
    if (options.displayName.isEmpty()) options.displayName = options.prefix;
    options.author = m_author->text().trimmed();
    options.includeMissions = m_includeMissions->isChecked();
    if (options.includeMissions) options.maps = selectedMaps(m_maps);
    return options;
}

QString NewModDialog::parentDirectory() const
{
    const QString typed = m_location->text().trimmed();
    if (typed.isEmpty()) return QString();
    return QDir::cleanPath(QDir::fromNativeSeparators(typed));
}

void NewModDialog::browse()
{
    QString start = parentDirectory();
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    const QString picked =
        QFileDialog::getExistingDirectory(this, tr("Where the mod folder goes"), start);
    if (picked.isEmpty()) return;
    m_location->setText(shown(picked));
}

void NewModDialog::validateInput()
{
    const bool missions = m_includeMissions->isChecked();
    m_maps->setEnabled(missions);

    const QString prefix = m_prefix->text().trimmed();
    const QString parentDir = parentDirectory();
    const QString target = joinPath(parentDir, prefix);
    const QStringList maps = selectedMaps(m_maps);

    QString problem;
    QString reason;
    if (!modTemplateAvailable())
        problem = tr("The bundled template is missing. Put it back in "
                     "resources/mod-template.");
    else if (!isValidModPrefix(prefix, &reason))
        problem = reason;
    else if (parentDir.isEmpty())
        problem = tr("Choose where the mod folder goes.");
    else if (!QFileInfo(parentDir).isDir())
        problem = tr("%1 is not a folder.").arg(shown(parentDir));
    else if (QFileInfo::exists(target) && !QFileInfo(target).isDir())
        problem = tr("%1 is already a file.").arg(shown(target));
    else if (QFileInfo(target).isDir() && !directoryIsEmpty(target))
        problem = tr("%1 already exists and has files in it.").arg(shown(target));
    else if (missions && maps.isEmpty())
        problem = tr("Pick a map, or leave the mission out.");
    // The mod folder would be the work drive folder its own junction has to
    // take, so no link can be made and AddonBuilder is handed a path that is
    // not there. It is the layout behind every "P:\<name> is a real folder"
    // report, and the only place to catch it is before the folder is written.
    else if (!target.isEmpty()
             && workDriveLinkFor(joinPath(target, prefix))
                    .compare(target, Qt::CaseInsensitive) == 0)
        problem = tr("A mod created directly on %1 takes the name its own work drive "
                     "link needs, and then it cannot be built. Pick a folder outside "
                     "the work drive.").arg(shown(workDriveRoot()));

    if (problem.isEmpty()) {
        QStringList lines;
        lines << tr("Creates %1").arg(shown(target));
        lines << (missions
                      ? tr("config.cpp, Scripts, a Workbench project, and missions for %1.")
                            .arg(maps.join(QStringLiteral(", ")))
                      : tr("config.cpp, Scripts and a Workbench project."));

        // The junction is part of creating the mod, so the dialog says so
        // before it is made. Only the two things that can be known before the
        // folder exists are claimed here; what is actually at that path is
        // classified after the write, when there is something to compare with.
        const QString link = workDriveLinkFor(joinPath(target, prefix));
        if (!QFileInfo(workDriveRoot()).isDir())
            lines << tr("%1 is not mounted, so the mod will not be linked to the "
                        "work drive yet.").arg(shown(workDriveRoot()));
        else if (QFileInfo::exists(link))
            lines << tr("Something is already at %1, so the report will say what it "
                        "is before anything is linked.").arg(shown(link));
        else
            lines << tr("Links %1 to it, so binarize and Workbench can find it.")
                         .arg(shown(link));

        m_preview->setText(lines.join(QLatin1Char('\n')));
        setSeverity(m_preview, "note");
    } else {
        m_preview->setText(problem);
        setSeverity(m_preview, "error");
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
}

ModTemplateResult NewModDialog::run(QWidget *parent)
{
    NewModDialog dialog(parent);
    while (dialog.exec() == QDialog::Accepted) {
        const ModTemplateOptions options = dialog.options();
        ModTemplateResult result = scaffoldMod(dialog.parentDirectory(), options);
        if (!result.ok) {
            // The dialog is shown again with everything still in it: a scaffold
            // fails on one wrong character in a path far more often than it
            // fails on the whole set of answers.
            QMessageBox::warning(parent, tr("Could not create the mod"),
                                 result.error.isEmpty()
                                     ? tr("The scaffolder gave no reason.")
                                     : result.error);
            continue;
        }

        QSettings settings;
        settings.setValue(QLatin1String(kLocationKey), dialog.parentDirectory());
        settings.setValue(QLatin1String(kAuthorKey), options.author);

        // Asked before the report, so the report is what is true when the user
        // closes it rather than a state that has already moved on.
        offerToMoveAside(parent, result);
        showReport(parent, result);
        return result;
    }
    // Cancelled. Every failure along the way has already been shown, so this
    // carries no error for the caller to report a second time.
    return {};
}
