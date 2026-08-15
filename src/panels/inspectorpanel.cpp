#include "inspectorpanel.h"

#include "analysis.h"
#include "codegen.h"
#include "document.h"
#include "enforce/highlighter.h"
#include "theme.h"
#include "variablespanel.h"
#include "widgets/codedialog.h"
#include "widgets/valueeditor.h"

#include <QCheckBox>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Both option groups are a container holding their control plus a label named
// "note", so the panel can show, hide and re-word them without the header
// carrying a member for every piece.
QWidget *optionBox(QWidget *control)
{
    return control ? control->parentWidget() : nullptr;
}

QLabel *optionNote(QWidget *control)
{
    QWidget *box = optionBox(control);
    return box ? box->findChild<QLabel *>(QStringLiteral("note")) : nullptr;
}

bool isCommentNode(const GraphNode &node)
{
    return node.kind == NodeKind::Comment || node.ref == bi::Comment;
}

// Nodes whose content is hand-written text rather than pins.
bool isCodeNode(const GraphNode &node)
{
    return node.ref == bi::Raw || node.ref == QLatin1String("bi.rawExpr")
           || isCommentNode(node);
}

// The median raw node in a real project is one line, so a fixed block would be
// mostly empty; six is where it starts crowding the options below it. Code does
// not wrap, so the horizontal bar gets its own room instead of eating a line.
int codePreviewHeight(const QPlainTextEdit *preview, int lines)
{
    const int rows = QFontMetrics(theme::monoFont(8)).lineSpacing() * qBound(2, lines, 6);
    const int bar = preview ? preview->horizontalScrollBar()->sizeHint().height() : 0;
    return rows + bar + 12;
}

// Comment prose has been stored under four different keys across builds of the
// reference app; a raw node has only ever used "code".
QString codeOf(const GraphNode &node)
{
    if (!isCommentNode(node)) return node.opts.value(QStringLiteral("code"));
    for (const char *key : {"text", "code", "comment", "note"}) {
        const QString v = node.opts.value(QString::fromLatin1(key));
        if (!v.isEmpty()) return v;
    }
    return QString();
}

// The help strings are written with `backticks` around identifiers, the way the
// reference build shows them.
QString richText(const QString &plain)
{
    QString out = plain.toHtmlEscaped();
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    out.replace(code, QStringLiteral("<span style=\"font-family:%1\">\\1</span>")
                          .arg(theme::monoFont().family()));
    return out;
}

QString bulletList(const QStringList &items, const QColor &colour)
{
    if (items.isEmpty()) return QString();
    QString rows;
    for (const QString &item : items)
        rows += QStringLiteral("<li style=\"margin-bottom:3px\">%1</li>").arg(richText(item));
    return QStringLiteral("<ul style=\"margin:0 0 9px 0; color:%1; -qt-list-indent:1\">%2</ul>")
        .arg(colour.name(), rows);
}

QString detailRow(const QString &key, const QString &value)
{
    if (value.isEmpty()) return QString();
    return QStringLiteral("<tr><td style=\"padding-right:10px; color:%1\">%2</td>"
                          "<td>%3</td></tr>")
        .arg(theme::textDim().name(), key.toHtmlEscaped(), value.toHtmlEscaped());
}

// QHash has no order of its own. This is the order the reference build shows,
// earliest moment first, which is also the order the choice matters in.
QStringList orderedBeginModes(const Builtins &builtins)
{
    static const QStringList canonical{QStringLiteral("init"), QStringLiteral("construct"),
                                       QStringLiteral("deferred"), QStringLiteral("afterLoad")};
    QStringList out;
    for (const QString &key : canonical)
        if (builtins.beginModes().contains(key)) out << key;
    QStringList rest = builtins.beginModes().keys();
    rest.sort();
    for (const QString &key : rest)
        if (!out.contains(key)) out << key;
    return out;
}

// Everything the catalogue cannot explain: variable access, builtins, and refs
// that answer to nothing at all.
NodeHelp fallbackHelp(const Document &doc, const GraphNode &node, const NodeDef &def)
{
    NodeHelp help;
    help.valid = true;

    if (node.kind == NodeKind::VarGet || node.kind == NodeKind::VarSet) {
        const bool get = node.kind == NodeKind::VarGet;
        const Graph *graph = doc.activeGraph();
        const GraphVariable *var = graph ? variableForRef(*graph, node.ref) : nullptr;
        if (!var) {
            help.kind = QStringLiteral("Variable");
            help.summary = QStringLiteral(
                "The variable this node refers to is no longer declared on the class.");
            help.cautions << QStringLiteral(
                "Delete the node or recreate the variable, or the generated script "
                "will not compile.");
            return help;
        }
        help.kind = QStringLiteral("Variable - %1").arg(var->type);
        help.summary = get
                           ? QStringLiteral("Reads the class member `%1`.").arg(var->name)
                           : QStringLiteral("Writes a new value into the class member `%1`.")
                                 .arg(var->name);
        help.effects << (get
                             ? QStringLiteral("No exec pins: it reads inline where it is used.")
                             : QStringLiteral("Emits `%1 = value;`.").arg(var->name));
        QStringList flags;
        if (var->sync) flags << QStringLiteral("network-synced");
        if (var->persist) flags << QStringLiteral("saved to storage");
        if (!flags.isEmpty())
            help.effects << QStringLiteral("Declared as %1.")
                                .arg(flags.join(QStringLiteral(" and ")));
        if (var->sync && !get)
            help.cautions << QStringLiteral(
                "Synced variables only reach clients after `SetSynchDirty()`, so call "
                "it after the last write.");
        help.documented = true;
        return help;
    }

    if (def.valid) {
        help.kind = def.category.isEmpty()
                        ? QStringLiteral("Builtin node")
                        : QStringLiteral("%1 - builtin node").arg(def.category);
        help.summary = def.doc;
        help.documented = !def.doc.isEmpty();
        help.effects << (def.pure
                             ? QStringLiteral("No exec pins: it evaluates where its output "
                                              "is used.")
                             : QStringLiteral("Runs as a statement in the flow."));
        help.source = def.loc;
        return help;
    }

    help.kind = QStringLiteral("Unknown node");
    help.summary = QStringLiteral(
                       "Nothing in the catalogue or the builtins answers to `%1`.")
                       .arg(node.ref);
    help.cautions << QStringLiteral(
        "The project was probably built against a different DayZ index. Rebuild the "
        "catalogue, or replace the node with one that still exists.");
    return help;
}

