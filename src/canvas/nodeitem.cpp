#include "nodeitem.h"

#include "builtins.h"
#include "catalog.h"
#include "document.h"
#include "enforce/lexer.h"
#include "moddeps.h"
#include "nodescene.h"
#include "theme.h"

#include <QAction>
#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMarginsF>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <limits>

namespace {

using theme::node::headerHeight;
using theme::node::padding;
using theme::node::pinRadius;
using theme::node::pinRow;
using theme::node::radius;
using theme::node::width;

// The pin sits on the node edge, so labels start one padding in from it.
constexpr double kLabelInset = padding;
// How far off a pin a press still counts as hitting it.
constexpr double kPinReach = pinRadius + 4.0;
// What counts as "on the pin" for a press. Wiring is the main gesture on this
// canvas, so the target is deliberately far bigger than the drawn dot: missing
// it starts a marquee, which reads as the canvas ignoring the drag.
constexpr double kPinGrab = pinRadius + 11.0;
// Drawn a little inside the grab radius so the halo reads as a target
// rather than as a second node.
constexpr double kPinHalo = pinRadius + 7.0;
constexpr double kBadgeRadius = 4.6;
// The dependency tag is a lettered pill, not a logo. A mod ships its icon as an
// .edds, which nothing here can decode, and two or three letters stay readable
// at the 0.75 zoom a long graph fits at, where a 9 unit image would be mud.
constexpr double kTagHeight = 9.2;
constexpr double kTagPadX = 3.2;
constexpr double kTagRadius = 2.2;
// Between the tag and the diagnostics badge, and between the tag and the text
// on its left. Enough that the two markers read as two things, and enough that
// an elided subtitle does not run into the pill it stops against.
constexpr double kTagGap = 4.0;
// How much of the dependency's colour the pill keeps. A solid chip beside the
// diagnostics dot reads as a second alert; this is low enough that the dot
// stays the loudest thing on the header and the tag stays a label.
constexpr int kTagFillAlpha = 46;
// A source marker, not a name field. Past this the header belongs to the title.
constexpr int kTagChars = 5;
// The strip above the header where a node shows the author's own comment. A
// comment the graph holds and the canvas never draws would be the tool quietly
// owning someone's words, so it is drawn wherever the node is.
constexpr double kNoteHeight = 9.5;
// How far outside a value field a press still counts, in SCREEN pixels. The
// field is nine scene units tall, so measured in scene units the target shrinks
// with the zoom and lands at three pixels by the time a long graph fits on
// screen. This is the same fix pin grabbing already got in nodeview.cpp.
constexpr double kEditorReachPixels = 5.0;
// A menu longer than this stops being a list you scan and becomes one you
// scroll, so past it the members go in a dialog with a search field instead.
constexpr int kMenuMembers = 28;
// Wider than it is tall: the triangle encodes flow direction, and at the 0.75
// zoom a long graph fits at, a taller-than-wide one resolves to a blob that
// cannot be told from a data pin's circle.
constexpr double kExecHalfHeight = 3.6;
constexpr double kExecHalfWidth = 4.4;
// Below this the title stops being a word and starts being an ellipsis.
constexpr double kMinTitleWidth = 42.0;
// Past this a node stops being a node and starts crowding the graph, so a
// very long label elides rather than growing without end.
constexpr double kMaxWidth = 340.0;
// A value field narrower than this is not worth clicking.
constexpr double kMinFieldWidth = 44.0;
// Between a pin's name and the type it carries.
constexpr double kTypeGap = 5.0;
// A type cut down to two letters and a cut mark names nothing, so below this
// the type gives up its half of the row and the name keeps the whole of it.
constexpr double kMinTypeWidth = 18.0;
// A sentence with less room than this is a fragment, and a fragment on a node
// is worse than the empty row it replaced.
constexpr double kMinSummaryWidth = 46.0;
// How much of the type's colour survives. The dot on the edge of the row is the
// loud copy of this information; the word is the quiet one that says which
// class, and it must not compete with the pin's own name.
constexpr int kTypeAlpha = 165;

// The element controls on a node whose pin list the user decides. Square, so
// the two read as a pair of buttons rather than as two more value fields, and
// the same height as an inline field so the footer sits on the same rhythm as
// the rows above it.
constexpr double kListButton = 9.0;
constexpr double kListGap = 3.0;

// Code body. The maximum width is a readability limit rather than a technical
// one: past roughly seventy monospace columns a node stops being a node and
// starts being a document, and the graph around it becomes unreadable.
constexpr double kCodeMaxWidth = 420.0;
constexpr int kCodeMaxLines = 12;
constexpr int kCodeFontSize = 8;
constexpr double kCodeIndent = padding;
constexpr double kCodeVPad = 4.0;
// A code node's header carries no title, only the pins and the raw marker, so
// the full 20 units would be a bar of empty accent over one line of script.
constexpr double kCodeHeaderHeight = 14.0;
const QLatin1String kCut("...");

QFont codeFont() { return theme::monoFont(kCodeFontSize); }


// Refs whose real content lives in opts rather than on pins.
bool carriesCode(const QString &ref)
{
    return ref == QLatin1String("bi.raw") || ref == QLatin1String("bi.rawExpr")
           || ref == QLatin1String("bi.comment");
}

// The key has moved between builds of the reference app, so read whichever one
// this node actually carries.
QString codeOf(const GraphNode &n)
{
    for (const char *key : {"code", "text"}) {
        const QString v = n.opts.value(QString::fromLatin1(key));
        if (!v.isEmpty()) return v;
    }
    return QString();
}

// Same palette the code editor uses, from theme::syntax. A node showing a
// keyword in one colour and the dialog that edits it showing another would make
// the canvas look like a preview of something else.
QColor syntaxColor(TokenKind kind)
{
    switch (kind) {
    case TokenKind::Comment:      return theme::syntax::comment();
    case TokenKind::Keyword:      return theme::syntax::keyword();
    case TokenKind::Type:         return theme::syntax::type();
    case TokenKind::Preprocessor: return theme::syntax::preprocessor();
    case TokenKind::String:       return theme::syntax::stringLit();
    case TokenKind::Number:       return theme::syntax::number();
    case TokenKind::ClassName:    return theme::syntax::className();
    case TokenKind::Identifier:
    case TokenKind::Unknown:      return theme::text();
    case TokenKind::Operator:
    case TokenKind::Punctuation:  return theme::textDim();
    case TokenKind::Whitespace:   return theme::text();
    }
    return theme::text();
}

// Names this graph declares itself. The catalogue has never heard of them, but
// they are not guesses either, and the editor colours them apart for the same
// reason.
QSet<QString> graphSymbols(const Graph &g)
{
    QSet<QString> names;
    names.insert(g.className);
    for (const GraphVariable &v : g.variables) names.insert(v.name);
    for (const GraphFunction &f : g.functions) {
        names.insert(f.name);
        for (const GraphParam &p : f.params) names.insert(p.name);
    }
    names.remove(QString());
    return names;
}

// Blank top and bottom lines are dead height on a node, and an indent the
// author only needed inside their own file is dead width.
QStringList displayLines(const QString &code)
{
    QStringList lines = code.split(QLatin1Char('\n'));
    for (QString &l : lines) {
        while (l.endsWith(QLatin1Char('\r'))) l.chop(1);
        l.replace(QLatin1Char('\t'), QStringLiteral("    "));
    }
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();

    int common = std::numeric_limits<int>::max();
    for (const QString &l : lines) {
        if (l.trimmed().isEmpty()) continue;
        int i = 0;
        while (i < l.size() && l.at(i) == QLatin1Char(' ')) i++;
        common = qMin(common, i);
    }
    if (common > 0 && common != std::numeric_limits<int>::max())
        for (QString &l : lines) l = l.mid(qMin(common, l.size()));
    return lines;
}

QString hiddenLineNote(int n)
{
    return n == 1 ? QStringLiteral("+1 more line")
                  : QStringLiteral("+%1 more lines").arg(n);
}

// Longest prefix of `text` that still draws inside `room`. Binary search rather
// than a character-width estimate, which is wrong the moment the monospace face
// falls back to something else.
QString fitPrefix(const QFontMetricsF &cm, const QString &text, double room)
{
    if (room <= 0.0) return QString();
    int lo = 0;
    int hi = text.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (cm.horizontalAdvance(text.left(mid)) <= room) lo = mid;
        else hi = mid - 1;
    }
    if (lo > 0 && text.at(lo - 1).isHighSurrogate()) lo--;
    return text.left(lo);
}

