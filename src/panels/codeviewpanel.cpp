#include "codeviewpanel.h"

#include "codegen.h"
#include "document.h"
#include "theme.h"
#include "widgets/codeeditor.h"

#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

constexpr int kHeaderHeight = 22;
constexpr int kButtonHeight = 18;
// Long enough that dragging a node does not regenerate on every frame, short
// enough that the file has caught up by the time the eye gets there.
constexpr int kDebounceMs = 200;

// Wash behind the lines one node produced. Low alpha so the highlighting still
// reads through it and the caret row stays the brighter of the two.
QColor ownerTint()
{
    QColor tint = theme::accent();
    tint.setAlpha(46);
    return tint;
}

// QWidget has no "became visible" signal, and regenerate() does nothing while
// the dock is hidden, so something has to kick it when the dock comes back.
// A filter does that without a showEvent override in the header.
class ShowWatcher : public QObject {
public:
    ShowWatcher(QObject *parent, std::function<void()> onShow)
        : QObject(parent), m_onShow(std::move(onShow))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show && m_onShow) m_onShow();
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_onShow;
};

QToolButton *barButton(QWidget *parent, const QString &text, const QString &tip)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    // The pane is for reading. Tabbing into its buttons only steals focus from
    // the canvas the user is actually driving.
    button->setFocusPolicy(Qt::NoFocus);
    button->setFont(theme::uiFont(8));
    button->setFixedHeight(kButtonHeight);
    // Toolbar padding is too generous for a strip this short: at 18px it eats
    // the descenders and "Copy" comes out as "Copv".
    button->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 6px; }"));
    return button;
}

// Qt's %n plural idiom needs a translator to pick a form, and there is none
// here, so it would put "1 line(s)" on the bar.
QString countPhrase(int n, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? singular : plural);
}

// A negative count means nothing has been generated yet, which is not the same
// as a file of no lines.
void setHeaderText(QLabel *label, const QString &className, int lines)
{
    const QString name = className.isEmpty() ? QObject::tr("Untitled") : className;
    QString text = QStringLiteral("<span style=\"color:%1\">%2.c</span>")
                       .arg(theme::text().name(), name.toHtmlEscaped());
    if (lines >= 0)
        text += QStringLiteral("<span style=\"color:%1\">&nbsp;&nbsp;%2</span>")
                    .arg(theme::textDim().name(),
                         countPhrase(lines, QObject::tr("line"), QObject::tr("lines")));
    label->setText(text);
}

void setWarningText(QLabel *label, const QStringList &warnings)
{
    const int count = warnings.size();
    label->setText(count == 0 ? QObject::tr("No warnings")
                              : countPhrase(count, QObject::tr("warning"),
                                            QObject::tr("warnings")));
    const QString sheet = QStringLiteral("color: %1")
                              .arg(count == 0 ? theme::textDim().name()
                                              : theme::warningColor().name());
    // Reassigning the same sheet re-polishes the label on every regeneration.
    if (label->styleSheet() != sheet) label->setStyleSheet(sheet);
    label->setToolTip(warnings.join(QLatin1Char('\n')));
}

int firstLineOwnedBy(const QVector<QString> &owners, const QString &nodeId)
{
    if (nodeId.isEmpty()) return -1;
    for (int i = 0; i < owners.size(); ++i)
        if (owners.at(i) == nodeId) return i;
    return -1;
}

// Washes every line one node produced, so its contribution to the file reads as
// a block rather than as a line the caret happens to sit on. Passing an empty
// id clears the wash.
void markOwnerLines(CodeEditor *editor, const QVector<QString> &owners,
                    const QString &nodeId)
{
    const QColor tint = ownerTint();

    // The caret row and the matched bracket pair live in the same list and
    // belong to the editor, so only the previous wash is dropped here.
    QList<QTextEdit::ExtraSelection> selections;
    for (const QTextEdit::ExtraSelection &sel : editor->extraSelections())
        if (sel.format.background().color() != tint) selections.append(sel);

    QTextDocument *doc = editor->document();
    for (int i = 0; !nodeId.isEmpty() && i < owners.size(); ++i) {
        if (owners.at(i) != nodeId) continue;
        const QTextBlock block = doc->findBlockByNumber(i);
        if (!block.isValid()) continue;
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(tint);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(block);
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    editor->setExtraSelections(selections);
}

} // namespace