QString signatureOf(const Catalog &cat, const QString &ref)
{
    MethodSig sig = cat.method(ref);
    const bool isMethod = sig.valid;
    if (!sig.valid) sig = cat.globalFn(ref);
    if (!sig.valid) return QString();

    QStringList args;
    for (const MethodSig::Param &p : sig.params) {
        const QString prefix = p.dir == 1 ? QStringLiteral("out ")
                                          : p.dir == 2 ? QStringLiteral("inout ")
                                                       : QString();
        args << QStringLiteral("%1%2 %3").arg(prefix, p.type, p.name);
    }
    const QString name = isMethod && !sig.owner.isEmpty()
                             ? QStringLiteral("%1::%2").arg(sig.owner, sig.name)
                             : sig.name;
    return QStringLiteral("%1 %2(%3)").arg(sig.ret, name, args.join(QStringLiteral(", ")));
}

// ------------------------------------------------------------ variable form

// What the ref box stores against each row. The model keeps the decision
// tri-state, so "never" has to be a choice of its own rather than the absence
// of "always".
enum RefMode { RefInfer = 0, RefAlways = 1, RefNever = 2 };

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setFont(theme::uiFont(8, true));
    return label;
}

QLabel *noteLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setFont(theme::uiFont(8));
    label->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
    return label;
}

// The declaration exactly as the file will carry it, taken from the generator
// rather than composed here.
//
// Modifier order is part of Enforce's grammar and codegen.cpp is the only place
// that knows it, so the preview runs the real generator over a copy of the
// graph holding this one variable and reads its member line back out. A second
// copy of the ordering rules would be one refactor away from disagreeing with
// the file the user actually ships.
QString declarationFor(const Document &doc, const GraphVariable &var)
{
    const Graph *live = doc.activeGraph();
    const QString name = var.name.trimmed();
    const QString type = var.type.trimmed();
    if (!live || name.isEmpty() || type.isEmpty()) return QString();

    Graph probe = *live;
    probe.nodes.clear();
    probe.edges.clear();
    probe.functions.clear();
    probe.variables.clear();
    probe.variables.append(var);

    const GenResult gen = generateEnforce(probe, doc.catalog(), doc.builtins(),
                                          doc.project());
    // Members are the only thing the generator writes one tab in, and the probe
    // graph holds exactly one of them.
    const QString needle = type + QLatin1Char(' ') + name;
    const QStringList lines = gen.code.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!line.startsWith(QLatin1Char('\t')) || line.startsWith(QLatin1String("\t\t")))
            continue;
        if (line.contains(needle)) return line.trimmed();
    }
    return QString();
}

const GraphVariable *variableById(const Graph *graph, const QString &id)
{
    if (!graph || id.isEmpty()) return nullptr;
    for (const GraphVariable &v : graph->variables)
        if (v.id == id) return &v;
    return nullptr;
}

// True while the caret or a popup is inside this control, which is the moment
// refilling it from the graph would eat what is being typed. refresh runs on
// every graph change, including ones made from another panel.
bool isBusy(const QWidget *widget)
{
    if (!widget) return false;
    if (widget->hasFocus()) return true;
    const QWidget *focus = QApplication::focusWidget();
    if (focus && widget->isAncestorOf(focus)) return true;
    const auto *combo = qobject_cast<const QComboBox *>(widget);
    return combo && combo->view() && combo->view()->isVisible();
}

QColor severityColor(Severity severity)
{
    switch (severity) {
    case Severity::Error: return theme::errorColor();
    case Severity::Warning: return theme::warningColor();
    default: return theme::textDim();
    }
}

// What the analyser already said about this variable.
//
// Variable rules (DZ117 among them) are graph-level: there is no node to hang
// them on, so the analyser names the variable in the message instead, and the
// backticked name is what tells one variable's finding from another's.
QString findingsHtml(const AnalysisResult &result, const QString &name)
{
    if (name.isEmpty()) return QString();
    const QString token = QLatin1Char('`') + name + QLatin1Char('`');

    QString rows;
    for (const Diagnostic &d : result.diagnostics) {
        if (!d.nodeId.isEmpty() || !d.message.contains(token)) continue;
        rows += QStringLiteral("<p style=\"margin:0 0 6px 0; color:%1\">%2 <span "
                               "style=\"color:%3\">%4</span></p>")
                    .arg(severityColor(d.severity).name(), richText(d.message),
                         theme::textDim().name(), richText(d.hint));
    }
    return rows;
}

} // namespace