// DESIGN.md asks for semibold. theme::uiFont's bold flag is QFont::Bold (700),
// one step heavier, so the weight is set here instead.
QFont titleFont()
{
    QFont f = theme::uiFont(8);
    f.setWeight(QFont::DemiBold);
    return f;
}
QFont smallFont() { return theme::uiFont(7); }
// Same size as the glyph inside the diagnostics badge, so the two markers sit
// on the header as a pair rather than as one loud one and one quiet one.
QFont tagFont() { return theme::uiFont(6, true); }

// What this node carries of the author's own words, in one line. Blank lines
// are left out: they are invisible in the file, and "2 blank lines" written on
// a node is noise rather than information. The Details panel is where the whole
// text is edited; this is the reminder that there is something to edit.
QString noteOf(const GraphNode &n)
{
    QStringList comments;
    const QStringList keys = {nodefmt::keyBefore(), nodefmt::keyEnd(),
                              nodefmt::keyEndElse()};
    for (const QString &key : keys)
        for (const QString &l : nodefmt::lines(n.opts.value(key)))
            if (!nodefmt::isIndentText(l)) comments << l.trimmed();
    const QString trailing = n.opts.value(nodefmt::keyTrailing()).trimmed();
    if (!trailing.isEmpty()) comments << trailing;
    if (comments.isEmpty()) return {};
    if (comments.size() == 1) return comments.first();
    return comments.first() + QStringLiteral("   +%1 more").arg(comments.size() - 1);
}

// The type a pin carries, spelled the way the declaration spelled it.
//
// A pin kept only a kind and, for an object, a class name, so `ref array<ref
// ItemBase>` comes back off the pin as `array<ItemBase>`: the same
// instantiation to the connection rules, a different string to anyone reading
// it, and not the string they will search P:\scripts for. The signature still
// holds the original, so it answers first and the pin answers for what the
// signature has no entry for.
//
// `sig` is resolved once per node rather than once per pin: it allocates a
// vector of parameters, and a node with twelve pins would build twelve of them.
QString declaredPinType(const MethodSig &sig, const Pin &pin)
{
    if (sig.valid) {
        if (pin.id == QLatin1String("ret")) return sig.ret;
        // Catalog::paramPins numbers both the input and the output of a
        // parameter after the parameter itself, so the digits are a direct
        // index rather than a count of the pins before it.
        const QChar family = pin.id.isEmpty() ? QChar() : pin.id.at(0);
        if (pin.id.size() > 1
            && (family == QLatin1Char('p') || family == QLatin1Char('o'))) {
            bool ok = false;
            const int index = pin.id.mid(1).toInt(&ok);
            if (ok && index >= 0 && index < sig.params.size())
                return sig.params.at(index).type;
        }
    }
    return pinTypeName(pin.type);
}

// Labels the catalogue writes when it has nothing to say. Which side of the
// node a pin sits on already means "this comes out", so `return` is a word
// spent restating the geometry, and the type is what the row could have said
// instead. Nothing else is treated this way: `object`, `value` and `self` all
// name something the type does not.
bool isPlaceholderLabel(const QString &label)
{
    return label == QLatin1String("return");
}

// The first sentence of a vanilla doc comment, ready to draw on one row.
//
// The index flattens the whole comment into one string, so the prose, the
// `[note]` and `[warning]` blocks and the parameter list arrive run together
// and the sentence has to stop at whichever of them comes first rather than at
// the full stop. Backticks go too: the canvas has no second font to switch to.
QString firstSentence(const QString &doc)
{
    QString text = doc.simplified();
    text.remove(QLatin1Char('`'));
    if (text.isEmpty()) return text;

    static const QStringList blocks = {
        QStringLiteral("[note]"),   QStringLiteral("[warning]"),
        QStringLiteral("\\see"),    QStringLiteral("\\param"),
        QStringLiteral("\\return"), QStringLiteral("\\note"),
        QStringLiteral(" param "),  QStringLiteral(" return "),
    };
    int cut = text.size();
    for (const QString &block : blocks) {
        const int at = text.indexOf(block, 0, Qt::CaseInsensitive);
        if (at >= 0) cut = qMin(cut, at);
    }
    const int stop = text.indexOf(QLatin1String(". "));
    if (stop >= 0) cut = qMin(cut, stop + 1);
    return text.left(cut).trimmed();
}

Severity worstOf(const QVector<Diagnostic> &diags, bool *any)
{
    Severity worst = Severity::Info;
    *any = false;
    for (const Diagnostic &d : diags) {
        *any = true;
        if (d.severity == Severity::Error) return Severity::Error;
        if (d.severity == Severity::Warning) worst = Severity::Warning;
    }
    return worst;
}

QColor severityColor(Severity s)
{
    switch (s) {
    case Severity::Error: return theme::errorColor();
    case Severity::Warning: return theme::warningColor();
    case Severity::Info: return theme::accent();
    }
    return theme::accent();
}

// Scene units per screen pixel in the view an event arrived through. Zero
// views, or a widget that is not a view's viewport, means no zoom to correct
// for and the scene unit is the pixel.
double unitsPerPixel(const QWidget *viewport)
{
    const auto *view = viewport
                           ? qobject_cast<const QGraphicsView *>(viewport->parentWidget())
                           : nullptr;
    if (!view) return 1.0;
    const double scale = view->transform().m11();
    return scale > 0.0001 ? 1.0 / scale : 1.0;
}

// The reach an event should hit-test value fields with, capped at one row so a
// press at minimum zoom still picks a value rather than the whole node.
double editorReach(const QWidget *viewport)
{
    return qMin(kEditorReachPixels * unitsPerPixel(viewport), pinRow);
}

// Value text takes the pin's own colour, so a row says what it holds before it
// is read: the green of an int and the pink of a string are already the two
// dots on the left edge. Empty and default values are dimmed instead, because
// nothing has been decided about them yet.
QColor valueColor(const PinType &type, bool overridden)
{
    QColor c = pinColor(type.kind);
    if (!overridden) c.setAlpha(150);
    return c;
}

} // namespace

NodeItem::NodeItem(Document *doc, const QString &nodeId, QGraphicsItem *parent)
    : QGraphicsObject(parent), m_doc(doc), m_nodeId(nodeId)
{
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setZValue(0);
    // Value fields light up under the cursor. Without that the only way to find
    // out a row is clickable is to click it and watch a dialog open.
    setAcceptHoverEvents(true);
    refresh();
}

void NodeItem::refresh()
{
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *n = g ? g->node(m_nodeId) : nullptr;

    prepareGeometryChange();
    // Whatever it said before, it is about to say something else.
    m_tipReady = false;
    setToolTip(QString());
    if (!n) {
        // The node went away under us; keep the item inert until the scene
        // rebuilds rather than reading a dangling def.
        m_def = NodeDef();
        m_pins.clear();
        m_code.clear();
        m_codeHidden = 0;
        m_width = width;
        m_headerHeight = headerHeight;
        m_pinsOnHeader = false;
        m_height = headerHeight + padding * 2;
        m_listRow = QRectF();
        m_listMinus = QRectF();
        m_listPlus = QRectF();
        m_listCount = 0;
        m_sourceTag.clear();
        m_sourceColor = QColor();
        m_note.clear();
        m_summary.clear();
        m_summaryRect = QRectF();
        m_bandRule = 0.0;
        update();
        return;
    }

    m_note = noteOf(*n);
    // Only what Bohemia wrote about this declaration. A builtin already spends
    // its subtitle on a description of itself ("if / else", "runs once on
    // init"), so a second sentence saying the same thing in more words would be
    // the node arguing with its own header.
    m_summary = m_doc ? firstSentence(m_doc->catalog().doc(n->ref)) : QString();
    resolveSource(*n);
    m_def = m_doc->defForNode(*n);
    if (m_def.valid) {
        m_title = m_def.title;
        m_subtitle = m_def.subtitle;
    } else {
        // A ref no catalogue in this build knows. Showing it beats dropping
        // it, because the user is the only one who can decide what replaces it.
        m_title = n->ref;
        m_subtitle = QStringLiteral("unknown node");
    }

    layoutCode(*n);
    if (!m_code.isEmpty()) {
        // The body is about to show the code itself, so a summary in the header
        // would be the same sentence twice, and the elided copy at that. What is
        // left is the marker saying this one is hand-written. An empty raw node
        // has no body and keeps the def title, so it is still identifiable.
        m_title.clear();
        m_subtitle = n->ref == QLatin1String("bi.comment") ? QStringLiteral("note")
                                                           : QStringLiteral("raw");
    }
    layoutPins();
    setPos(n->x, n->y);
    update();
}

