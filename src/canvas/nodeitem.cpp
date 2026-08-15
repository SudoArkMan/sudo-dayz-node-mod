#include "nodeitem.h"

#include "catalog.h"
#include "document.h"
#include "enforce/lexer.h"
#include "nodescene.h"
#include "theme.h"

#include <QAction>
#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QInputDialog>
#include <QLineEdit>
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
        update();
        return;
    }

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

void NodeItem::setDiagnostics(const QVector<Diagnostic> &diags)
{
    m_diags = diags;
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

    const auto place = [&](const QVector<Pin> &list, int firstRow, bool left) {
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
                                  && inlineEditorFor(pl.pin.type) != InlineEditor::None;
            if (editable) {
                const double h = pinRow - 4.0;
                pl.editor = QRectF(m_width * 0.46, y - h / 2.0, m_width * 0.48, h);
                // The whole row band from a little left of the box to the node
                // edge. The box is four units shorter than its row and stops
                // six short of the edge, and every one of those units used to
                // be a press that selected the node instead of editing it.
                const double left = pl.editor.left() - 4.0;
                pl.hit = QRectF(left, y - pinRow / 2.0, m_width - left, pinRow);
            }
            m_pins.append(pl);
        }
    };

    // Exec pins own the first rows on both sides, so a flow reads top to
    // bottom no matter how many parameters a call has.
    place(execIn, 0, true);
    place(execOut, 0, false);
    place(dataIn, execRows, true);
    place(dataOut, execRows, false);
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
    const double grow = qBound(0.0, reach, pinRow);
    QString best;
    double bestDist = std::numeric_limits<double>::max();
    for (const PinLayout &pl : m_pins) {
        if (pl.hit.isEmpty()) continue;
        if (!pl.hit.adjusted(-grow, -grow, grow, grow).contains(local)) continue;
        // Grown bands overlap their neighbours, so the winner is the row whose
        // centre line the press is nearest rather than whichever comes first.
        const double dist = std::fabs(local.y() - pl.pos.y());
        if (dist >= bestDist) continue;
        bestDist = dist;
        best = pl.pin.id;
    }
    return best;
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
    // Wide enough to contain shape()'s pin caps, which Qt requires.
    return QRectF(-kPinGrab - 1.0, -4.0, m_width + kPinGrab * 2.0 + 2.0, m_height + 8.0);
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
        const QRectF tr(box.right() + 3.0, pl.editor.top(),
                        pl.editor.right() - box.right() - 5.0, pl.editor.height());
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
    const QRectF tr = pl.editor.adjusted(3.0, 0, -3.0 - chevron, 0);
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

void NodeItem::paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w)
{
    Q_UNUSED(opt);
    Q_UNUSED(w);
    p->setRenderHint(QPainter::Antialiasing, true);

    const Graph *g = m_doc ? m_doc->activeGraph() : nullptr;
    const GraphNode *node = g ? g->node(m_nodeId) : nullptr;
    const QRectF body(0, 0, m_width, m_height);

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

    const double badgeSpace = hasDiags ? kBadgeRadius * 2.0 + 2.0 : 0.0;
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

    if (hasDiags) {
        const QPointF c(m_width - padding - pinClearance - kBadgeRadius + 1.0,
                        m_headerHeight / 2.0);
        p->setPen(Qt::NoPen);
        p->setBrush(severityColor(worst));
        p->drawEllipse(c, kBadgeRadius, kBadgeRadius);
        p->setFont(theme::uiFont(6, true));
        p->setPen(theme::windowBg());
        const QString glyph = worst == Severity::Error ? QStringLiteral("x")
                              : worst == Severity::Warning ? QStringLiteral("!")
                                                           : QStringLiteral("i");
        p->drawText(QRectF(c.x() - kBadgeRadius, c.y() - kBadgeRadius,
                           kBadgeRadius * 2, kBadgeRadius * 2),
                    Qt::AlignCenter, glyph);
    }

    for (const PinLayout &pl : m_pins) {
        const bool left = pl.pin.dir == PinDir::In;
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

        if (!pl.pin.label.isEmpty()) {
            const double avail = pl.editor.isEmpty()
                                     ? m_width * 0.62
                                     : pl.editor.left() - kLabelInset - 3.0;
            if (avail > 10.0) {
                const QRectF lr = left
                    ? QRectF(kLabelInset, pl.pos.y() - pinRow / 2.0, avail, pinRow)
                    : QRectF(m_width - kLabelInset - avail, pl.pos.y() - pinRow / 2.0,
                             avail, pinRow);
                p->setFont(sf);
                p->setPen(theme::textDim());
                p->drawText(lr, (left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                            sm.elidedText(pl.pin.label, Qt::ElideRight, avail));
            }
        }

        if (pl.editor.isEmpty()) continue;
        paintValueField(p, pl, node ? node->inputs.value(pl.pin.id, pl.pin.def)
                                    : pl.pin.def);
    }

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

    const QString pinId = editorAt(e->scenePos(), editorReach(e->widget()));
    if (pinId.isEmpty()) {
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
    activateEditor(pinId, e->widget(), e->screenPos());
}

void NodeItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e)
{
    // The press already acted on the field. Letting the second click through
    // would flip a bool straight back, which reads as the click doing nothing.
    if (!editorAt(e->scenePos(), editorReach(e->widget())).isEmpty()) {
        e->accept();
        return;
    }

    if (auto *s = qobject_cast<NodeScene *>(scene())) s->emitNodeDoubleClicked(m_nodeId);
    QGraphicsObject::mouseDoubleClickEvent(e);
}

void NodeItem::hoverMoveEvent(QGraphicsSceneHoverEvent *e)
{
    setHoverEditor(editorAt(e->scenePos(), editorReach(e->widget())));
    QGraphicsObject::hoverMoveEvent(e);
}

void NodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *e)
{
    setHoverEditor(QString());
    QGraphicsObject::hoverLeaveEvent(e);
}