InspectorPanel::InspectorPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_title(new QLabel(this)), m_kind(new QLabel(this)),
      m_body(new QTextBrowser(this)), m_beginMode(nullptr), m_callSuper(nullptr),
      m_varPane(nullptr), m_varName(nullptr), m_varNameError(nullptr),
      m_varType(nullptr), m_varValue(nullptr), m_varDefaultWarning(nullptr),
      m_defaultCommit(nullptr), m_varPreview(nullptr),
      m_varSync(nullptr), m_varPersist(nullptr), m_varStatic(nullptr),
      m_varConst(nullptr), m_varPrivate(nullptr), m_varProtected(nullptr),
      m_varRef(nullptr), m_varRefNote(nullptr), m_varFindings(nullptr)
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour
    // instead of the panel colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    m_title->setFont(theme::uiFont(10, true));
    m_title->setWordWrap(true);
    layout->addWidget(m_title);

    m_kind->setFont(theme::uiFont(8));
    m_kind->setWordWrap(true);
    m_kind->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
    layout->addWidget(m_kind);

    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setOpenLinks(false);
    m_body->setOpenExternalLinks(false);
    m_body->document()->setDefaultFont(theme::uiFont(8));
    // setFrameShape loses to the app sheet's input-border rule, which counts a
    // QTextBrowser as an input. This is prose, not a field, so the border has
    // to be turned off here where a widget sheet outranks the app one.
    m_body->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    layout->addWidget(m_body, 1);

    buildVariableForm();
    layout->addWidget(m_varPane, 1);

    // A raw node's whole meaning is the text inside it, and the canvas can only
    // show one line of it. The panel carries the rest, plus the way in.
    auto *codeBox = new QWidget(this);
    codeBox->setObjectName(QStringLiteral("codeBox"));
    auto *codeLayout = new QVBoxLayout(codeBox);
    codeLayout->setContentsMargins(0, 6, 0, 0);
    codeLayout->setSpacing(3);

    auto *codeLabel = new QLabel(tr("Code"), codeBox);
    codeLabel->setObjectName(QStringLiteral("codeLabel"));
    codeLabel->setFont(theme::uiFont(8, true));
    codeLayout->addWidget(codeLabel);

    auto *preview = new QPlainTextEdit(codeBox);
    preview->setObjectName(QStringLiteral("codePreview"));
    preview->setReadOnly(true);
    preview->setFont(theme::monoFont(8));
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setStyleSheet(QStringLiteral("QPlainTextEdit { background: %1;"
                                          " border: 1px solid %2; }")
                               .arg(theme::canvasBg().name(), theme::border().name()));
    preview->setFixedHeight(codePreviewHeight(preview, 1));
    new EnforceHighlighter(preview->document(), m_doc ? &m_doc->catalog() : nullptr);
    codeLayout->addWidget(preview);

    auto *codeEdit = new QPushButton(tr("Edit code"), codeBox);
    codeEdit->setObjectName(QStringLiteral("codeEdit"));
    connect(codeEdit, &QPushButton::clicked, this, [this]() {
        if (!m_nodeId.isEmpty()) CodeDialog::editNodeCode(this, m_doc, m_nodeId);
    });
    codeLayout->addWidget(codeEdit, 0, Qt::AlignLeft);
    codeBox->setVisible(false);
    // Above the description, not below it: for a raw node the code is the
    // answer to what the node does, and the prose only explains the wrapper.
    layout->insertWidget(layout->indexOf(m_body), codeBox);

    auto *beginBox = new QWidget(this);
    auto *beginLayout = new QVBoxLayout(beginBox);
    beginLayout->setContentsMargins(0, 6, 0, 0);
    beginLayout->setSpacing(3);
    auto *beginLabel = new QLabel(tr("When does this run?"), beginBox);
    beginLabel->setFont(theme::uiFont(8, true));
    beginLayout->addWidget(beginLabel);
    m_beginMode = new QComboBox(beginBox);
    beginLayout->addWidget(m_beginMode);
    auto *beginNote = new QLabel(beginBox);
    beginNote->setObjectName(QStringLiteral("note"));
    beginNote->setWordWrap(true);
    beginNote->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
    beginLayout->addWidget(beginNote);
    beginBox->setVisible(false);
    layout->addWidget(beginBox);

    auto *superBox = new QWidget(this);
    auto *superLayout = new QVBoxLayout(superBox);
    superLayout->setContentsMargins(0, 6, 0, 0);
    superLayout->setSpacing(3);
    m_callSuper = new QCheckBox(tr("Call super first"), superBox);
    superLayout->addWidget(m_callSuper);
    auto *superNote = new QLabel(
        tr("Leave this on unless you deliberately mean to replace the base behaviour."),
        superBox);
    superNote->setObjectName(QStringLiteral("note"));
    superNote->setWordWrap(true);
    superNote->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
    superLayout->addWidget(superNote);
    superBox->setVisible(false);
    layout->addWidget(superBox);

    connect(m_beginMode, &QComboBox::currentIndexChanged,
            this, &InspectorPanel::onBeginModeChanged);
    connect(m_callSuper, &QCheckBox::toggled, this, &InspectorPanel::onSuperToggled);

    if (m_doc) {
        connect(m_doc, &Document::selectionChanged,
                this, &InspectorPanel::onNodeSelectionChanged);
        connect(m_doc, &Document::graphChanged, this, &InspectorPanel::refresh);
        connect(m_doc, &Document::activeScriptChanged, this, &InspectorPanel::refresh);
    }

    // The Variable Manager is a sibling dock rather than something this panel is
    // handed, so it is looked up once the window around both exists.
    QTimer::singleShot(0, this, [this]() { bindVariablesPanel(); });

    showEmpty();
}

void InspectorPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    bindVariablesPanel();
}

void InspectorPanel::bindVariablesPanel()
{
    QWidget *root = window();
    auto *vars = root ? root->findChild<VariablesPanel *>() : nullptr;
    if (!vars) return;
    // Unique, so wiring the same pair from the main window instead would cost
    // nothing and would not deliver the selection twice.
    connect(vars, &VariablesPanel::variableSelected, this,
            &InspectorPanel::showVariable, Qt::UniqueConnection);
}

// The variable side of the panel, built once and hidden until a variable is
// picked. Default value leads, because "set it without wiring a Set node" is
// the question the panel exists to answer; everything else is the rest of the
// declaration in the order it is read.
void InspectorPanel::buildVariableForm()
{
    auto *pane = new QScrollArea(this);
    pane->setFrameShape(QFrame::NoFrame);
    pane->setWidgetResizable(true);
    // The dock is narrow and the form is long, so it scrolls down and never
    // sideways: a horizontal bar here would hide the right half of every field.
    pane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The app sheet counts a scroll area as an input and frames it. This is a
    // page of controls, not a field, so the frame is turned off where a widget
    // sheet outranks the app one. The second selector reaches the scrolled
    // widget, which would otherwise paint itself over the dock colour.
    pane->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent;"
        " border: none; }"));
    m_varPane = pane;

    auto *form = new QWidget(pane);
    auto *layout = new QVBoxLayout(form);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(4);

    auto *identity = new QFormLayout;
    identity->setContentsMargins(0, 0, 0, 0);
    identity->setSpacing(4);
    identity->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    identity->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_varName = new QLineEdit(form);
    // An Enforce member name is a C-style identifier, so the field refuses
    // anything that could not be one rather than letting it reach the generator.
    m_varName->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*")), m_varName));
    identity->addRow(tr("Name"), m_varName);

    m_varType = new QComboBox(form);
    configureVariableTypeCombo(m_varType, m_doc);
    identity->addRow(tr("Type"), m_varType);
    layout->addLayout(identity);

    m_varNameError = new QLabel(form);
    m_varNameError->setWordWrap(true);
    m_varNameError->setFont(theme::uiFont(8));
    m_varNameError->setStyleSheet(
        QStringLiteral("color: %1").arg(theme::errorColor().name()));
    m_varNameError->setVisible(false);
    layout->addWidget(m_varNameError);

    layout->addSpacing(4);
    layout->addWidget(sectionLabel(tr("Default value"), form));

    // The editor picks its own widget from the type, and it is the same one the
    // variable table uses, so a bool is a box here and there and a vector is
    // three fields in both.
    m_varValue = new ValueEditor(m_doc ? &m_doc->catalog() : nullptr, form);
    layout->addWidget(m_varValue);

    layout->addWidget(noteLabel(
        tr("This is the declaration's initial value, so it is applied once when the "
           "object is created. Changing the value while the game runs still needs a "
           "Set node."),
        form));

    m_varDefaultWarning = new QLabel(form);
    m_varDefaultWarning->setWordWrap(true);
    m_varDefaultWarning->setFont(theme::uiFont(8));
    m_varDefaultWarning->setStyleSheet(
        QStringLiteral("color: %1").arg(theme::warningColor().name()));
    m_varDefaultWarning->setVisible(false);
    layout->addWidget(m_varDefaultWarning);

    layout->addSpacing(4);
    layout->addWidget(sectionLabel(tr("Declaration"), form));
    m_varPreview = new QPlainTextEdit(form);
    m_varPreview->setReadOnly(true);
    m_varPreview->setFont(theme::monoFont(8));
    m_varPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_varPreview->setStyleSheet(QStringLiteral("QPlainTextEdit { background: %1;"
                                               " border: 1px solid %2; }")
                                    .arg(theme::canvasBg().name(),
                                         theme::border().name()));
    m_varPreview->setFixedHeight(codePreviewHeight(m_varPreview, 1));
    new EnforceHighlighter(m_varPreview->document(), m_doc ? &m_doc->catalog() : nullptr);
    layout->addWidget(m_varPreview);

    layout->addSpacing(4);
    layout->addWidget(sectionLabel(tr("Networking and storage"), form));
    m_varSync = new QCheckBox(tr("Network synced"), form);
    layout->addWidget(m_varSync);
    QLabel *syncNote = noteLabel(
        tr("Registers the member with RegisterNetSyncVariable in the constructor. "
           "Clients keep the old value until SetSynchDirty() runs after the write."),
        form);
    syncNote->setContentsMargins(17, 0, 0, 3);
    layout->addWidget(syncNote);

    m_varPersist = new QCheckBox(tr("Saved to storage"), form);
    layout->addWidget(m_varPersist);
    QLabel *persistNote = noteLabel(
        tr("Writes the member in OnStoreSave and reads it back in OnStoreLoad, so "
           "the value survives a server restart."),
        form);
    persistNote->setContentsMargins(17, 0, 0, 3);
    layout->addWidget(persistNote);

    layout->addSpacing(4);
    layout->addWidget(sectionLabel(tr("Modifiers"), form));
    m_varStatic = new QCheckBox(tr("static"), form);
    m_varStatic->setToolTip(tr("One value shared by the class, not one per instance."));
    layout->addWidget(m_varStatic);
    m_varConst = new QCheckBox(tr("const"), form);
    m_varConst->setToolTip(tr("Cannot be assigned after the declaration, so it needs "
                              "a default value."));
    layout->addWidget(m_varConst);
    m_varPrivate = new QCheckBox(tr("private"), form);
    m_varPrivate->setToolTip(tr("Reachable only from this class."));
    layout->addWidget(m_varPrivate);
    m_varProtected = new QCheckBox(tr("protected"), form);
    m_varProtected->setToolTip(tr("Reachable from this class and anything that "
                                  "extends it."));
    layout->addWidget(m_varProtected);

    layout->addSpacing(4);
    layout->addWidget(sectionLabel(tr("Reference"), form));
    m_varRef = new QComboBox(form);
    m_varRef->addItem(tr("Infer from the type"), RefInfer);
    m_varRef->addItem(tr("Always ref"), RefAlways);
    m_varRef->addItem(tr("Never ref"), RefNever);
    layout->addWidget(m_varRef);
    m_varRefNote = noteLabel(QString(), form);
    layout->addWidget(m_varRefNote);

    m_varFindings = new QLabel(form);
    m_varFindings->setWordWrap(true);
    m_varFindings->setFont(theme::uiFont(8));
    m_varFindings->setTextFormat(Qt::RichText);
    m_varFindings->setVisible(false);
    layout->addSpacing(4);
    layout->addWidget(m_varFindings);

    layout->addStretch(1);
    pane->setWidget(form);
    pane->setVisible(false);

    // Name and type commit on editing-finished (Enter, Tab, or leaving the
    // field) rather than per keystroke, so renaming a member is one undo step
    // and not one per letter. Picking a type off the list is already a finished
    // gesture, so it commits on activation.
    connect(m_varName, &QLineEdit::editingFinished,
            this, &InspectorPanel::onNameCommitted);
    connect(m_varName, &QLineEdit::textChanged, this, [this]() { updatePreview(); });
    connect(m_varType, &QComboBox::activated, this, &InspectorPanel::onTypeCommitted);
    if (m_varType->lineEdit())
        connect(m_varType->lineEdit(), &QLineEdit::editingFinished,
                this, &InspectorPanel::onTypeCommitted);
    connect(m_varType, &QComboBox::editTextChanged, this, [this]() { updatePreview(); });

    // The value editor has no editing-finished of its own: it reports every
    // keystroke and every spin step. So the default commits on a debounce, and
    // the preview, which writes nothing to the graph, follows the keystrokes.
    m_defaultCommit = new QTimer(this);
    m_defaultCommit->setSingleShot(true);
    m_defaultCommit->setInterval(400);
    connect(m_defaultCommit, &QTimer::timeout, this, &InspectorPanel::onDefaultCommitted);
    connect(m_varValue, &ValueEditor::valueChanged, this, [this]() {
        if (m_varLoading) return;
        updatePreview();
        m_defaultCommit->start();
    });

    for (QCheckBox *box : {m_varSync, m_varPersist, m_varStatic, m_varConst,
                           m_varPrivate, m_varProtected})
        connect(box, &QCheckBox::toggled, this, &InspectorPanel::onFlagToggled);
    connect(m_varRef, &QComboBox::activated, this, &InspectorPanel::onRefModeChanged);
}