// Which mod this node came from, read off the node's own key. Going through the
// key rather than through ModIndex is deliberate: a project opened on a machine
// that does not have the mod installed has never indexed it, and the badge is
// most worth drawing exactly then.
void NodeItem::resolveSource(const GraphNode &node)
{
    m_sourceTag.clear();
    m_sourceColor = QColor();

    const QString depId = ModIndex::dependencyIdOf(node.ref);
    if (depId.isEmpty() || !m_doc) return;

    const ModDependency *dep = m_doc->project().dependency(depId);
    // A key naming a dependency the project has since dropped still says where
    // the node came from, so the addon id answers for the missing record.
    const QString tag = dep && !dep->shortName.isEmpty()
                            ? dep->shortName
                            : shortNameFor(dep ? dep->displayName : depId);
    m_sourceTag = tag.isEmpty() ? shortNameFor(depId) : tag.left(kTagChars);
    m_sourceColor = dep && dep->badgeColor.isValid() ? dep->badgeColor
                                                     : badgeColorFor(depId);
}

double NodeItem::sourceTagWidth() const
{
    if (m_sourceTag.isEmpty()) return 0.0;
    const QFontMetricsF fm(tagFont());
    return qMax(kTagHeight, std::ceil(fm.horizontalAdvance(m_sourceTag) + kTagPadX * 2.0));
}

void NodeItem::setDiagnostics(const QVector<Diagnostic> &diags)
{
    m_diags = diags;
    // The tooltip ends with the findings, so it is now out of date. The badge in
    // the header says a node has one; the tooltip is the only place on the
    // canvas that says which.
    m_tipReady = false;
    setToolTip(QString());
    update();
}

// Lexes the block once and freezes it into coloured runs at fixed offsets, then
// sizes the node to what came out. Runs on every refresh, not every repaint.
void NodeItem::layoutCode(const GraphNode &node)
{
    m_code.clear();
    m_codeHidden = 0;
    m_codeLineHeight = 0.0;
    m_width = width;
    if (!carriesCode(node.ref)) return;

    const QStringList lines = displayLines(codeOf(node));
    if (lines.isEmpty()) return;

    const QFont cf = codeFont();
    const QFontMetricsF cm(cf);
    m_codeLineHeight = std::ceil(cm.height());

    const int shown = qMin(lines.size(), kCodeMaxLines);
    m_codeHidden = lines.size() - shown;

    double widest = 0.0;
    for (int i = 0; i < shown; ++i)
        widest = qMax(widest, cm.horizontalAdvance(lines.at(i)));
    if (m_codeHidden > 0) {
        widest = qMax(widest, QFontMetricsF(smallFont())
                                  .horizontalAdvance(hiddenLineNote(m_codeHidden)));
    }
    m_width = qBound(width, std::ceil(widest) + kCodeIndent * 2.0, kCodeMaxWidth);

    const double avail = m_width - kCodeIndent * 2.0;
    const double cutWidth = cm.horizontalAdvance(kCut);
    const Catalog *cat = m_doc ? &m_doc->catalog() : nullptr;
    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    const QSet<QString> locals = graph ? graphSymbols(*graph) : QSet<QString>();
    // A sticky note holds prose, not script; lexing it would paint half of an
    // English sentence as if it were code.
    const bool prose = node.ref == QLatin1String("bi.comment");

    LexState state = LexState::Normal;
    for (int i = 0; i < shown; ++i) {
        const QVector<Token> tokens = EnforceLexer::tokenize(lines.at(i), state);
        CodeLine cl;
        double x = 0.0;
        for (const Token &t : tokens) {
            const double adv = cm.horizontalAdvance(t.text);
            if (t.kind == TokenKind::Whitespace) {
                x += adv;
                continue;
            }

            QColor color = syntaxColor(prose ? TokenKind::Comment : t.kind);
            if (!prose && t.kind == TokenKind::Identifier) {
                // Resolved in the order the editor uses: a vanilla class first,
                // then a name this graph declares, then just an identifier.
                if (cat && cat->classId(t.text) >= 0)
                    color = theme::syntax::className();
                else if (locals.contains(t.text))
                    color = theme::syntax::localName();
            }

            if (x + adv > avail) {
                cl.elided = true;
                // Cut inside the token rather than dropping it whole: the token
                // that overflows is usually a long string literal, and losing
                // all of it leaves the node wide and half empty.
                const QString head = fitPrefix(cm, t.text, avail - cutWidth - x);
                if (!head.isEmpty()) {
                    cl.runs.append({head, color, x});
                    x += cm.horizontalAdvance(head);
                }
                // Nothing of it fit, so give the marker room by dropping back
                // over what is already there.
                while (head.isEmpty() && !cl.runs.isEmpty()
                       && cl.runs.last().x + cm.horizontalAdvance(cl.runs.last().text)
                              > avail - cutWidth) {
                    x = cl.runs.last().x;
                    cl.runs.removeLast();
                }
                cl.cutX = qBound(0.0, x, qMax(0.0, avail - cutWidth));
                break;
            }

            cl.runs.append({t.text, color, x});
            x += adv;
        }
        m_code.append(cl);
    }
}

double NodeItem::codeBlockHeight() const
{
    if (m_code.isEmpty()) return 0.0;
    const double footer = m_codeHidden > 0 ? m_codeLineHeight : 0.0;
    return kCodeVPad * 2.0 + m_code.size() * m_codeLineHeight + footer;
}

// Pin geometry is computed here and nowhere else: painting, hit-testing and
// wire endpoints all read m_pins, so they cannot drift apart.
// How wide the node has to be for its own text. Inputs and outputs share a row,
// so the row needs both labels, the gap between them, and the value field when
// the input carries one.
double NodeItem::contentWidth(const QVector<Pin> &dataIn,
                              const QVector<Pin> &dataOut,
                              const GraphNode *node) const
{
    const QFontMetricsF tm(titleFont());
    const QFontMetricsF sm(smallFont());
    const QFontMetricsF vm(theme::uiFont(7));

    // Header: title, a gap, then the owning class, then the corner markers.
    // Room for the diagnostics badge is held whether or not this node has one,
    // so gaining a finding does not resize the node under the reader. The
    // dependency tag is measured instead of assumed, because most nodes are
    // vanilla and would otherwise pay width for a marker they never draw.
    const double tag = sourceTagWidth();
    double widest = tm.horizontalAdvance(m_title) + padding * 3.0
                    + sm.horizontalAdvance(m_subtitle) + kBadgeRadius * 2.0
                    + (tag > 0.0 ? tag + kTagGap : 0.0);

    // The footer, when there is one: the count on the left and the two buttons
    // on the right, which must not overlap at the default width.
    if (m_def.list.valid()) {
        widest = qMax(widest, kLabelInset * 2.0
                                  + sm.horizontalAdvance(QStringLiteral("00 elements"))
                                  + padding + kListButton * 2.0 + kListGap);
    }

    // Every value field on the node shares one right edge, taken from the row
    // that can spare the least, so the width has to be solved against that one
    // rather than against each row's own output label.
    double outRoom = 0.0;
    for (int i = 0; i < dataOut.size() && i < dataIn.size(); ++i) {
        if (dataOut.at(i).label.isEmpty()) continue;
        if (fieldEditorFor(dataIn.at(i).type) == InlineEditor::None) continue;
        outRoom = qMax(outRoom, sm.horizontalAdvance(dataOut.at(i).label)
                                    + kLabelInset + padding);
    }

    for (int i = 0; i < qMax(dataIn.size(), dataOut.size()); ++i) {
        double row = kLabelInset * 2.0;
        if (i < dataIn.size()) {
            const Pin &p = dataIn.at(i);
            row += sm.horizontalAdvance(p.label);
            // An unconnected literal gets a field on the same row, and the
            // field has to hold its own text rather than eliding it too. What
            // the author typed is measured, not the declaration's default: the
            // value on the node is the thing they came to read, and a node
            // sized to "unset" shows them the first eight characters of it.
            if (fieldEditorFor(p.type) != InlineEditor::None) {
                QString value = node ? node->inputs.value(p.id) : QString();
                if (value.isEmpty()) value = p.def;
                if (value.isEmpty()) value = QStringLiteral("unset");
                // An enum draws a chevron inside the box, so its text stops
                // short of the edge by that much again.
                const double chevron =
                    p.type.kind == PinKind::Enum && !p.type.isArray ? 7.0 : 0.0;
                const double text = vm.horizontalAdvance(value) + padding + chevron;
                row += padding + qMax(kMinFieldWidth, text);
                // The field is PLACED as a fraction of the finished width
                // rather than packed after the label, so summing the row is
                // not enough: the width has to be solved for, or a value wide
                // enough to have widened the node still elides inside a box
                // that grew by less than the node did.
                widest = qMax(widest, (qMax(kMinFieldWidth, text) + outRoom) / 0.48);
                widest = qMax(widest, (kLabelInset + sm.horizontalAdvance(p.label) + 3.0)
                                          / 0.46);
            }
        }
        if (i < dataOut.size()) row += padding * 2.0 + sm.horizontalAdvance(dataOut.at(i).label);
        widest = qMax(widest, row);
    }
    return std::ceil(widest);
}

