#include "codedialog.h"

#include "document.h"
#include "enforce/lexer.h"
#include "theme.h"
#include "widgets/codeeditor.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QVBoxLayout>

namespace {

// The editor's own indent and newline handling runs before QPlainTextEdit's
// read-only guard, so in the generated-script view Tab, Return and a typed '}'
// would still rewrite text the user is only meant to read. Selection and copy
// come through untouched.
class ReadOnlyKeys : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress)
            return QObject::eventFilter(watched, event);
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return true;
        default:
            break;
        }
        return key->text() == QLatin1String("}");
    }
};

// Comment prose has been stored under four different keys across builds of the
// reference app. Write back to the key the node already carries or the canvas
// goes on drawing the old text.
QString codeKey(const GraphNode &node)
{
    if (node.kind != NodeKind::Comment && node.ref != bi::Comment)
        return QStringLiteral("code");
    for (const char *key : {"text", "code", "comment", "note"})
        if (!node.opts.value(QString::fromLatin1(key)).isEmpty())
            return QString::fromLatin1(key);
    return QStringLiteral("text");
}

// Names the surrounding class declares. The editor treats them as known, so a
// reference to the graph's own member is not flagged as a typo.
QStringList graphSymbols(const Graph &g)
{
    QStringList names{g.className};
    for (const GraphVariable &v : g.variables) names << v.name;
    for (const GraphFunction &f : g.functions) {
        names << f.name;
        for (const GraphParam &p : f.params) names << p.name;
    }
    names.removeAll(QString());
    names.removeDuplicates();
    return names;
}

// Wording for the save prompt. The status line does not use it: validate()
// already phrases every balance problem, and a second phrasing of the same
// count reads as two separate faults.
QString braceNote(int balance)
{
    if (balance > 0)
        return balance == 1 ? QStringLiteral("One brace is left open.")
                            : QStringLiteral("%1 braces are left open.").arg(balance);
    if (balance < 0)
        return balance == -1
                   ? QStringLiteral("One closing brace has nothing to close.")
                   : QStringLiteral("%1 closing braces have nothing to close.")
                         .arg(-balance);
    return QString();
}

void applyStatus(QLabel *label, const CodeEditor::Status &status)
{
    if (!label) return;
    QStringList notes = status.problems;
    notes.removeDuplicates();

    // A whole generated file can report dozens of findings, and a status line
    // that grows to twenty rows pushes the code itself out of the dialog.
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

QString sizeKey(bool readOnly)
{
    return readOnly ? QStringLiteral("codeDialog/viewSize")
                    : QStringLiteral("codeDialog/editSize");
}

} // namespace