void InspectorPanel::refresh()
{
    const Graph *activeGraph = m_doc ? m_doc->activeGraph() : nullptr;
    if (!m_variableId.isEmpty()) {
        const GraphVariable *var = nullptr;
        if (activeGraph)
            for (const GraphVariable &v : activeGraph->variables)
                if (v.id == m_variableId) { var = &v; break; }
        if (var) {
            fillVariable(*var);
            return;
        }
        // Deleted, or the editor moved to another script. Either way the panel
        // has nothing left to show for it.
        m_variableId.clear();
    }
    setVariableMode(false);

    const QStringList selection = m_doc ? m_doc->selection() : QStringList();
    const GraphNode *node = (selection.size() == 1 && activeGraph)
                                ? activeGraph->node(selection.first())
                                : nullptr;
    if (!node) {
        m_nodeId.clear();
        showEmpty();
        return;
    }
    m_nodeId = node->id;

    const NodeDef def = m_doc->defForNode(*node);
    // An unresolvable ref still gets a readable panel: the raw ref is the one
    // piece of information left that can be acted on.
    m_title->setText(def.valid && !def.title.isEmpty() ? def.title : node->ref);

    NodeHelp help = m_doc->catalog().explain(node->ref);
    if (!help.valid) help = fallbackHelp(*m_doc, *node, def);
    m_kind->setText(help.kind);

    QString body;
    if (!help.summary.isEmpty()) {
        body += QStringLiteral("<p style=\"margin:0 0 9px 0\">%1</p>")
                    .arg(richText(help.summary));
    } else {
        body += QStringLiteral("<p style=\"margin:0 0 9px 0; color:%1\">%2</p>")
                    .arg(theme::textDim().name(),
                         tr("Bohemia ship no doc comment for this one, so what follows is "
                            "read from its declaration."));
    }
    body += bulletList(help.effects, theme::text());
    body += bulletList(help.cautions, theme::warningColor());

    QString details;
    details += detailRow(tr("owner"), def.subtitle);
    details += detailRow(tr("category"), def.category);
    details += detailRow(tr("declared"), help.source.isEmpty() ? def.loc : help.source);
    if (!details.isEmpty())
        body += QStringLiteral("<table style=\"margin:0 0 9px 0\">%1</table>").arg(details);

    const QString signature = signatureOf(m_doc->catalog(), node->ref);
    if (!signature.isEmpty()) {
        body += QStringLiteral("<p style=\"margin:0; font-family:%1; color:%2\">%3</p>")
                    .arg(theme::monoFont().family(), theme::textDim().name(),
                         signature.toHtmlEscaped());
    }
    m_body->setHtml(body);

    QWidget *codeBox = findChild<QWidget *>(QStringLiteral("codeBox"));
    const bool hasCode = isCodeNode(*node);
    if (codeBox) codeBox->setVisible(hasCode);
    if (hasCode && codeBox) {
        const bool comment = isCommentNode(*node);
        if (auto *label = codeBox->findChild<QLabel *>(QStringLiteral("codeLabel")))
            label->setText(comment ? tr("Comment text") : tr("Code"));
        if (auto *preview = codeBox->findChild<QPlainTextEdit *>(
                QStringLiteral("codePreview"))) {
            const QString code = codeOf(*node);
            preview->setPlainText(code);
            preview->setFixedHeight(
                codePreviewHeight(preview, code.count(QLatin1Char('\n')) + 1));
            preview->setPlaceholderText(comment ? tr("Empty note.")
                                                : tr("No code yet. Nothing will be "
                                                     "generated for this node."));
        }
        if (auto *button = codeBox->findChild<QPushButton *>(QStringLiteral("codeEdit")))
            button->setText(comment ? tr("Edit text") : tr("Edit code"));
    }

    QWidget *beginBox = optionBox(m_beginMode);
    const bool isBegin = node->ref == bi::Begin;
    if (beginBox) beginBox->setVisible(isBegin);
    if (isBegin) {
        const QSignalBlocker blocker(m_beginMode);
        m_beginMode->clear();
        for (const QString &key : orderedBeginModes(m_doc->builtins())) {
            const LifecycleSig sig = m_doc->builtins().beginMode(key);
            m_beginMode->addItem(sig.label.isEmpty() ? key : sig.label, key);
        }
        int index = m_beginMode->findData(node->opts.value(QStringLiteral("when")));
        if (index < 0) index = m_beginMode->findData(QStringLiteral("init"));
        if (index < 0) index = 0;
        m_beginMode->setCurrentIndex(index);
        if (QLabel *note = optionNote(m_beginMode)) {
            note->setText(m_doc->builtins()
                              .beginMode(m_beginMode->currentData().toString())
                              .note);
        }
    }

    QWidget *superBox = optionBox(m_callSuper);
    // Begin and End are Builtin-kind, not Event-kind, but codegen and analysis
    // both honour noSuper on them, so gating on kind alone left the flag set
    // with no control that could clear it.
    const bool isEvent = node->kind == NodeKind::Event
                         || node->ref == bi::Begin || node->ref == bi::End;
    if (superBox) superBox->setVisible(isEvent);
    if (isEvent) {
        const QSignalBlocker blocker(m_callSuper);
        m_callSuper->setChecked(
            node->opts.value(QStringLiteral("noSuper")) != QLatin1String("1"));
    }
}