bool NodeItem::alreadyOnHeader(const QString &type) const
{
    return !type.isEmpty() && (type == m_title || type == m_subtitle);
}

void NodeItem::layoutPins()
{
    m_pins.clear();
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;

    QVector<Pin> execIn, execOut, dataIn, dataOut;
    for (const Pin &p : m_def.pins) {
        const bool isExec = p.type.kind == PinKind::Exec;
        if (p.dir == PinDir::In) (isExec ? execIn : dataIn).append(p);
        else (isExec ? execOut : dataOut).append(p);
    }

    const int execRows = qMax(execIn.size(), execOut.size());
    const int dataRows = qMax(dataIn.size(), dataOut.size());
    const int rows = execRows + dataRows;

    m_listRow = QRectF();
    m_listMinus = QRectF();
    m_listPlus = QRectF();
    m_listCount = 0;
    const GraphNode *node = g ? g->node(m_nodeId) : nullptr;
    const bool hasList = m_def.list.valid() && node;
    if (hasList) m_listCount = bi::listCount(*node, m_def.list);

    // Widen to whatever the node has to say. A catalogue call carries its
    // optional parameters as `name = DEFAULT`, and at the fixed width those
    // came out as `plugin = LOG_D...`, which names neither the parameter nor
    // the default. A code node has already sized itself, so leave it alone.
    if (m_code.isEmpty())
        m_width = qBound(width, contentWidth(dataIn, dataOut, node), kMaxWidth);

    // A raw node's pins ride on the header itself: one exec in, one exec out,
    // and nothing to fill a row of their own with. Given a row below the header
    // they open a band of blank node between the header and the code.
    m_pinsOnHeader = !m_code.isEmpty() && rows <= 1;
    m_headerHeight = m_pinsOnHeader ? kCodeHeaderHeight : headerHeight;
    const double rowsTop = m_pinsOnHeader ? (m_headerHeight - pinRow) / 2.0
                                          : m_headerHeight + padding;
    const double codeH = codeBlockHeight();
    m_codeTop = m_pinsOnHeader ? m_headerHeight : rowsTop + rows * pinRow;
    m_height = m_codeTop + (codeH > 0.0 ? codeH : padding);
    m_height = qMax(m_height, m_headerHeight + padding * 2);

    // The footer goes under everything else, so growing the list pushes the
    // controls down with the pins they belong to rather than the other way
    // round. A code node never has one; the two shapes do not combine.
    if (hasList && m_code.isEmpty()) {
        m_listRow = QRectF(0.0, m_codeTop, m_width, pinRow);
        const double top = m_listRow.center().y() - kListButton / 2.0;
        m_listPlus = QRectF(m_width - padding - kListButton, top, kListButton, kListButton);
        m_listMinus = QRectF(m_listPlus.left() - kListGap - kListButton, top, kListButton,
                             kListButton);
        m_height = m_listRow.bottom() + padding;
    }

    // What the output on a data row wants for its own label. The value field
    // has to stop short of it: the field is placed as a fraction of the width
    // and an output label is right-aligned, so on a row carrying both they ran
    // over each other. Make Array is the first node with a labelled output
    // beside a field, but nothing stopped an existing one from having one.
    // One right edge for every value field on the node, taken from the row that
    // can spare the least. A per-row edge would leave a ragged column, and the
    // column is the thing a reader scans down.
    const QFontMetricsF labelMetrics(smallFont());
    const double fieldLeft = m_width * 0.46;
    double fieldRight = m_width * 0.94;
    for (int i = 0; i < dataOut.size() && i < dataIn.size(); ++i) {
        if (dataOut.at(i).label.isEmpty()) continue;
        const Pin &in = dataIn.at(i);
        if (fieldEditorFor(in.type) == InlineEditor::None) continue;
        fieldRight = qMin(fieldRight,
                          m_width * 0.94
                              - labelMetrics.horizontalAdvance(dataOut.at(i).label)
                              - kLabelInset - padding);
    }
    fieldRight = qMax(fieldRight, fieldLeft + kMinFieldWidth);

    // One signature for the whole node. Every pin's declared type comes out of
    // it, and building it per pin would allocate a parameter vector per row.
    MethodSig sig;
    if (m_doc && node) {
        sig = m_doc->catalog().method(node->ref);
        if (!sig.valid) sig = m_doc->catalog().globalFn(node->ref);
    }

    const auto place = [&](const QVector<Pin> &list, int firstRow, bool left, bool data) {
        for (int i = 0; i < list.size(); ++i) {
            PinLayout pl;
            pl.pin = list.at(i);
            const double y = rowsTop + (firstRow + i) * pinRow + pinRow / 2.0;
            pl.pos = QPointF(left ? 0.0 : m_width, y);
            if (g) {
                pl.connected = left
                    ? edgeInto(*g, m_nodeId, pl.pin.id) != nullptr
                    : edgeFrom(*g, m_nodeId, pl.pin.id) != nullptr;
            }
            const bool editable = left && pl.pin.type.kind != PinKind::Exec
                                  && !pl.connected
                                  && fieldEditorFor(pl.pin.type) != InlineEditor::None;
            if (editable) {
                const double h = pinRow - 4.0;
                pl.editor = QRectF(fieldLeft, y - h / 2.0, fieldRight - fieldLeft, h);
                // The row band the box sits in, reaching a little past it on
                // both sides. The box is four units shorter than its row, and
                // every one of those units used to be a press that selected the
                // node instead of editing it. It stops short of the node edge
                // on purpose: an output pin can share this row, and a press
                // that just misses one must not edit the input instead.
                const double left = pl.editor.left() - 4.0;
                pl.hit = QRectF(left, y - pinRow / 2.0,
                                pl.editor.right() + 2.0 - left, pinRow);
            }
            // An input with a field owns the space up to it; one without gets
            // the same share it always had. An output stops where the field on
            // its row ends, and takes the old share when there is none.
            const bool sharesWithField =
                data && !left && i < dataIn.size()
                && fieldEditorFor(dataIn.at(i).type) != InlineEditor::None
                && (g ? edgeInto(*g, m_nodeId, dataIn.at(i).id) == nullptr : true);
            if (left) {
                pl.labelRoom = pl.editor.isEmpty() ? m_width * 0.62
                                                   : pl.editor.left() - kLabelInset - 3.0;
            } else {
                pl.labelRoom = sharesWithField
                                   ? m_width - kLabelInset - fieldRight - padding
                                   : m_width * 0.62;
            }

            // What the row says about the type it carries. Three rows say it
            // another way and do not repeat themselves: an exec pin has no
            // type, a row with a value field spends its width on the literal
            // the author typed, which answers the question harder than the type
            // name does, and a pin whose type is already a word on the header
            // (every `target`, whose class IS the subtitle) has been answered
            // once already.
            if (pl.pin.type.kind != PinKind::Exec && pl.editor.isEmpty()) {
                const QString declared = declaredPinType(sig, pl.pin);
                if (!alreadyOnHeader(declared)) pl.typeText = declared;
            }
            m_pins.append(pl);
        }
    };

    // Exec pins own the first rows on both sides, so a flow reads top to
    // bottom no matter how many parameters a call has. Data inputs are placed
    // before data outputs so an output on a shared row already knows where the
    // field beside it stopped.
    place(execIn, 0, true, false);
    place(execOut, 0, false, false);
    place(dataIn, execRows, true, true);
    place(dataOut, execRows, false, true);

    // The rule between the flow rows and the data rows. It is the only division
    // a node gets, because it is the only one that is real: above it the node
    // says when it runs, below it what it works on. A second rule between the
    // receiver and the arguments would put two lines on a three-row node.
    m_bandRule = (execRows > 0 && dataRows > 0 && m_code.isEmpty())
                     ? rowsTop + execRows * pinRow
                     : 0.0;

    // The declaration's sentence goes in the exec row, which on a call node
    // holds a triangle at each edge and a hundred and fifty units of nothing in
    // between. That is the "mostly empty body": not padding, a whole row the
    // node was already paying for. Whatever labels the exec pins carry are
    // measured out of the way first, so Branch's `true` keeps its corner.
    m_summaryRect = QRectF();
    if (!m_summary.isEmpty() && execRows > 0 && m_code.isEmpty()) {
        const QFontMetricsF sm(smallFont());
        double from = kLabelInset;
        double to = m_width - kLabelInset;
        for (const PinLayout &pl : m_pins) {
            if (pl.pin.type.kind != PinKind::Exec) continue;
            if (pl.pos.y() > rowsTop + pinRow) continue;
            const double label = pl.pin.label.isEmpty()
                                     ? 0.0
                                     : sm.horizontalAdvance(pl.pin.label) + kTypeGap;
            const double clear = kExecHalfWidth + 4.0 + label;
            if (pl.pin.dir == PinDir::In) from = qMax(from, clear);
            else to = qMin(to, m_width - clear);
        }
        if (to - from >= kMinSummaryWidth)
            m_summaryRect = QRectF(from, rowsTop, to - from, pinRow);
    }
}

