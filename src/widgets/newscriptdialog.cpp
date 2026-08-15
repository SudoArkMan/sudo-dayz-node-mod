#include "newscriptdialog.h"

#include "catalog.h"
#include "enforce/highlighter.h"
#include "enforce/lexer.h"
#include "theme.h"

#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QStyle>
#include <QVBoxLayout>

namespace {

QString ind(int n) { return QString(n, QLatin1Char('\t')); }

// Enforce identifiers are ASCII, so the letter test here is a to z and nothing
// wider: a name the dialog accepts and the compiler rejects is worse than no
// check at all.
bool isAsciiLetter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

bool isAsciiDigit(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }

// The method a modded class overrides as its example, or empty when there is no
// answer worth putting in front of the user. A made up name is worse than a
// blank body: Enforce compiles it as a new method and it never runs.
//
// EntityAI::EEInit (3_game/entities/entityai.c) and Mission::OnInit
// (3_game/gameplay.c) are both plain script methods rather than proto native
// ones, so every descendant can override them and call super.
QString exampleOverride(const QString &base, const Catalog *catalog)
{
    static const QStringList kMission = {
        QStringLiteral("Mission"), QStringLiteral("MissionBase"),
        QStringLiteral("MissionBaseWorld"), QStringLiteral("MissionServer"),
        QStringLiteral("MissionGameplay"),
    };
    static const QStringList kEntity = {
        QStringLiteral("EntityAI"),  QStringLiteral("InventoryItem"),
        QStringLiteral("ItemBase"),  QStringLiteral("ManBase"),
        QStringLiteral("PlayerBase"),
    };
    if (kMission.contains(base)) return QStringLiteral("OnInit");
    if (kEntity.contains(base)) return QStringLiteral("EEInit");

    if (catalog && catalog->isLoaded()) {
        if (catalog->isA(base, QStringLiteral("Mission"))) return QStringLiteral("OnInit");
        if (catalog->isA(base, QStringLiteral("EntityAI"))) return QStringLiteral("EEInit");
    }
    return {};
}

// The stylesheet carries the diagnostic colours on a property selector, so a
// repolish is what actually recolours the label.
void setSeverity(QLabel *label, const char *severity)
{
    label->setProperty("severity", QString::fromLatin1(severity));
    label->style()->unpolish(label);
    label->style()->polish(label);
}

} // namespace

QString scriptSkeleton(const NewScriptOptions &options, const Catalog *catalog)
{
    const QString name = options.className.trimmed();
    const QString base = options.baseClass.trimmed();
    QStringList lines;

    switch (options.kind) {
    case ScriptKind::Empty:
        return {};

    case ScriptKind::NewClass:
        // No `extends`. A bare class is its own root in Enforce and that is a
        // real difference from one deriving from Managed, so the base stays a
        // decision the author makes rather than one the template makes for them.
        lines << QStringLiteral("class ") + name;
        lines << QStringLiteral("{");
        lines << ind(1) + QStringLiteral("void ") + name + QStringLiteral("()");
        lines << ind(1) + QStringLiteral("{");
        lines << ind(1) + QStringLiteral("}");
        lines << QString();
        lines << ind(1) + QStringLiteral("void ~") + name + QStringLiteral("()");
        lines << ind(1) + QStringLiteral("{");
        lines << ind(1) + QStringLiteral("}");
        lines << QStringLiteral("}");
        break;

    case ScriptKind::ModdedClass: {
        const QString method = exampleOverride(base, catalog);
        lines << QStringLiteral("modded class ") + base;
        lines << QStringLiteral("{");
        if (method.isEmpty()) {
            lines << ind(1)
                         + QStringLiteral("// An override that never calls super drops the "
                                          "behaviour it replaced.");
        } else {
            lines << ind(1) + QStringLiteral("override void ") + method
                         + QStringLiteral("()");
            lines << ind(1) + QStringLiteral("{");
            lines << ind(2) + QStringLiteral("super.") + method + QStringLiteral("();");
            lines << ind(1) + QStringLiteral("}");
        }
        lines << QStringLiteral("}");
        break;
    }
    }

    // The trailing empty entry is what puts a newline on the last line.
    lines << QString();
    return lines.join(QLatin1Char('\n'));
}