void InspectorPanel::showEmpty()
{
    const int count = m_doc ? m_doc->selection().size() : 0;
    m_title->setText(count > 1 ? tr("%1 nodes selected").arg(count) : tr("Nothing selected"));
    m_kind->clear();
    m_body->setHtml(QStringLiteral("<p style=\"margin:0; color:%1\">%2</p>")
                        .arg(theme::textDim().name(),
                             count > 1
                                 ? tr("Pick a single node to see what it does.")
                                 : tr("Select a node to see what it does, where it comes "
                                      "from, and what it will generate.")));
    if (QWidget *box = findChild<QWidget *>(QStringLiteral("codeBox")))
        box->setVisible(false);
    if (QWidget *box = optionBox(m_beginMode)) box->setVisible(false);
    if (QWidget *box = optionBox(m_callSuper)) box->setVisible(false);
}

void InspectorPanel::showVariable(const QString &variableId)
{
    if (m_variableId == variableId) return;
    flushPendingDefault();
    m_variableId = variableId;
    refresh();
}

void InspectorPanel::flushPendingDefault()
{
    // A value still inside the debounce belongs to the variable being left, and
    // m_variableId still names it here.
    if (!m_defaultCommit || !m_defaultCommit->isActive()) return;
    m_defaultCommit->stop();
    onDefaultCommitted();
}

void InspectorPanel::onNodeSelectionChanged()
{
    // Last selection wins: picking a node takes the panel off the variable it
    // was showing. Clicking empty canvas is not a pick, so a variable stays put
    // rather than blinking away under the cursor.
    if (m_doc && !m_doc->selection().isEmpty()) {
        flushPendingDefault();
        m_variableId.clear();
    }
    refresh();
}