QPointF NodeItem::pinScenePos(const QString &pinId, PinDir dir) const
{
    for (const PinLayout &pl : m_pins)
        if (pl.pin.id == pinId && pl.pin.dir == dir) return mapToScene(pl.pos);
    // An edge naming a pin this def no longer has: anchor it on the header so
    // the stale wire is visible instead of shooting off to the origin.
    return mapToScene(QPointF(dir == PinDir::In ? 0.0 : m_width, m_headerHeight / 2.0));
}

QString NodeItem::pinAt(const QPointF &scenePos, PinDir *dirOut, double reach,
                        double *distanceOut) const
{
    const QPointF local = mapFromScene(scenePos);
    QString best;
    double bestDist = reach > 0.0 ? reach : kPinGrab;
    for (const PinLayout &pl : m_pins) {
        const QPointF d = local - pl.pos;
        const double dist = std::hypot(d.x(), d.y());
        if (dist > bestDist) continue;
        bestDist = dist;
        best = pl.pin.id;
        if (dirOut) *dirOut = pl.pin.dir;
    }
    if (!best.isEmpty() && distanceOut) *distanceOut = bestDist;
    return best;
}

QString NodeItem::editorAt(const QPointF &scenePos, double reach) const
{
    const QPointF local = mapFromScene(scenePos);
    // Vertical only. A row is thirteen units tall and the box inside it is
    // nine, so height is where the misses happen; the box is already eighty
    // units wide, and growing that direction would reach the pin columns.
    const double grow = qBound(0.0, reach, pinRow);
    QString best;
    double bestDist = std::numeric_limits<double>::max();
    for (const PinLayout &pl : m_pins) {
        if (pl.hit.isEmpty()) continue;
        if (!pl.hit.adjusted(0.0, -grow, 0.0, grow).contains(local)) continue;
        // Grown bands overlap their neighbours, so the winner is the row whose
        // centre line the press is nearest rather than whichever comes first.
        const double dist = std::fabs(local.y() - pl.pos.y());
        if (dist >= bestDist) continue;
        bestDist = dist;
        best = pl.pin.id;
    }
    return best;
}

int NodeItem::listButtonAt(const QPointF &scenePos, double reach) const
{
    if (m_listMinus.isEmpty()) return 0;
    const QPointF local = mapFromScene(scenePos);
    // Grown the same way a value field is, and for the same reason: nine scene
    // units is a seven pixel target once a long graph is zoomed to fit. The
    // two grown boxes overlap in the gap between them, so the winner is the
    // nearer centre rather than whichever is tested first. Testing in order
    // would make every press in that gap a removal.
    const double grow = qBound(0.0, reach, kListButton);
    const QMarginsF pad(grow, grow, grow, grow);
    const QRectF pair = m_listMinus.united(m_listPlus).marginsAdded(pad);
    if (!pair.contains(local)) return 0;
    return local.x() < (m_listMinus.right() + m_listPlus.left()) / 2.0 ? -1 : 1;
}

const NodeItem::PinLayout *NodeItem::layoutForPin(const QString &pinId) const
{
    for (const PinLayout &pl : m_pins)
        if (pl.pin.dir == PinDir::In && pl.pin.id == pinId) return &pl;
    return nullptr;
}

QVector<NodeInput> NodeItem::editableInputs() const
{
    return m_doc ? nodeInputsOf(*m_doc, m_nodeId) : QVector<NodeInput>();
}

void NodeItem::activateEditor(const QString &pinId, QWidget *host, const QPoint &at)
{
    if (!m_doc || !layoutForPin(pinId)) return;
    bool found = false;
    const NodeInput in = nodeInputOf(*m_doc, m_nodeId, pinId, &found);
    if (!found || in.connected || in.editor == InlineEditor::None) return;

    if (in.editor == InlineEditor::Checkbox) {
        // Two states need no dialog to choose between: the click is the whole
        // gesture. The commit rebuilds the scene, so nothing may touch this
        // item afterwards.
        setNodeInput(m_doc, m_nodeId, pinId, toggledBool(in.value));
        return;
    }

    // Opened from a queued call so the press and the release that started it
    // land first. A popup grab or a modal loop taken mid-gesture leaves the
    // view waiting on a release it never receives.
    Document *doc = m_doc;
    const QString nodeId = m_nodeId;
    QPointer<QWidget> parent(host);
    QTimer::singleShot(0, this, [doc, nodeId, pinId, parent, at, in]() {
        const QString title = QStringLiteral("Set value");
        const QString prompt = QStringLiteral("%1  (%2)").arg(in.label, in.typeName);

        if (!in.choices.isEmpty() && in.choices.size() <= kMenuMembers) {
            QMenu menu(parent);
            for (const QString &member : in.choices) {
                QAction *row = menu.addAction(member);
                row->setData(member);
                row->setCheckable(true);
                row->setChecked(member == in.value.trimmed());
            }
            const QAction *chosen = menu.exec(at);
            if (chosen) setNodeInput(doc, nodeId, pinId, chosen->data().toString());
            return;
        }

        if (!in.choices.isEmpty()) {
            bool ok = false;
            const int current = qMax(0, in.choices.indexOf(in.value.trimmed()));
            // Editable, because a parameter typed as an enum still accepts a
            // raw value and the catalogue only knows the named members.
            const QString picked = QInputDialog::getItem(parent, title, prompt,
                                                         in.choices, current, true, &ok);
            if (ok) setNodeInput(doc, nodeId, pinId, picked);
            return;
        }

        bool ok = false;
        // Text rather than a spin box even for numbers: Enforce takes a constant
        // expression here, and a spin box would refuse to hold one.
        const QString entered = QInputDialog::getText(parent, title, prompt,
                                                      QLineEdit::Normal, in.value, &ok);
        if (ok) setNodeInput(doc, nodeId, pinId, entered);
    });
}

void NodeItem::setHoverEditor(const QString &pinId)
{
    if (m_hoverEditor == pinId) return;
    m_hoverEditor = pinId;
    update();
}

void NodeItem::setHoverPin(const QString &pinId, PinDir dir)
{
    if (m_hoverPin == pinId && m_hoverDir == dir) return;
    m_hoverPin = pinId;
    m_hoverDir = dir;
    update();
}

void NodeItem::clearHoverPin()
{
    if (m_hoverPin.isEmpty()) return;
    m_hoverPin.clear();
    update();
}

void NodeItem::setDropCandidate(bool candidate)
{
    if (m_dropCandidate == candidate) return;
    m_dropCandidate = candidate;
    update();
}

QRectF NodeItem::bodyRect() const
{
    return QRectF(0.0, 0.0, m_width, m_height);
}

QRectF NodeItem::boundingRect() const
{
    // Pins straddle both edges and the selection outline sits outside the body.
    // Wide enough to contain shape()'s pin caps, which Qt requires. The note
    // strip sits above the header, outside the body but inside the bounds.
    const double above = 4.0 + (m_note.isEmpty() ? 0.0 : kNoteHeight);
    return QRectF(-kPinGrab - 1.0, -above, m_width + kPinGrab * 2.0 + 2.0,
                  m_height + above + 4.0);
}

QPainterPath NodeItem::shape() const
{
    QPainterPath path;
    path.addRoundedRect(bodyRect(), radius, radius);
    // Only the pins reach past the body. Taking the whole inflated rect made a
    // six-unit invisible band around every node behave like the node itself.
    for (const PinLayout &pl : m_pins)
        path.addEllipse(pl.pos, kPinGrab, kPinGrab);
    return path.simplified();
}