CodeViewPanel::CodeViewPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_editor(new CodeEditor(this)),
      m_header(new QLabel(this)), m_warnings(new QLabel(this)),
      m_debounce(new QTimer(this)),
      m_liveToggle(barButton(this, tr("Live"),
                             tr("Regenerate as the graph changes")))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour
    // instead of the panel colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("codeViewBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(kHeaderHeight);
    bar->setStyleSheet(QStringLiteral("#codeViewBar { background: %1; "
                                      "border-bottom: 1px solid %2; }")
                           .arg(theme::headerBg().name(), theme::border().name()));

    m_header->setFont(theme::uiFont(8));
    m_header->setTextFormat(Qt::RichText);
    m_warnings->setFont(theme::uiFont(8));

    QToolButton *copy = barButton(bar, tr("Copy"),
                                  tr("Copy the whole script to the clipboard"));
    QToolButton *save = barButton(bar, tr("Save as"),
                                  tr("Write this script out as a .c file"));
    m_liveToggle->setCheckable(true);
    m_liveToggle->setChecked(m_live);

    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(7, 0, 4, 0);
    row->setSpacing(6);
    row->addWidget(m_header);
    row->addStretch(1);
    row->addWidget(m_warnings);
    row->addSpacing(4);
    row->addWidget(m_liveToggle);
    row->addWidget(copy);
    row->addWidget(save);

    m_editor->setDocumentContext(m_doc);
    m_editor->setReadOnly(true);
    // Read-only still has to select and copy, which is most of what a generated
    // file is good for.
    m_editor->setTextInteractionFlags(Qt::TextSelectableByMouse
                                      | Qt::TextSelectableByKeyboard);
    // Same size the editor picks for itself: it derived its tab stops from
    // those metrics once, in its constructor, and never again.
    m_editor->setFont(theme::monoFont(9));
    m_editor->setFrameShape(QFrame::NoFrame);
    // The app sheet gives every QPlainTextEdit a border and padding, which
    // reads as a text box dropped into a panel rather than as a pane of code.
    m_editor->setStyleSheet(
        QStringLiteral("QPlainTextEdit { background: %1; color: %2; border: none; "
                       "padding: 0px; selection-background-color: %3; }")
            .arg(theme::windowBg().name(), theme::text().name(),
                 theme::accent().name()));

    outer->addWidget(bar);
    outer->addWidget(m_editor, 1);

    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &CodeViewPanel::regenerate);

    connect(m_liveToggle, &QToolButton::toggled, this, &CodeViewPanel::setLive);
    connect(copy, &QToolButton::clicked, this, &CodeViewPanel::copyAll);
    connect(save, &QToolButton::clicked, this, &CodeViewPanel::saveAs);
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this,
            &CodeViewPanel::onCursorMoved);

    if (m_doc) {
        connect(m_doc, &Document::graphChanged, this, &CodeViewPanel::scheduleRefresh);
        connect(m_doc, &Document::activeScriptChanged, this,
                &CodeViewPanel::scheduleRefresh);
        connect(m_doc, &Document::projectChanged, this, &CodeViewPanel::scheduleRefresh);
    }

    installEventFilter(new ShowWatcher(this, [this]() { scheduleRefresh(); }));

    setHeaderText(m_header, m_doc && m_doc->activeGraph()
                                ? m_doc->activeGraph()->className
                                : QString(),
                  -1);
    setWarningText(m_warnings, {});
    scheduleRefresh();
}

void CodeViewPanel::setLive(bool live)
{
    m_live = live;
    if (m_liveToggle->isChecked() != live) {
        const QSignalBlocker blocker(m_liveToggle);
        m_liveToggle->setChecked(live);
    }
    // Coming back on with a file from three edits ago is worse than showing no
    // file at all, so catch up before the user reads a stale line.
    if (live) regenerate();
}

void CodeViewPanel::scheduleRefresh()
{
    m_debounce->start();
}