void InspectorPanel::setVariableMode(bool on)
{
    m_varPane->setVisible(on);
    m_body->setVisible(!on);
    if (!on) return;
    // The node-only controls have no meaning next to a declaration.
    if (QWidget *box = findChild<QWidget *>(QStringLiteral("codeBox")))
        box->setVisible(false);
    if (QWidget *box = optionBox(m_beginMode)) box->setVisible(false);
    if (QWidget *box = optionBox(m_callSuper)) box->setVisible(false);
}

void InspectorPanel::fillVariable(const GraphVariable &var)
{
    setVariableMode(true);
    m_nodeId.clear();

    m_title->setText(var.name.isEmpty() ? tr("Unnamed variable") : var.name);
    m_kind->setText(var.type.trimmed().isEmpty()
                        ? tr("Variable with no type")
                        : tr("Variable - %1").arg(var.type));

    m_varLoading = true;

    // A control the user is in is left alone. refresh runs on every graph
    // change, including ones made elsewhere while a name is half typed here.
    if (!m_varName->hasFocus()) m_varName->setText(var.name);
    if (!isBusy(m_varType)) {
        // Move to the row first so the popup opens on the current type, then
        // put the text back: a type the list does not carry still has to
        // survive being shown.
        const int row = m_varType->findText(var.type);
        if (row >= 0) m_varType->setCurrentIndex(row);
        m_varType->setEditText(var.type);
    }

    // setType is a no-op when the type has not moved, so this does not rebuild
    // the editor under a caret that is sitting in it.
    m_varValue->setType(var.type);
    if (!isBusy(m_varValue)) m_varValue->setValue(var.def);

    m_varSync->setChecked(var.sync);
    m_varPersist->setChecked(var.persist);
    m_varStatic->setChecked(var.isStatic);
    m_varConst->setChecked(var.isConst);
    m_varPrivate->setChecked(var.isPrivate);
    m_varProtected->setChecked(var.isProtected);

    const int mode = !var.hasRef ? RefInfer : (var.isRef ? RefAlways : RefNever);
    m_varRef->setCurrentIndex(qMax(0, m_varRef->findData(mode)));

    m_varNameError->setVisible(false);
    m_varLoading = false;

    // Findings come from a fresh analysis pass rather than from a copy of the
    // rules. It runs here, once per graph change while a variable is on show,
    // and not from updatePreview, which follows every keystroke.
    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    const QString findings =
        graph ? findingsHtml(analyzeGraph(*graph, m_doc->catalog(), m_doc->builtins(),
                                          m_doc->activeScriptId()),
                             var.name)
              : QString();
    m_varFindings->setText(findings);
    m_varFindings->setVisible(!findings.isEmpty());

    updatePreview();
}

QString InspectorPanel::currentDefaultText() const
{
    return m_varValue->value();
}

bool InspectorPanel::pendingVariable(GraphVariable *out) const
{
    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphVariable *live = variableById(graph, m_variableId);
    if (!live || !out) return false;
    *out = *live;
    out->name = m_varName->text().trimmed();
    out->type = m_varType->currentText().trimmed();
    out->def = currentDefaultText();
    return true;
}

void InspectorPanel::updatePreview()
{
    if (m_varLoading || m_variableId.isEmpty() || !m_doc) return;
    GraphVariable pending;
    if (!pendingVariable(&pending)) return;

    const QString name = pending.name.trimmed();
    const QString type = pending.type.trimmed();
    const QString decl = declarationFor(*m_doc, pending);
    m_varPreview->setPlainText(decl);
    m_varPreview->setPlaceholderText(
        tr("Fill in a name and a type and the declaration appears here."));

    // Whether `ref` is written is codegen's decision, so the note reads it off
    // the line that was just generated instead of working the rule out again.
    // `ref` is the last modifier, so it sits immediately before the type.
    const bool writesRef =
        !decl.isEmpty()
        && decl.contains(QStringLiteral("ref ") + type + QLatin1Char(' ') + name);
    switch (m_varRef->currentData().toInt()) {
    case RefAlways:
        m_varRefNote->setText(tr("Holds a counted reference even where the type "
                                 "would not be given one."));
        break;
    case RefNever:
        m_varRefNote->setText(tr("Never holds one, even where the type would be "
                                 "given one. Kept as a decision, so inference "
                                 "does not put it back."));
        break;
    default:
        m_varRefNote->setText(
            writesRef ? tr("Inferred: this type is counted, so the member holds a "
                           "reference and survives as long as the class does.")
                      : tr("Inferred: no reference. Primitives do not take one, and "
                           "neither do entities, which the engine owns."));
        break;
    }

    // The editor keeps a literal it cannot represent rather than normalising it
    // away, and says why. Showing that here is what stops a default the file
    // will not compile from looking accepted.
    QString problem;
    const bool bad = !m_varValue->isValid(&problem) && !problem.isEmpty();
    if (bad) m_varDefaultWarning->setText(problem);
    m_varDefaultWarning->setVisible(bad);
}

bool InspectorPanel::editVariable(const QString &label,
                                  const std::function<void(GraphVariable &)> &edit)
{
    Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    if (!graph) return false;
    int index = -1;
    for (int i = 0; i < graph->variables.size(); ++i)
        if (graph->variables.at(i).id == m_variableId) { index = i; break; }
    if (index < 0) return false;

    m_doc->beginEdit(label);
    // beginEdit snapshots the graph, so the variable is looked up again on the
    // live one rather than through a pointer taken before the snapshot.
    edit(m_doc->activeGraph()->variables[index]);
    m_doc->commitEdit();
    return true;
}