// The well is a shade under the node body, which is what makes a code node
// read as a block of script rather than a list of pin rows.
void NodeItem::paintCode(QPainter *p, const QPainterPath &bodyPath) const
{
    if (m_code.isEmpty()) return;

    QPainterPath wellClip;
    wellClip.addRect(QRectF(0.0, m_codeTop, m_width, m_height - m_codeTop));
    p->fillPath(bodyPath.intersected(wellClip), theme::windowBg());
    // Against the header accent the well already has an edge; the rule is only
    // needed where a pin row sits between the two.
    if (m_codeTop > m_headerHeight) {
        p->setPen(QPen(theme::border(), 0.6));
        p->drawLine(QLineF(0.0, m_codeTop, m_width, m_codeTop));
    }

    const QFont cf = codeFont();
    const QFontMetricsF cm(cf);
    p->setFont(cf);
    double y = m_codeTop + kCodeVPad + cm.ascent();
    for (const CodeLine &cl : m_code) {
        for (const CodeRun &r : cl.runs) {
            p->setPen(r.color);
            p->drawText(QPointF(kCodeIndent + r.x, y), r.text);
        }
        if (cl.elided) {
            p->setPen(theme::textDim());
            p->drawText(QPointF(kCodeIndent + cl.cutX, y), kCut);
        }
        y += m_codeLineHeight;
    }

    if (m_codeHidden > 0) {
        p->setFont(smallFont());
        p->setPen(theme::textDim());
        p->drawText(QRectF(kCodeIndent, y - cm.ascent(), m_width - kCodeIndent * 2.0,
                           m_codeLineHeight),
                    Qt::AlignLeft | Qt::AlignVCenter, hiddenLineNote(m_codeHidden));
    }
}

// One inline value box. Hovered rows take the pin's colour on the border so the
// row that a press would act on is named before the press, the way a hovered
// pin is ringed.
void NodeItem::paintValueField(QPainter *p, const PinLayout &pl,
                               const QString &value) const
{
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *node = g ? g->node(m_nodeId) : nullptr;
    const bool overridden = node && node->inputs.contains(pl.pin.id);
    const bool hovered = pl.pin.id == m_hoverEditor;
    const QColor accent = pinColor(pl.pin.type.kind);

    // drawText clips to the rect it is handed, and the box is four units
    // shorter than its row, which is enough to cut the descender off a 'p' or a
    // 'g'. The box keeps the height it is drawn at; the text gets the whole row.
    const auto textRow = [&pl](double x, double w) {
        return QRectF(x, pl.pos.y() - pinRow / 2.0, w, pinRow);
    };

    QPainterPath field;
    field.addRoundedRect(pl.editor, 2.0, 2.0);
    p->fillPath(field, theme::windowBg());
    QColor edge = theme::border();
    if (hovered) {
        edge = accent;
        edge.setAlpha(190);
    }
    p->setPen(QPen(edge, hovered ? 0.9 : 0.6));
    p->setBrush(Qt::NoBrush);
    p->drawPath(field);

    if (inlineEditorFor(pl.pin.type) == InlineEditor::Checkbox) {
        const double side = pl.editor.height() - 3.0;
        const QRectF box(pl.editor.left() + 3.0,
                         pl.editor.center().y() - side / 2.0, side, side);
        p->setPen(QPen(hovered ? theme::text() : theme::textDim(), 0.8));
        p->drawRect(box);
        if (isTrueLiteral(value)) {
            p->setPen(QPen(accent, 1.4));
            p->drawLine(QLineF(box.left() + 1.4, box.center().y(),
                               box.center().x(), box.bottom() - 1.4));
            p->drawLine(QLineF(box.center().x(), box.bottom() - 1.4,
                               box.right() - 1.2, box.top() + 1.2));
        }
        // The word beside the box: at a glance a ticked box and an unticked one
        // differ by a few pixels, and false is the state worth being sure about.
        const QFont sf = smallFont();
        const QFontMetricsF sm(sf);
        p->setFont(sf);
        p->setPen(valueColor(pl.pin.type, overridden));
        const QRectF tr = textRow(box.right() + 3.0,
                                  pl.editor.right() - box.right() - 5.0);
        if (tr.width() > 8.0) {
            p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter,
                        sm.elidedText(isTrueLiteral(value) ? QStringLiteral("true")
                                                           : QStringLiteral("false"),
                                      Qt::ElideRight, tr.width()));
        }
        return;
    }

    // Enums open a list, so they get the mark a combo box has. Without it the
    // row looks like a box you have to know the members of to fill in.
    const bool listed = pl.pin.type.kind == PinKind::Enum && !pl.pin.type.isArray;
    const double chevron = listed ? 7.0 : 0.0;
    if (listed) {
        const double cx = pl.editor.right() - 5.0;
        const double cy = pl.editor.center().y();
        p->setPen(QPen(hovered ? accent : theme::textDim(), 0.9));
        p->drawLine(QLineF(cx - 2.4, cy - 1.2, cx, cy + 1.4));
        p->drawLine(QLineF(cx, cy + 1.4, cx + 2.4, cy - 1.2));
    }

    const QFont sf = smallFont();
    const QFontMetricsF sm(sf);
    p->setFont(sf);
    const QRectF tr = textRow(pl.editor.left() + 3.0,
                              pl.editor.width() - 6.0 - chevron);
    if (tr.width() <= 0.0) return;

    if (value.isEmpty()) {
        p->setPen(theme::textDim());
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter,
                    sm.elidedText(QStringLiteral("unset"), Qt::ElideRight, tr.width()));
        return;
    }
    p->setPen(valueColor(pl.pin.type, overridden));
    p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter,
                sm.elidedText(value, Qt::ElideRight, tr.width()));
}