void CodeViewPanel::regenerate()
{
    // Generating a large graph for a dock nobody can see is work thrown away.
    if (!m_live || !isVisible()) return;

    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    if (!graph) {
        m_lineOwners.clear();
        m_revealed.clear();
        m_syncingCursor = true;
        m_editor->clear();
        m_syncingCursor = false;
        setHeaderText(m_header, QString(), -1);
        setWarningText(m_warnings, {});
        return;
    }

    const GenResult result = generateEnforce(*graph, m_doc->catalog(),
                                             m_doc->builtins(), m_doc->project());

    // Regeneration replaces the whole document, and a replaced document starts
    // at line 1. Without putting the view back the pane scrolls itself to the
    // top every time the graph changes, which is every drag of a node.
    QScrollBar *vbar = m_editor->verticalScrollBar();
    QScrollBar *hbar = m_editor->horizontalScrollBar();
    const int scrollY = vbar->value();
    const int scrollX = hbar->value();
    const QTextCursor before = m_editor->textCursor();
    const int line = before.blockNumber();
    const int column = before.positionInBlock();

    m_syncingCursor = true;
    // The editor already takes variables and functions from the document; the
    // class it is being generated into is the one name it cannot know.
    m_editor->setLocalSymbols({graph->className, graph->baseClass});
    m_editor->setPlainText(result.code);

    const QTextBlock block = m_editor->document()->findBlockByNumber(
        qBound(0, line, m_editor->blockCount() - 1));
    if (block.isValid()) {
        QTextCursor cursor(block);
        cursor.setPosition(block.position() + qMin(column, block.length() - 1));
        m_editor->setTextCursor(cursor);
    }
    // After the cursor: setTextCursor scrolls to keep it visible, which would
    // undo the restore if it ran second.
    vbar->setValue(qMin(scrollY, vbar->maximum()));
    hbar->setValue(qMin(scrollX, hbar->maximum()));
    m_syncingCursor = false;

    m_lineOwners = result.lineOwners;
    setHeaderText(m_header, graph->className, m_editor->blockCount());
    setWarningText(m_warnings, result.warnings);

    // The canvas selection did not change, so the marked block should survive
    // the regeneration even though the lines under it were rewritten.
    if (firstLineOwnedBy(m_lineOwners, m_revealed) < 0) m_revealed.clear();
    markOwnerLines(m_editor, m_lineOwners, m_revealed);
}

void CodeViewPanel::onCursorMoved()
{
    // The round trip is panel selects node, node reveals lines, lines move the
    // cursor. Without the guard that last step starts the trip over.
    if (m_syncingCursor) return;

    const int line = m_editor->textCursor().blockNumber();
    const QString owner = line >= 0 && line < m_lineOwners.size()
                              ? m_lineOwners.at(line)
                              : QString();
    // Class header, member declarations and the user region belong to no node.
    // Leave the last mark standing rather than clearing it on the way past.
    if (owner.isEmpty()) return;

    const bool changed = owner != m_revealed;
    m_revealed = owner;
    // The editor rewrites its extra selections on every cursor move, so the
    // wash goes back on after it rather than before.
    markOwnerLines(m_editor, m_lineOwners, owner);
    if (changed) emit nodeActivated(owner);
}

void CodeViewPanel::revealNode(const QString &nodeId)
{
    // Nothing generated yet, because the dock was hidden or live is off. Hold
    // the node so the next regeneration marks it instead of losing it.
    if (m_lineOwners.isEmpty()) {
        m_revealed = nodeId;
        return;
    }

    const int line = firstLineOwnedBy(m_lineOwners, nodeId);
    if (line < 0) {
        // A pure node inlined into an expression owns no line of its own, and
        // marking the statement that consumed it would point at the wrong node.
        m_revealed.clear();
        markOwnerLines(m_editor, m_lineOwners, QString());
        return;
    }

    const QTextBlock block = m_editor->document()->findBlockByNumber(line);
    if (!block.isValid()) return;

    m_revealed = nodeId;
    m_syncingCursor = true;
    m_editor->setTextCursor(QTextCursor(block));
    // Centred rather than merely visible: the dock is short, and a line pinned
    // to its bottom edge shows none of the block it belongs to.
    m_editor->centerCursor();
    m_syncingCursor = false;

    markOwnerLines(m_editor, m_lineOwners, nodeId);
}

void CodeViewPanel::copyAll()
{
    QGuiApplication::clipboard()->setText(m_editor->toPlainText());
}

void CodeViewPanel::saveAs()
{
    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    const QString name = graph && !graph->className.isEmpty()
                             ? graph->className
                             : QStringLiteral("Generated");

    // Next to the project, where the mod's script tree already is.
    const QString folder = m_doc && !m_doc->projectPath().isEmpty()
                               ? QFileInfo(m_doc->projectPath()).absolutePath()
                               : QString();
    const QString file = name + QStringLiteral(".c");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save generated script"),
        folder.isEmpty() ? file : QDir(folder).filePath(file),
        tr("Enforce Script (*.c)"));
    if (path.isEmpty()) return;

    // UTF-8 with no BOM: the Enfusion compiler reads a script as plain bytes
    // and a BOM lands in the first token.
    QSaveFile out(path);
    const QByteArray bytes = m_editor->toPlainText().toUtf8();
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()
        || !out.commit()) {
        QMessageBox::warning(this, tr("Save generated script"),
                             tr("Could not write %1.\n\n%2")
                                 .arg(QFileInfo(path).fileName(), out.errorString()));
    }
}