void InspectorPanel::onNameCommitted()
{
    if (m_varLoading || m_variableId.isEmpty()) return;
    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphVariable *current = variableById(graph, m_variableId);
    if (!current) return;

    const QString name = m_varName->text().trimmed();
    if (name == current->name) {
        m_varNameError->setVisible(false);
        return;
    }

    bool clash = false;
    for (const GraphVariable &v : graph->variables)
        if (v.id != m_variableId && v.name == name) clash = true;

    // An empty or duplicated name produces a class that will not compile. The
    // edit is refused, and the reason is said out loud rather than the field
    // silently snapping back to what it held.
    const QString problem =
        name.isEmpty() ? tr("A member needs a name.")
                       : clash ? tr("Another member is already called %1.").arg(name)
                               : QString();
    m_varNameError->setText(problem);
    m_varNameError->setVisible(!problem.isEmpty());
    if (!problem.isEmpty()) {
        m_varName->setText(current->name);
        return;
    }
    editVariable(tr("Rename variable"), [&name](GraphVariable &v) { v.name = name; });
}

void InspectorPanel::onTypeCommitted()
{
    if (m_varLoading || m_variableId.isEmpty()) return;
    const GraphVariable *current =
        variableById(m_doc ? m_doc->activeGraph() : nullptr, m_variableId);
    if (!current) return;

    const QString type = m_varType->currentText().trimmed();
    if (type == current->type) return;
    // A member with no type is not generated at all, so the box keeps the type
    // it had rather than emptying the declaration.
    if (type.isEmpty()) {
        m_varType->setEditText(current->type);
        return;
    }
    editVariable(tr("Change variable type"), [&type](GraphVariable &v) { v.type = type; });
}

void InspectorPanel::onDefaultCommitted()
{
    if (m_varLoading || m_variableId.isEmpty()) return;
    const GraphVariable *current =
        variableById(m_doc ? m_doc->activeGraph() : nullptr, m_variableId);
    if (!current) return;

    const QString def = currentDefaultText();
    if (def == current->def) return;
    editVariable(tr("Set default value"), [&def](GraphVariable &v) { v.def = def; });
}

void InspectorPanel::onFlagToggled()
{
    if (m_varLoading || m_variableId.isEmpty()) return;
    auto *box = qobject_cast<QCheckBox *>(sender());
    if (!box) return;

    // private and protected are one choice under two names. Emitting both gives
    // `private protected`, which Enforce does not accept, so turning one on
    // turns the other off.
    if (box == m_varPrivate && box->isChecked()) {
        const QSignalBlocker blocker(m_varProtected);
        m_varProtected->setChecked(false);
    }
    if (box == m_varProtected && box->isChecked()) {
        const QSignalBlocker blocker(m_varPrivate);
        m_varPrivate->setChecked(false);
    }

    const bool sync = m_varSync->isChecked();
    const bool persist = m_varPersist->isChecked();
    const bool isStatic = m_varStatic->isChecked();
    const bool isConst = m_varConst->isChecked();
    const bool isPrivate = m_varPrivate->isChecked();
    const bool isProtected = m_varProtected->isChecked();
    editVariable(tr("Change variable flags"), [=](GraphVariable &v) {
        v.sync = sync;
        v.persist = persist;
        v.isStatic = isStatic;
        v.isConst = isConst;
        v.isPrivate = isPrivate;
        v.isProtected = isProtected;
    });
}

void InspectorPanel::onRefModeChanged(int index)
{
    if (m_varLoading || m_variableId.isEmpty() || index < 0) return;
    const int mode = m_varRef->itemData(index).toInt();
    editVariable(tr("Change variable reference"), [mode](GraphVariable &v) {
        // An explicit "never" has to survive as a decision rather than as an
        // absent flag: codegen reads hasRef first and only infers without it.
        v.hasRef = mode != RefInfer;
        v.isRef = mode == RefAlways;
    });
}

void InspectorPanel::onBeginModeChanged(int index)
{
    if (m_nodeId.isEmpty() || index < 0 || !m_doc || !m_doc->activeGraph()) return;
    const QString key = m_beginMode->itemData(index).toString();
    if (key.isEmpty()) return;

    const GraphNode *node = m_doc->activeGraph()->node(m_nodeId);
    if (!node || node->opts.value(QStringLiteral("when")) == key) return;

    // Which moment Begin stands for decides which Enforce method the script
    // becomes, so it is a graph edit and belongs on the undo stack.
    m_doc->beginEdit(tr("Change lifecycle moment"));
    if (GraphNode *live = m_doc->activeGraph()->node(m_nodeId))
        live->opts.insert(QStringLiteral("when"), key);
    m_doc->commitEdit();
}

void InspectorPanel::onSuperToggled(bool checked)
{
    if (m_nodeId.isEmpty() || !m_doc || !m_doc->activeGraph()) return;
    const GraphNode *node = m_doc->activeGraph()->node(m_nodeId);
    if (!node) return;

    // Stored inverted, as "noSuper", to match the .sdzn the Electron build
    // writes: an absent option means super is called.
    const QString value = checked ? QStringLiteral("0") : QStringLiteral("1");
    if (node->opts.value(QStringLiteral("noSuper"), QStringLiteral("0")) == value) return;

    m_doc->beginEdit(tr("Toggle super call"));
    if (GraphNode *live = m_doc->activeGraph()->node(m_nodeId))
        live->opts.insert(QStringLiteral("noSuper"), value);
    m_doc->commitEdit();
}