// A pin's name and the type it carries, sharing the row's room.
//
// The name is what a reader is hunting for and the type is what they can get
// back by hovering, so when the two do not both fit the type gives up its half,
// and gives it up whole rather than shrinking to two letters and a cut mark.
// The one exception is `return`, which is not a name at all: it restates which
// side of the node the pin is on, and on that row the type is the only thing
// worth the width.
void NodeItem::paintPinText(QPainter *p, const PinLayout &pl) const
{
    const bool left = pl.pin.dir == PinDir::In;
    const double room = pl.labelRoom;
    if (room <= 10.0) return;

    QString label = pl.pin.label;
    QString type = pl.typeText;
    if (isPlaceholderLabel(label) && !type.isEmpty()) label.clear();
    if (label.isEmpty() && type.isEmpty()) return;

    const QFont sf = smallFont();
    const QFontMetricsF sm(sf);
    p->setFont(sf);

    double labelW = label.isEmpty() ? 0.0 : sm.horizontalAdvance(label);
    const double gap = (label.isEmpty() || type.isEmpty()) ? 0.0 : kTypeGap;
    double typeW = type.isEmpty() ? 0.0 : sm.horizontalAdvance(type);
    if (labelW + gap + typeW > room) {
        typeW = qMax(0.0, room - labelW - gap);
        if (typeW < kMinTypeWidth) {
            type.clear();
            typeW = 0.0;
        }
    }
    labelW = qMin(labelW, room - typeW - (type.isEmpty() ? 0.0 : kTypeGap));

    // The whole row is used for the vertical extent so a descender is not cut
    // off, the same reason paintValueField measures its text against the row.
    const double top = pl.pos.y() - pinRow / 2.0;
    // Names run outward from the pin, so an input reads name then type left to
    // right and an output reads type then name into the right edge. Both put
    // the name against the node's own edge, which is the column a reader scans.
    const double outerX = left ? kLabelInset : m_width - kLabelInset - labelW;
    const double typeX = left ? outerX + labelW + kTypeGap
                              : m_width - kLabelInset - labelW - kTypeGap - typeW;

    if (!label.isEmpty()) {
        p->setPen(theme::textDim());
        p->drawText(QRectF(outerX, top, labelW, pinRow),
                    (left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                    sm.elidedText(label, Qt::ElideRight, labelW));
    }
    if (type.isEmpty()) return;

    // The type takes the pin's own colour, pulled back. The dot on the edge is
    // already saying this in the loudest form it has; the word is here to say
    // which class, not to say it twice as brightly.
    QColor c = pinColor(pl.pin.type.kind);
    c.setAlpha(kTypeAlpha);
    p->setPen(c);
    const double x = label.isEmpty() ? (left ? kLabelInset : m_width - kLabelInset - typeW)
                                     : typeX;
    p->drawText(QRectF(x, top, typeW, pinRow),
                (left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                sm.elidedText(type, Qt::ElideRight, typeW));
}

// The sentence Bohemia wrote about this declaration, and the rule under the
// flow rows. Both draw into space the node already had.
void NodeItem::paintSummary(QPainter *p) const
{
    if (m_bandRule > 0.0) {
        QColor rule = theme::border();
        rule.setAlpha(120);
        p->setPen(QPen(rule, 0.6));
        p->drawLine(QLineF(padding, m_bandRule, m_width - padding, m_bandRule));
    }
    if (m_summaryRect.isEmpty()) return;

    const QFont sf = smallFont();
    const QFontMetricsF sm(sf);
    p->setFont(sf);
    // The same colour the author's own note above the header takes. Both are
    // somebody's prose about this node rather than part of it, and reading as
    // one thing is right: one was written in the mod, one in P:\scripts.
    p->setPen(theme::syntax::comment());
    p->drawText(m_summaryRect, Qt::AlignLeft | Qt::AlignVCenter,
                sm.elidedText(m_summary, Qt::ElideRight, m_summaryRect.width()));
}

// "3 elements" and the two controls that change it. The buttons are drawn
// hollow and take the accent only under the cursor, so a node full of value
// fields does not gain two more things competing for attention.
void NodeItem::paintListRow(QPainter *p) const
{
    if (m_listRow.isEmpty()) return;

    p->setPen(QPen(theme::border(), 0.6));
    p->drawLine(QLineF(padding, m_listRow.top(), m_width - padding, m_listRow.top()));

    const QFont sf = smallFont();
    p->setFont(sf);
    p->setPen(theme::textDim());
    const QString label = m_listCount == 1
                              ? QStringLiteral("1 %1").arg(m_def.list.label).left(64)
                              : QStringLiteral("%1 %2").arg(m_listCount).arg(m_def.list.label);
    p->drawText(QRectF(kLabelInset, m_listRow.top(), m_listMinus.left() - kLabelInset - 3.0,
                       m_listRow.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(sf).elidedText(label, Qt::ElideRight,
                                             m_listMinus.left() - kLabelInset - 3.0));

    const auto button = [&](const QRectF &box, bool plus, bool hovered, bool enabled) {
        QPainterPath path;
        path.addRoundedRect(box, 2.0, 2.0);
        p->fillPath(path, theme::windowBg());
        QColor edge = enabled ? theme::border() : theme::border().darker(130);
        if (hovered && enabled) {
            edge = theme::accent();
            edge.setAlpha(200);
        }
        p->setPen(QPen(edge, hovered && enabled ? 0.9 : 0.6));
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);

        const QPointF c = box.center();
        const double arm = box.width() / 2.0 - 2.6;
        p->setPen(QPen(enabled ? (hovered ? theme::text() : theme::textDim())
                               : theme::textDim().darker(140),
                       1.1));
        p->drawLine(QLineF(c.x() - arm, c.y(), c.x() + arm, c.y()));
        if (plus) p->drawLine(QLineF(c.x(), c.y() - arm, c.x(), c.y() + arm));
    };

    button(m_listMinus, false, m_hoverListButton < 0, m_listCount > m_def.list.min);
    button(m_listPlus, true, m_hoverListButton > 0, m_listCount < m_def.list.max);
}

void NodeItem::paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w)
{
    Q_UNUSED(opt);
    Q_UNUSED(w);
    p->setRenderHint(QPainter::Antialiasing, true);

    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *node = g ? g->node(m_nodeId) : nullptr;
    const QRectF body(0, 0, m_width, m_height);

    if (!m_note.isEmpty()) {
        const QFont nf = smallFont();
        const QFontMetricsF nm(nf);
        p->setFont(nf);
        p->setPen(theme::syntax::comment());
        const QRectF strip(0.0, -kNoteHeight, m_width, kNoteHeight);
        p->drawText(strip, Qt::AlignLeft | Qt::AlignVCenter,
                    nm.elidedText(m_note, Qt::ElideRight, m_width));
    }

    QPainterPath bodyPath;
    bodyPath.addRoundedRect(body, radius, radius);
    p->fillPath(bodyPath, theme::nodeBody());

    // The alternate shade marks the rows that carry an editable value, which
    // is the only place a click does something other than select.
    for (const PinLayout &pl : m_pins) {
        if (pl.editor.isEmpty()) continue;
        const QRectF row(1.0, pl.pos.y() - pinRow / 2.0, m_width - 2.0, pinRow);
        p->fillRect(row, theme::nodeBodyAlt());
    }

    paintCode(p, bodyPath);

    QPainterPath headerClip;
    headerClip.addRect(QRectF(0, 0, m_width, m_headerHeight));
    QColor accent = m_def.valid && m_def.accent.isValid()
                        ? m_def.accent
                        : theme::errorColor().darker(220);
    p->fillPath(bodyPath.intersected(headerClip), accent);

    bool hasDiags = false;
    const Severity worst = worstOf(m_diags, &hasDiags);

    const QFont tf = titleFont();
    const QFont sf = smallFont();
    const QFontMetricsF tm(tf);
    const QFontMetricsF sm(sf);

    // The two header markers, right to left: the diagnostics badge keeps the
    // corner it has always had, and the dependency tag goes to its left. Moving
    // the badge instead would make a node's own state jump sideways depending
    // on which mod it came from.
    const double tagW = sourceTagWidth();
    const double diagSpace = hasDiags ? kBadgeRadius * 2.0 + 2.0 : 0.0;
    const double badgeSpace = diagSpace + (tagW > 0.0 ? tagW + kTagGap : 0.0);
    // An output pin drawn on the header would otherwise sit under the marker.
    const double pinClearance = m_pinsOnHeader ? kExecHalfWidth + 3.0 : 0.0;
    const QRectF headText(padding, 0,
                          m_width - padding * 2 - badgeSpace - pinClearance,
                          m_headerHeight);

    // The subtitle is measured first and the title gets everything it does not
    // want. A flat fraction of the header truncates short titles next to a gap
    // wide enough to have shown them whole.
    const double subWanted = m_subtitle.isEmpty()
                                 ? 0.0
                                 : sm.horizontalAdvance(m_subtitle) + 6.0;
    const double titleRoom =
        qMax(kMinTitleWidth, headText.width() - qMin(subWanted, headText.width() * 0.5));
    const QString title = tm.elidedText(m_title, Qt::ElideRight, titleRoom);
    p->setFont(tf);
    p->setPen(theme::text());
    p->drawText(headText, Qt::AlignLeft | Qt::AlignVCenter, title);

    if (!m_subtitle.isEmpty()) {
        const double used = tm.horizontalAdvance(title) + 6.0;
        const QRectF subRect = headText.adjusted(used, 0, 0, 0);
        if (subRect.width() > 14.0) {
            p->setFont(sf);
            p->setPen(theme::textDim());
            p->drawText(subRect, Qt::AlignRight | Qt::AlignVCenter,
                        sm.elidedText(m_subtitle, Qt::ElideRight, subRect.width()));
        }
    }

    double markerRight = m_width - padding - pinClearance;
    if (hasDiags) {
        const QPointF c(markerRight - kBadgeRadius + 1.0, m_headerHeight / 2.0);
        p->setPen(Qt::NoPen);
        p->setBrush(severityColor(worst));
        p->drawEllipse(c, kBadgeRadius, kBadgeRadius);
        p->setFont(tagFont());
        p->setPen(theme::windowBg());
        const QString glyph = worst == Severity::Error ? QStringLiteral("x")
                              : worst == Severity::Warning ? QStringLiteral("!")
                                                           : QStringLiteral("i");
        p->drawText(QRectF(c.x() - kBadgeRadius, c.y() - kBadgeRadius,
                           kBadgeRadius * 2, kBadgeRadius * 2),
                    Qt::AlignCenter, glyph);
        markerRight = c.x() - kBadgeRadius - kTagGap;
    }

    if (tagW > 0.0) {
        const QRectF tagRect(markerRight - tagW, (m_headerHeight - kTagHeight) / 2.0,
                             tagW, kTagHeight);
        const QColor c = m_sourceColor.isValid() ? m_sourceColor : theme::accent();
        // Washed rather than filled. The header is already a solid accent, and a
        // second solid block beside the diagnostics dot would read as a second
        // alert instead of as a label saying where the node came from.
        QColor fill = c;
        fill.setAlpha(kTagFillAlpha);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRoundedRect(tagRect, kTagRadius, kTagRadius);
        p->setFont(tagFont());
        p->setPen(c);
        p->drawText(tagRect, Qt::AlignCenter, m_sourceTag);
    }

    for (const PinLayout &pl : m_pins) {
        const QColor c = pinColor(pl.pin.type.kind);

        // The halo says "a press here grabs this pin" before the press happens.
        // Without it the grab area is invisible and a miss looks like the
        // canvas ignoring the drag.
        const bool hovered = !m_hoverPin.isEmpty() && pl.pin.id == m_hoverPin
                             && pl.pin.dir == m_hoverDir;
        if (hovered || m_dropCandidate) {
            QColor halo = c;
            halo.setAlpha(hovered ? 70 : 40);
            p->setPen(Qt::NoPen);
            p->setBrush(halo);
            p->drawEllipse(pl.pos, kPinHalo, kPinHalo);
        }

        if (pl.pin.type.kind == PinKind::Exec) {
            QPolygonF tri;
            tri << QPointF(pl.pos.x() - kExecHalfWidth, pl.pos.y() - kExecHalfHeight)
                << QPointF(pl.pos.x() + kExecHalfWidth, pl.pos.y())
                << QPointF(pl.pos.x() - kExecHalfWidth, pl.pos.y() + kExecHalfHeight);
            p->setPen(QPen(c, 1.0));
            p->setBrush(pl.connected ? QBrush(c) : Qt::NoBrush);
            p->drawPolygon(tri);
        } else {
            const double r = pinRadius + (pl.pin.type.isArray ? 0.9 : 0.0);
            p->setPen(QPen(c, 1.2));
            p->setBrush(pl.connected ? QBrush(c) : QBrush(theme::nodeBody()));
            p->drawEllipse(pl.pos, r, r);
        }

        paintPinText(p, pl);

        if (pl.editor.isEmpty()) continue;
        paintValueField(p, pl, node ? node->inputs.value(pl.pin.id, pl.pin.def)
                                    : pl.pin.def);
    }

    paintSummary(p);
    paintListRow(p);

    QColor outline = theme::nodeOutline();
    double outlineWidth = 1.0;
    if (hasDiags && worst != Severity::Info) {
        outline = severityColor(worst);
        outlineWidth = 1.5;
    }
    p->setBrush(Qt::NoBrush);
    p->setPen(QPen(outline, outlineWidth));
    p->drawPath(bodyPath);

    if (isSelected()) {
        // Drawn outside the body so a selected node with an error keeps both
        // colours instead of one hiding the other.
        QPainterPath sel;
        sel.addRoundedRect(body.adjusted(-2.0, -2.0, 2.0, 2.0),
                           radius + 1.5, radius + 1.5);
        p->setPen(QPen(theme::selection(), 1.5));
        p->drawPath(sel);
    }
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemPositionHasChanged) {
        if (auto *s = qobject_cast<NodeScene *>(scene())) s->nodeMoved(m_nodeId);
    } else if (change == ItemSelectedHasChanged) {
        update();
    }
    return QGraphicsObject::itemChange(change, value);
}