CodeDialog::CodeDialog(QWidget *parent, Document *doc, bool readOnly)
    : QDialog(parent), m_editor(new CodeEditor(this)), m_status(new QLabel(this)),
      m_warnings(new QLabel(this)),
      m_buttons(new QDialogButtonBox(readOnly ? QDialogButtonBox::Close
                                              : QDialogButtonBox::Save
                                                    | QDialogButtonBox::Cancel,
                                     this)),
      m_readOnly(readOnly)
{
    setModal(true);

    m_editor->setDocumentContext(doc);
    m_editor->setReadOnly(readOnly);
    if (readOnly) {
        // Read-only still has to select and copy, which is the only reason the
        // generated script is worth showing at all.
        m_editor->setTextInteractionFlags(Qt::TextSelectableByMouse
                                          | Qt::TextSelectableByKeyboard);
        m_editor->installEventFilter(new ReadOnlyKeys(this));
    }

    m_status->setFont(theme::uiFont(8));
    m_status->setWordWrap(true);

    m_warnings->setFont(theme::uiFont(8));
    m_warnings->setWordWrap(true);
    m_warnings->setStyleSheet(QStringLiteral("color: %1").arg(theme::warningColor().name()));
    m_warnings->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_warnings);
    layout->addWidget(m_buttons);

    if (readOnly) {
        QPushButton *copy = m_buttons->addButton(tr("Copy all"),
                                                 QDialogButtonBox::ActionRole);
        connect(copy, &QPushButton::clicked, this, [this]() {
            QGuiApplication::clipboard()->setText(m_editor->toPlainText());
        });
    }

    connect(m_editor, &CodeEditor::statusChanged, this,
            [this](const CodeEditor::Status &s) { applyStatus(m_status, s); });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (readOnly) {
        connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    } else {
        // An unbalanced raw block does not stop at its own node: the brace it
        // leaves open swallows whatever codegen writes after it, so the whole
        // file stops compiling. Warn, but let the user save a half-written
        // block if that is what they meant.
        connect(m_buttons, &QDialogButtonBox::accepted, this, [this]() {
            const CodeEditor::Status s = m_editor->validate();
            if (s.braceBalance == 0) {
                accept();
                return;
            }
            QMessageBox box(QMessageBox::Warning, tr("Unbalanced braces"),
                            braceNote(s.braceBalance) + QLatin1Char('\n')
                                + tr("Saved as it stands, this block breaks every "
                                     "line the generated file writes after it."),
                            QMessageBox::NoButton, this);
            QPushButton *save = box.addButton(tr("Save anyway"),
                                              QMessageBox::AcceptRole);
            box.addButton(tr("Keep editing"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == save) accept();
        });

        auto *commit = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
        connect(commit, &QShortcut::activated, this, [this]() {
            if (QPushButton *save = m_buttons->button(QDialogButtonBox::Save))
                save->click();
        });
    }

    QSettings settings;
    const QSize remembered = settings.value(sizeKey(readOnly)).toSize();
    resize(remembered.isValid() ? remembered
                                : (readOnly ? QSize(880, 700) : QSize(720, 460)));
    connect(this, &QDialog::finished, this, [this](int) {
        QSettings out;
        out.setValue(sizeKey(m_readOnly), size());
    });

    onStatusChanged();
}

void CodeDialog::setCode(const QString &code)
{
    m_editor->setPlainText(code);
    onStatusChanged();
}

QString CodeDialog::code() const
{
    return m_editor->toPlainText();
}

void CodeDialog::setWarnings(const QStringList &warnings)
{
    m_warnings->setText(warnings.join(QLatin1Char('\n')));
    m_warnings->setVisible(!warnings.isEmpty());
}

void CodeDialog::onStatusChanged()
{
    applyStatus(m_status, m_editor->validate());
}

bool CodeDialog::editNodeCode(QWidget *parent, Document *doc, const QString &nodeId)
{
    Graph *graph = doc ? doc->activeGraph() : nullptr;
    const GraphNode *node = graph ? graph->node(nodeId) : nullptr;
    if (!node) return false;

    const QString key = codeKey(*node);
    const QString before = node->opts.value(key);
    const bool comment = node->kind == NodeKind::Comment || node->ref == bi::Comment;

    const ScriptEntry *entry = doc->project().script(doc->activeScriptId());
    const QString script = entry && !entry->name.isEmpty() ? entry->name : graph->className;
    QString summary = enforceSummary(before, 48);
    if (summary.isEmpty())
        summary = comment ? QStringLiteral("Comment") : QStringLiteral("Raw Enforce");

    CodeDialog dialog(parent, doc, false);
    dialog.setWindowTitle(QStringLiteral("%1 - %2.c").arg(summary, script));
    dialog.m_editor->setLocalSymbols(graphSymbols(*graph));
    dialog.setCode(before);
    dialog.m_editor->setFocus();

    if (dialog.exec() != QDialog::Accepted) return false;

    const QString after = dialog.code();
    if (after == before) return true;

    doc->beginEdit(QStringLiteral("Edit code"));
    // The snapshot is taken inside beginEdit, so the live graph is re-read here
    // rather than trusting a pointer taken before it.
    Graph *live = doc->activeGraph();
    if (GraphNode *target = live ? live->node(nodeId) : nullptr)
        target->opts.insert(key, after);
    doc->commitEdit();
    return true;
}

void CodeDialog::showGenerated(QWidget *parent, Document *doc, const QString &title,
                               const QString &code, const QStringList &warnings)
{
    CodeDialog dialog(parent, doc, true);
    dialog.setWindowTitle(title);
    dialog.setCode(code);
    dialog.setWarnings(warnings);
    dialog.exec();
}