QString scriptModuleOf(const QString &path)
{
    static const QStringList kModules = {
        QStringLiteral("3_Game"),
        QStringLiteral("4_World"),
        QStringLiteral("5_Mission"),
    };
    const QStringList parts =
        QDir::fromNativeSeparators(QDir::cleanPath(path))
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);

    // Deepest first, because a script always belongs to the module folder it is
    // actually in and not to one further up the path.
    for (int i = parts.size() - 1; i >= 0; --i)
        for (const QString &module : kModules)
            if (parts.at(i).compare(module, Qt::CaseInsensitive) == 0) return module;
    return {};
}

QString defaultBaseFor(const QString &module)
{
    if (module.compare(QLatin1String("4_World"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("ItemBase");
    if (module.compare(QLatin1String("5_Mission"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("MissionServer");
    // 3_Game compiles before entities and the mission exist, so a script there
    // has no one class it usually reopens.
    return {};
}

bool isValidScriptName(const QString &name, QString *reason)
{
    const auto refuse = [reason](const QString &why) {
        if (reason) *reason = why;
        return false;
    };

    if (name.isEmpty()) return refuse(QObject::tr("Name the class."));
    if (!isAsciiLetter(name.at(0)) && name.at(0) != QLatin1Char('_'))
        return refuse(
            QObject::tr("A class name starts with a letter or an underscore."));
    for (const QChar c : name)
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != QLatin1Char('_'))
            return refuse(QObject::tr("A class name takes letters, digits and "
                                      "underscores. \"%1\" is none of those.")
                              .arg(c));
    if (EnforceLexer::isKeyword(name) || EnforceLexer::isType(name))
        return refuse(QObject::tr("%1 is part of the Enforce language.").arg(name));

    if (reason) reason->clear();
    return true;
}

NewScriptDialog::NewScriptDialog(QWidget *parent, const QString &folder,
                                 const Catalog *catalog)
    : QDialog(parent), m_folder(folder), m_catalog(catalog),
      m_form(new QFormLayout), m_name(new QLineEdit(this)),
      m_kind(new QComboBox(this)), m_base(new QLineEdit(this)),
      m_preview(new QPlainTextEdit(this)), m_note(new QLabel(this)),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                     this))
{
    setWindowTitle(tr("New script"));
    setModal(true);

    const QString module = scriptModuleOf(folder);
    const QString base = defaultBaseFor(module);

    m_name->setPlaceholderText(tr("MyClass"));
    m_name->setToolTip(tr("Names the file. A new class takes this name as well."));

    m_kind->addItem(tr("New class"), static_cast<int>(ScriptKind::NewClass));
    m_kind->addItem(tr("Modded class"), static_cast<int>(ScriptKind::ModdedClass));
    m_kind->addItem(tr("Empty"), static_cast<int>(ScriptKind::Empty));
    // A script under 4_World or 5_Mission is far more often a change to vanilla
    // than a class of its own, so the module the folder sits in picks the shape.
    m_kind->setCurrentIndex(base.isEmpty() ? 0 : 1);

    m_base->setText(base);
    m_base->setPlaceholderText(tr("ItemBase"));
    m_base->setToolTip(tr("The vanilla class this script reopens."));
    if (m_catalog && m_catalog->isLoaded()) {
        // 6,000 names is past what anyone types from memory, and the base has to
        // match a declared class exactly for the modded class to reopen it.
        auto *completer = new QCompleter(m_catalog->classNames(), this);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        completer->setMaxVisibleItems(12);
        m_base->setCompleter(completer);
    }

    m_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_form->setHorizontalSpacing(8);
    m_form->setVerticalSpacing(6);
    m_form->addRow(tr("Class name"), m_name);
    m_form->addRow(tr("Starts as"), m_kind);
    m_form->addRow(tr("Modded class"), m_base);

    m_preview->setReadOnly(true);
    m_preview->setFont(theme::monoFont());
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview->setTabStopDistance(QFontMetrics(m_preview->font())
                                      .horizontalAdvance(QLatin1Char(' '))
                                  * 4);
    // Nine lines is the tallest skeleton, so the dialog keeps one height whatever
    // the choices are and the preview never scrolls to show its own last brace.
    m_preview->setFixedHeight(QFontMetrics(m_preview->font()).lineSpacing() * 9 + 12);
    new EnforceHighlighter(m_preview->document(), m_catalog);

    m_note->setWordWrap(true);
    m_note->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Two lines held open whatever the note says, so the dialog does not resize
    // under the pointer on every keystroke.
    m_note->setMinimumHeight(QFontMetrics(m_note->font()).lineSpacing() * 2 + 4);

    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Create script"));
    m_buttons->button(QDialogButtonBox::Ok)->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addLayout(m_form);
    layout->addWidget(m_preview);
    layout->addWidget(m_note);
    layout->addWidget(m_buttons);

    connect(m_name, &QLineEdit::textChanged, this, &NewScriptDialog::validateInput);
    connect(m_base, &QLineEdit::textChanged, this, &NewScriptDialog::validateInput);
    connect(m_kind, &QComboBox::currentIndexChanged, this,
            &NewScriptDialog::validateInput);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    setMinimumWidth(470);
    m_name->setFocus();
    validateInput();
}

NewScriptOptions NewScriptDialog::options() const
{
    NewScriptOptions opts;
    opts.className = m_name->text().trimmed();
    opts.baseClass = m_base->text().trimmed();
    opts.kind = static_cast<ScriptKind>(m_kind->currentData().toInt());
    return opts;
}

QString NewScriptDialog::targetPath() const
{
    const QString name = m_name->text().trimmed();
    if (name.isEmpty()) return {};
    return QDir(m_folder).absoluteFilePath(name + QStringLiteral(".c"));
}

void NewScriptDialog::validateInput()
{
    const NewScriptOptions opts = options();
    const bool modded = opts.kind == ScriptKind::ModdedClass;
    // The whole row, so a new class and an empty file do not leave a labelled
    // gap where the base used to be.
    m_form->setRowVisible(m_base, modded);

    QString problem;
    QString warning;
    QString reason;
    if (!isValidScriptName(opts.className, &reason))
        problem = reason;
    else if (modded && opts.baseClass.isEmpty())
        problem = tr("Name the class this script reopens.");
    else if (modded && !isValidScriptName(opts.baseClass, &reason))
        problem = reason;
    else if (QFileInfo::exists(targetPath()))
        problem = tr("%1.c is already in %2.")
                      .arg(opts.className, QDir(m_folder).dirName());

    if (problem.isEmpty() && m_catalog && m_catalog->isLoaded()) {
        if (modded && m_catalog->classId(opts.baseClass) < 0)
            warning = tr("The catalogue has no class called %1. That is expected for "
                         "a class another mod declares, and a compile error "
                         "otherwise.")
                          .arg(opts.baseClass);
        else if (!modded && m_catalog->classId(opts.className) >= 0)
            warning = tr("Vanilla already declares %1. Declaring it a second time "
                         "does not compile, so reopen it as a modded class instead.")
                          .arg(opts.className);
    }

    m_preview->setPlainText(problem.isEmpty() ? scriptSkeleton(opts, m_catalog)
                                              : QString());

    if (!problem.isEmpty()) {
        m_note->setText(problem);
        setSeverity(m_note, "error");
    } else if (!warning.isEmpty()) {
        m_note->setText(warning);
        setSeverity(m_note, "warning");
    } else {
        m_note->setText(tr("Writes %1").arg(QDir::toNativeSeparators(targetPath())));
        setSeverity(m_note, "note");
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
}

QString NewScriptDialog::run(QWidget *parent, const QString &folder,
                             const Catalog *catalog)
{
    NewScriptDialog dialog(parent, folder, catalog);
    while (dialog.exec() == QDialog::Accepted) {
        const NewScriptOptions opts = dialog.options();
        const QString path =
            QDir(folder).absoluteFilePath(opts.className + QStringLiteral(".c"));
        const QByteArray text = scriptSkeleton(opts, catalog).toUtf8();

        QFile file(path);
        // NewOnly rather than WriteOnly: a file that appeared between the dialog
        // and here is somebody's work and must not be truncated to make room.
        bool written = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        if (written) {
            written = file.write(text) == text.size();
            file.close();
            written = written && file.error() == QFileDevice::NoError;
            // Half a file left on disk would block the retry below on a name
            // that never got its contents.
            if (!written) file.remove();
        }
        if (!written) {
            QMessageBox::warning(parent, tr("New script"),
                                 tr("Could not write %1.")
                                     .arg(QDir::toNativeSeparators(path)));
            continue;
        }
        return path;
    }
    // Cancelled, or every attempt was reported as it failed.
    return {};
}
