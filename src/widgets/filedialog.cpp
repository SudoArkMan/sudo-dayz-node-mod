#include "filedialog.h"

#include "document.h"
#include "theme.h"
#include "widgets/codeeditor.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QVBoxLayout>

namespace {

// Every open window, keyed by the file it is on, so activating a path twice
// raises the first window rather than opening a second editor over the same
// bytes. Cleared from the destructor.
QHash<QString, FileDialog *> &openWindows()
{
    static QHash<QString, FileDialog *> windows;
    return windows;
}

QString windowKey(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

// A mod folder is mostly not text: .paa, .p3d and .pbo sit right beside the
// files worth editing, and loading one into a text box wastes a minute and
// shows nothing. Refuse by size first, then on the first NUL byte.
constexpr qint64 kSizeLimit = 8 * 1024 * 1024;

bool looksBinary(const QByteArray &bytes)
{
    const int scan = static_cast<int>(qMin<qsizetype>(bytes.size(), 4096));
    for (int i = 0; i < scan; ++i)
        if (bytes.at(i) == '\0') return true;
    return false;
}

// Enforce Script by extension. These are the files where catalogue completion
// and the brace balance check are worth having; everything else the explorer
// hands over is data, and both would only get in the way.
bool isEnforceFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("c") || suffix == QLatin1String("cpp")
           || suffix == QLatin1String("h") || suffix == QLatin1String("hpp");
}

void applyStatus(QLabel *label, const CodeEditor::Status &status)
{
    if (!label) return;
    QStringList notes = status.problems;
    notes.removeDuplicates();

    const int shown = 3;
    if (notes.size() > shown) {
        const int rest = notes.size() - shown;
        notes = notes.mid(0, shown);
        notes << QStringLiteral("and %1 more.").arg(rest);
    }

    const bool clean = notes.isEmpty();
    label->setText(clean ? QStringLiteral("Balanced. Nothing to flag.")
                         : notes.join(QStringLiteral("  ")));
    label->setStyleSheet(QStringLiteral("color: %1")
                             .arg(clean ? theme::textDim().name()
                                        : theme::errorColor().name()));
}

void setNote(QLabel *label, const QString &text)
{
    if (!label) return;
    label->setText(text);
    label->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
}

} // namespace

FileDialog::FileDialog(QWidget *parent, Document *doc, const QString &path)
    : QDialog(parent), m_doc(doc), m_path(QDir::cleanPath(QFileInfo(path).absoluteFilePath())),
      m_editor(new CodeEditor(this)), m_status(new QLabel(this)),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close,
                                     this)),
      m_enforce(isEnforceFile(path))
{
    // A file window is not modal and does not block the canvas, so it gets a
    // real window frame: a dialog frame has no minimise button, and this window
    // stays open as long as the file is being worked on.
    setWindowFlags(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, true);

    // Highlighting stays on whatever the file is: quoted values, numbers and
    // comments read the same way in an .xml as in a .c. Completion is the part
    // that would be wrong, and that only comes with the document context.
    if (m_enforce) m_editor->setDocumentContext(m_doc);

    m_status->setFont(theme::uiFont(8));
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);

    connect(m_editor, &CodeEditor::statusChanged, this,
            [this](const CodeEditor::Status &s) {
                if (m_enforce) applyStatus(m_status, s);
            });
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_loading) setDirty(true);
    });

    // Save leaves the window open: this is an editor, not a prompt.
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this]() {
        QString error;
        if (!save(&error))
            QMessageBox::warning(this, tr("Save file"), error);
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &FileDialog::reject);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this]() {
        if (QPushButton *button = m_buttons->button(QDialogButtonBox::Save))
            button->click();
    });

    QSettings settings;
    const QSize remembered = settings.value(QStringLiteral("fileDialog/size")).toSize();
    resize(remembered.isValid() ? remembered : QSize(860, 640));

    if (QPushButton *button = m_buttons->button(QDialogButtonBox::Save))
        button->setEnabled(false);
    updateTitle();
}

FileDialog::~FileDialog()
{
    // The window closes rather than finishing, so the remembered size is
    // written here instead of from QDialog::finished.
    QSettings settings;
    settings.setValue(QStringLiteral("fileDialog/size"), size());

    auto &windows = openWindows();
    for (auto it = windows.begin(); it != windows.end(); ++it) {
        if (it.value() != this) continue;
        windows.erase(it);
        break;
    }
}

FileDialog *FileDialog::openFile(QWidget *parent, Document *doc, const QString &path,
                                 QString *error)
{
    const QString key = windowKey(path);
    if (FileDialog *existing = openWindows().value(key)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return existing;
    }

    auto *window = new FileDialog(parent, doc, path);
    if (!window->load(error)) {
        delete window;
        return nullptr;
    }
    openWindows().insert(key, window);
    window->show();
    return window;
}

bool FileDialog::load(QString *error)
{
    const QFileInfo info(m_path);
    if (info.size() > kSizeLimit) {
        if (error)
            *error = tr("%1 is %2 MB. Open it in an editor built for files that size.")
                         .arg(info.fileName())
                         .arg(info.size() / (1024.0 * 1024.0), 0, 'f', 1);
        return false;
    }

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Cannot read %1.").arg(m_path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (looksBinary(bytes)) {
        if (error) *error = tr("%1 is not a text file.").arg(info.fileName());
        return false;
    }

    m_crlf = bytes.contains("\r\n");
    QString text = QString::fromUtf8(bytes);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    m_loading = true;
    m_editor->setPlainText(text);
    m_loading = false;
    setDirty(false);

    if (m_enforce)
        applyStatus(m_status, m_editor->validate());
    else
        setNote(m_status, QDir::toNativeSeparators(m_path));
    return true;
}

bool FileDialog::save(QString *error)
{
    QString text = m_editor->toPlainText();
    if (m_crlf) text.replace(QLatin1String("\n"), QLatin1String("\r\n"));

    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("Cannot write %1.").arg(m_path);
        return false;
    }
    out.write(text.toUtf8());
    if (!out.commit()) {
        if (error) *error = tr("Failed to save %1.").arg(m_path);
        return false;
    }

    setDirty(false);
    if (!m_enforce) setNote(m_status, tr("Saved %1").arg(QFileInfo(m_path).fileName()));
    return true;
}

void FileDialog::setDirty(bool dirty)
{
    if (m_dirty == dirty) return;
    m_dirty = dirty;
    if (QPushButton *button = m_buttons->button(QDialogButtonBox::Save))
        button->setEnabled(dirty);
    updateTitle();
}

void FileDialog::updateTitle()
{
    const QFileInfo info(m_path);
    setWindowTitle(QStringLiteral("%1%2 - %3")
                       .arg(info.fileName(),
                            m_dirty ? QStringLiteral(" *") : QString(),
                            QDir::toNativeSeparators(info.absolutePath())));
}

void FileDialog::reject()
{
    // Not QDialog::reject: that hides the window without a close event, which
    // would step straight past the unsaved-changes check.
    close();
}

void FileDialog::closeEvent(QCloseEvent *event)
{
    if (!m_dirty) {
        event->accept();
        return;
    }

    // Deliberately not calling QDialog::closeEvent, which would call reject()
    // and come back through here.
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has changes that are not written to disk.")
            .arg(QFileInfo(m_path).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (answer == QMessageBox::Save) {
        QString error;
        if (!save(&error)) {
            QMessageBox::warning(this, tr("Save file"), error);
            event->ignore();
            return;
        }
    }
    event->accept();
}