// A value is set by clicking it, not by finding the double-click that used to
// be the only way in. The press is accepted, so this row never starts a node
// drag and the gesture cannot be half an edit and half a move.
void NodeItem::mousePressEvent(QGraphicsSceneMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QGraphicsObject::mousePressEvent(e);
        return;
    }

    const double reach = editorReach(e->widget());
    const int listButton = listButtonAt(e->scenePos(), reach);
    const QString pinId = listButton == 0 ? editorAt(e->scenePos(), reach) : QString();
    if (listButton == 0 && pinId.isEmpty()) {
        QGraphicsObject::mousePressEvent(e);
        return;
    }

    // The view opened a node-move bracket on this press, on the chance it turns
    // into a drag. It cannot now. Closing it here matters because a commit
    // rebuilds the scene, and the fresh items setting their own positions look
    // exactly like a drag to a bracket that is still open: the release would
    // then push a second undo entry for a move that never happened.
    if (auto *s = qobject_cast<NodeScene *>(scene())) s->endNodeMove();

    // Accepting the press skips the base class, so selection is set by hand.
    // The inspector reads the document's selection, and it should already be
    // showing this node's values by the time a popup opens over it.
    if (!isSelected()) {
        if (QGraphicsScene *sc = scene(); sc && !e->modifiers().testFlag(Qt::ControlModifier))
            sc->clearSelection();
        setSelected(true);
    }
    e->accept();
    if (listButton != 0) {
        // The commit rebuilds the scene, so nothing may touch this item after
        // it. Same rule the checkbox in activateEditor works to.
        setNodeListCount(m_doc, m_nodeId, m_listCount + listButton);
        return;
    }
    activateEditor(pinId, e->widget(), e->screenPos());
}

void NodeItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e)
{
    // The press already acted on the field. Letting the second click through
    // would flip a bool straight back, which reads as the click doing nothing.
    // A second press on plus is a second element, so that one is left alone.
    if (!editorAt(e->scenePos(), editorReach(e->widget())).isEmpty()) {
        e->accept();
        return;
    }
    if (listButtonAt(e->scenePos(), editorReach(e->widget())) != 0) {
        e->accept();
        mousePressEvent(e);
        return;
    }

    if (auto *s = qobject_cast<NodeScene *>(scene())) s->emitNodeDoubleClicked(m_nodeId);
    QGraphicsObject::mouseDoubleClickEvent(e);
}

// Everything the catalogue holds and the node has no room for: the whole
// signature with every parameter and its default, where the declaration lives
// in P:\scripts, what it does at length, what it will do to you, and which
// findings the badge in the corner is standing for.
//
// The canvas is glanced at and this is asked for, which is the whole reason it
// can be this long. Plain text, not rich: a stylesheet here would be a second
// theme nobody maintains, and a tooltip is read once and dismissed.
QString NodeItem::buildTooltip() const
{
    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *node = g ? g->node(m_nodeId) : nullptr;
    if (!node || !m_doc) return QString();

    QStringList out;
    const Catalog &cat = m_doc->catalog();

    MethodSig sig = cat.method(node->ref);
    const bool isMethod = sig.valid;
    if (!sig.valid) sig = cat.globalFn(node->ref);
    if (sig.valid) {
        QStringList args;
        for (const MethodSig::Param &p : sig.params) {
            const QString dir = p.dir == 1 ? QStringLiteral("out ")
                                : p.dir == 2 ? QStringLiteral("inout ")
                                             : QString();
            QString one = QStringLiteral("%1%2 %3").arg(dir, p.type, p.name);
            // The default is why an optional parameter has no value field on
            // the node: leave it unwired and the call is written without it.
            if (!p.def.isEmpty()) one += QStringLiteral(" = ") + p.def;
            args << one;
        }
        QStringList lead;
        if (sig.flags & flag::Static) lead << QStringLiteral("static");
        if (sig.flags & flag::Protected) lead << QStringLiteral("protected");
        if (sig.flags & flag::Native) lead << QStringLiteral("proto native");
        if (sig.flags & flag::Override) lead << QStringLiteral("override");
        const QString name = isMethod && !sig.owner.isEmpty()
                                 ? sig.owner + QStringLiteral("::") + sig.name
                                 : sig.name;
        out << (lead.isEmpty() ? QString() : lead.join(QLatin1Char(' ')) + QLatin1Char(' '))
                   + QStringLiteral("%1 %2(%3)")
                         .arg(sig.ret, name, args.join(QStringLiteral(", ")));
    } else {
        out << (m_def.valid ? m_def.title : node->ref);
        if (!m_def.subtitle.isEmpty()) out.last() += QStringLiteral("  ") + m_def.subtitle;
    }

    const NodeHelp help = cat.explain(node->ref);
    const QString summary = help.valid && !help.summary.isEmpty() ? help.summary : m_def.doc;
    if (!summary.isEmpty()) {
        QString prose = summary;
        prose.remove(QLatin1Char('`'));
        out << QString() << prose;
    }
    if (help.valid && !help.cautions.isEmpty()) {
        out << QString();
        for (const QString &c : help.cautions) {
            QString one = c;
            one.remove(QLatin1Char('`'));
            out << QStringLiteral("Caution: ") + one;
        }
    }

    const QString where = help.valid && !help.source.isEmpty() ? help.source : m_def.loc;
    if (!where.isEmpty()) out << QString() << where;

    if (!m_diags.isEmpty()) {
        out << QString();
        for (const Diagnostic &d : m_diags) {
            QString one = d.message;
            one.remove(QLatin1Char('`'));
            if (!d.hint.isEmpty()) {
                QString hint = d.hint;
                hint.remove(QLatin1Char('`'));
                one += QLatin1Char(' ') + hint;
            }
            out << one;
        }
    }
    return out.join(QLatin1Char('\n')).trimmed();
}

void NodeItem::hoverEnterEvent(QGraphicsSceneHoverEvent *e)
{
    // Built here rather than in refresh: explain() walks the whole search index
    // for the guard a declaration sits behind, and doing that for every node of
    // a 496-node project at load would buy a string nobody has asked to read.
    if (!m_tipReady) {
        m_tipReady = true;
        setToolTip(buildTooltip());
    }
    QGraphicsObject::hoverEnterEvent(e);
}

void NodeItem::hoverMoveEvent(QGraphicsSceneHoverEvent *e)
{
    const double reach = editorReach(e->widget());
    const int button = listButtonAt(e->scenePos(), reach);
    if (button != m_hoverListButton) {
        m_hoverListButton = button;
        update();
    }
    setHoverEditor(button == 0 ? editorAt(e->scenePos(), reach) : QString());
    QGraphicsObject::hoverMoveEvent(e);
}

void NodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *e)
{
    setHoverEditor(QString());
    if (m_hoverListButton != 0) {
        m_hoverListButton = 0;
        update();
    }
    QGraphicsObject::hoverLeaveEvent(e);
}
