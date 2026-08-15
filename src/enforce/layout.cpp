// Places nodes so a converted body reads left to right.
//
// Two axes, decided separately.
//
// Columns come from the exec chain: a node sits one column right of whatever
// ran before it, with room reserved in between for the data feeding it. A node
// with two exec predecessors lands after the later of them, which is what puts
// the join of a Branch past the longer of its two arms.
//
// Rows come from the branches: the first exec output of a node carries on along
// the same row, every other one starts a row below. Data feeders stack down and
// to the left of whatever consumes them, nearest pin first, recursively.
//
// Node sizes are worked out here rather than read off the canvas, which has not
// been built yet when a conversion runs. Builtins and variable nodes come out
// exact, a code node is measured with the same font the canvas draws it in, and
// a catalogue call is the one guess: counting its parameter rows would need the
// catalogue, and this runs without one. Every guess rounds up, so an error can
// only ever leave too much room.
#include "layout.h"

#include "builtins.h"
#include "theme.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QRectF>
#include <QSizeF>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using theme::node::headerHeight;
using theme::node::padding;
using theme::node::pinRow;

constexpr double kNodeWidth = theme::node::width;

// Code-node metrics, mirroring canvas/nodeitem.cpp.
constexpr double kCodeMaxWidth = 420.0;
constexpr int kCodeMaxLines = 12;
constexpr double kCodeVPad = 4.0;
constexpr double kCodeHeaderHeight = 14.0;
constexpr int kCodeFontSize = 8;
// Only used when there is no GUI to ask, which is a headless test rather than
// the editor. Wide enough that a real face cannot beat it at 96 dpi.
constexpr double kCodeCharWidth = 7.0;
constexpr double kCodeLineHeight = 15.0;

// Sticky note defaults, from canvas/noteitem.cpp.
constexpr double kNoteWidth = 220.0;
constexpr double kNoteHeight = 120.0;
constexpr double kNoteMinWidth = 90.0;
constexpr double kNoteMinHeight = 50.0;

// Counting a catalogue call's parameter rows needs the catalogue, and this runs
// without one. Three rows covers a target, an argument and a return; the slack
// on top absorbs the calls that carry one or two more than that.
constexpr int kUnknownDataRows = 3;
constexpr double kUnknownSlack = pinRow * 2.0;

// How far below a node its feeders hang. Exec wires are drawn behind the nodes
// and run level with the exec pins, so a pure node left sitting on that line
// reads as a step in a flow it is not part of. One pin row under the exec row
// clears it.
constexpr double kFeederDrop = headerHeight + padding + pinRow;

const QString kExec = QStringLiteral("exec");
const QString kRawExpr = QStringLiteral("bi.rawExpr");

// Built once. A few dozen QString allocations buys exact pin counts for every
// node that is not a catalogue call.
const Builtins &builtinTable()
{
    static const Builtins table;
    return table;
}

bool isNote(const GraphNode &n)
{
    return n.kind == NodeKind::Comment || n.ref == bi::Comment;
}

bool isVarNode(const GraphNode &n)
{
    return n.kind == NodeKind::VarGet || n.kind == NodeKind::VarSet
           || n.ref.startsWith(QLatin1String("var.get."))
           || n.ref.startsWith(QLatin1String("var.set."));
}

bool isVarSetter(const GraphNode &n)
{
    if (n.kind == NodeKind::VarSet) return true;
    if (n.kind == NodeKind::VarGet) return false;
    return n.ref.startsWith(QLatin1String("var.set."));
}

// Which pins a node carries, in the order the canvas draws them.
struct PinShape {
    QStringList execIn;
    QStringList execOut;  // def order: true before false, body before done
    QStringList dataIn;
    QStringList dataOut;
    bool known = false;   // false when only the catalogue could fill in the rest
};

PinShape shapeOf(const GraphNode &n)
{
    PinShape s;

    if (isVarNode(n)) {
        s.known = true;
        if (isVarSetter(n)) {
            s.execIn << kExec;
            s.execOut << kExec;
            s.dataIn << QStringLiteral("v");
            s.dataOut << QStringLiteral("ret");
        } else {
            s.dataOut << QStringLiteral("ret");
        }
        return s;
    }

    const NodeDef def = builtinTable().def(n.ref);
    if (def.valid) {
        s.known = true;
        for (const Pin &p : def.pins) {
            const bool exec = p.type.kind == PinKind::Exec;
            if (p.dir == PinDir::In) (exec ? s.execIn : s.dataIn) << p.id;
            else (exec ? s.execOut : s.dataOut) << p.id;
        }
        return s;
    }

    // A catalogue call. Catalog::build spells its exec pins "exec" on both
    // sides and nothing else, so the flow is still traceable exactly; only the
    // parameter rows are a guess.
    s.execIn << kExec;
    s.execOut << kExec;
    return s;
}

// Raw text lives under one of two keys depending on which build wrote the file.
QString codeOf(const GraphNode &n)
{
    for (const char *key : {"code", "text"}) {
        const QString v = n.opts.value(QString::fromLatin1(key));
        if (!v.isEmpty()) return v;
    }
    return QString();
}

// NodeItem::displayLines, up to the point where it would start drawing.
QStringList displayLines(const QString &code)
{
    QStringList rows = code.split(QLatin1Char('\n'));
    for (QString &l : rows) {
        while (l.endsWith(QLatin1Char('\r'))) l.chop(1);
        l.replace(QLatin1Char('\t'), QStringLiteral("    "));
    }
    while (!rows.isEmpty() && rows.first().trimmed().isEmpty()) rows.removeFirst();
    while (!rows.isEmpty() && rows.last().trimmed().isEmpty()) rows.removeLast();

    int common = std::numeric_limits<int>::max();
    for (const QString &l : rows) {
        if (l.trimmed().isEmpty()) continue;
        int i = 0;
        while (i < l.size() && l.at(i) == QLatin1Char(' ')) i++;
        common = qMin(common, i);
    }
    if (common > 0 && common != std::numeric_limits<int>::max())
        for (QString &l : rows) l = l.mid(qMin(common, l.size()));
    return rows;
}

// Measured with the canvas's own font, because a monospace advance at 8pt is
// not a constant: it moves with the installed faces and the font DPI, and a
// guess that was a third short left raw nodes overlapping their neighbour.
// Headless callers have no font database, so they get the constants instead.
void measureCode(const QStringList &shown, int hidden, double *widest,
                 double *lineHeight)
{
    // The footer NodeItem draws under a block it had to cut short. On a node
    // whose visible code is one short line it is the widest thing on it.
    const QString footer =
        hidden <= 0 ? QString()
                    : hidden == 1 ? QStringLiteral("+1 more line")
                                  : QStringLiteral("+%1 more lines").arg(hidden);

    // QGuiApplication::instance() is QCoreApplication's, so it answers yes in a
    // console test and the first QFontMetrics call after that aborts the
    // process. Only a real GUI application has a font database.
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        const QFontMetricsF cm(theme::monoFont(kCodeFontSize));
        *lineHeight = std::ceil(cm.height());
        *widest = 0.0;
        for (const QString &l : shown) *widest = qMax(*widest, cm.horizontalAdvance(l));
        if (!footer.isEmpty())
            *widest = qMax(*widest, QFontMetricsF(theme::uiFont(7))
                                        .horizontalAdvance(footer));
        return;
    }

    *lineHeight = kCodeLineHeight;
    *widest = footer.size() * kCodeCharWidth;
    for (const QString &l : shown) *widest = qMax(*widest, l.size() * kCodeCharWidth);
}

QSizeF sizeOf(const GraphNode &n, const PinShape &shape)
{
    if (isNote(n)) {
        const double w = n.opts.value(QStringLiteral("w")).toDouble();
        const double h = n.opts.value(QStringLiteral("h")).toDouble();
        return QSizeF(w > 0 ? qMax(w, kNoteMinWidth) : kNoteWidth,
                      h > 0 ? qMax(h, kNoteMinHeight) : kNoteHeight);
    }

    const int execRows = qMax(shape.execIn.size(), shape.execOut.size());
    int dataRows = qMax(shape.dataIn.size(), shape.dataOut.size());
    double slack = 0.0;
    if (!shape.known) {
        dataRows = qMax(dataRows, kUnknownDataRows);
        slack = kUnknownSlack;
    }
    const int rows = execRows + dataRows;

    if (n.ref == bi::Raw || n.ref == kRawExpr) {
        const QStringList all = displayLines(codeOf(n));
        if (!all.isEmpty()) {
            const QStringList shown = all.mid(0, kCodeMaxLines);
            const int hidden = all.size() - shown.size();
            double widest = 0.0;
            double lineHeight = kCodeLineHeight;
            measureCode(shown, hidden, &widest, &lineHeight);

            const double w = qBound(kNodeWidth, std::ceil(widest) + padding * 2.0,
                                    kCodeMaxWidth);
            // One pin row at most, so the pins ride on the header itself and the
            // code starts directly under it.
            const double top = rows <= 1
                                   ? kCodeHeaderHeight
                                   : headerHeight + padding + rows * pinRow;
            const double h = top + kCodeVPad * 2.0
                             + (shown.size() + (hidden > 0 ? 1 : 0)) * lineHeight;
            return QSizeF(w, h);
        }
    }

    const double h = qMax(headerHeight + padding * 2.0,
                          headerHeight + padding + rows * pinRow + padding);
    return QSizeF(kNodeWidth, h + slack);
}

// One end of a wire, from the point of view of the node holding the list.
struct Link {
    int node = 0;   // the far end
    int rank = 0;   // pin order on this node's side
    int seq = 0;    // edge index, so equal ranks still order the same way
};

struct Item {
    int gi = 0;         // index into Graph::nodes
    PinShape shape;
    QSizeF size;
    bool exec = false;  // carries at least one exec wire
    int col = 0;
    int lane = -1;
    int cluster = -1;   // connected group; each gets its own column widths
    int owner = -1;     // the consumer that placed this feeder
    double x = 0.0;
    double y = 0.0;
};

// Where a wire lands in a node's input column. Catalogue calls are not in the
// builtin table, but their inputs are always `target` then p0, p1 ... which is
// the order the canvas draws them in.
int dataRank(const PinShape &shape, const QString &pin)
{
    const int idx = shape.dataIn.indexOf(pin);
    if (idx >= 0) return idx;
    if (pin == QLatin1String("target")) return 0;
    if (pin.size() > 1 && pin.startsWith(QLatin1Char('p'))) {
        bool ok = false;
        const int n = pin.mid(1).toInt(&ok);
        if (ok && n >= 0) return 1 + n;
    }
    return 1000;
}

class Placer {
public:
    Placer(Graph &graph, const QSet<QString> &ids, const LayoutOptions &opts)
        : m_g(graph), m_ids(ids), m_opt(opts) {}

    void run();

private:
    Graph &m_g;
    const QSet<QString> &m_ids;
    const LayoutOptions &m_opt;

    QVector<Item> m_items;
    QHash<QString, int> m_byId;
    QVector<QVector<Link>> m_execOut;
    QVector<QVector<Link>> m_execIn;
    QVector<QVector<Link>> m_feeds;       // data sources, in input-pin order
    QVector<QVector<int>> m_owned;        // feeders this node placed, same order
    QVector<int> m_depth;                 // rows of feeder tree hanging off a node
    QVector<double> m_stack;              // height of a node plus the feeders it owns
    QVector<int> m_anchors;               // lane owners, in placement order
    QVector<QVector<int>> m_clusters;     // members of each cluster, in item order
    int m_maxLane = -1;

    const GraphNode &nodeAt(int i) const { return m_g.nodes.at(m_items.at(i).gi); }
    // A row break has to read as a break, so it is wider than the gap between
    // two feeders stacked inside one row.
    double laneGap() const { return m_opt.rowGap * 2.0; }

    void collect();
    void linkEdges();
    void computeFeederDepths();
    QVector<int> rootOrder() const;
    void assignColumns();
    void assignLanes();
    void claimFeeders();
    void relaxFeederColumns();
    double stackHeight(int i);
    void placeStack(int i, double top);
    void assignRows();
    void resolveColumns();
    void stackClusters();
    void writeBack();
};

void Placer::collect()
{
    for (int i = 0; i < m_g.nodes.size(); ++i) {
        const GraphNode &n = m_g.nodes.at(i);
        if (!m_ids.contains(n.id)) continue;
        Item it;
        it.gi = i;
        it.shape = shapeOf(n);
        it.size = sizeOf(n, it.shape);
        m_byId.insert(n.id, m_items.size());
        m_items.append(it);
    }
    const int n = m_items.size();
    m_execOut.resize(n);
    m_execIn.resize(n);
    m_feeds.resize(n);
    m_owned.resize(n);
    m_depth.fill(0, n);
    m_stack.fill(-1.0, n);
}

void Placer::linkEdges()
{
    for (int e = 0; e < m_g.edges.size(); ++e) {
        const GraphEdge &ed = m_g.edges.at(e);
        const int f = m_byId.value(ed.from.node, -1);
        const int t = m_byId.value(ed.to.node, -1);
        // A wire leaving the set is not structure this pass can honour: the far
        // end is staying where it is.
        if (f < 0 || t < 0 || f == t) continue;

        const int execRank = m_items.at(f).shape.execOut.indexOf(ed.from.pin);
        if (execRank >= 0) {
            m_execOut[f].append({t, execRank, e});
            m_execIn[t].append({f, 0, e});
            m_items[f].exec = true;
            m_items[t].exec = true;
        } else {
            m_feeds[t].append({f, dataRank(m_items.at(t).shape, ed.to.pin), e});
        }
    }

    const auto byRank = [this](const Link &a, const Link &b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (m_items.at(a.node).gi != m_items.at(b.node).gi)
            return m_items.at(a.node).gi < m_items.at(b.node).gi;
        return a.seq < b.seq;
    };
    for (QVector<Link> &l : m_execOut) std::sort(l.begin(), l.end(), byRank);
    for (QVector<Link> &l : m_feeds) std::sort(l.begin(), l.end(), byRank);
}

// How many columns of pure nodes hang off a node's inputs. Relaxed rather than
// recursed so a data cycle cannot run off the stack.
void Placer::computeFeederDepths()
{
    for (int pass = 0; pass <= m_items.size(); ++pass) {
        bool changed = false;
        for (int i = 0; i < m_items.size(); ++i) {
            for (const Link &l : m_feeds.at(i)) {
                if (m_items.at(l.node).exec) continue;
                const int want = m_depth.at(l.node) + 1;
                if (m_depth.at(i) >= want) continue;
                m_depth[i] = want;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

// Entry nodes first, so a converted body starts at the top left rather than
// wherever the file happened to list its nodes.
QVector<int> Placer::rootOrder() const
{
    QVector<int> roots;
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).exec && m_execIn.at(i).isEmpty()) roots.append(i);

    const auto rank = [this](int i) {
        const GraphNode &n = nodeAt(i);
        if (n.kind == NodeKind::Event) return 0;
        if (n.ref == bi::Begin || n.ref == bi::End) return 0;
        const PinShape &s = m_items.at(i).shape;
        if (s.known && s.execIn.isEmpty() && !s.execOut.isEmpty()) return 0;
        return 1;
    };
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        const int ra = rank(a);
        const int rb = rank(b);
        if (ra != rb) return ra < rb;
        return m_items.at(a).gi < m_items.at(b).gi;
    });
    return roots;
}

void Placer::assignColumns()
{
    for (int i = 0; i < m_items.size(); ++i) m_items[i].col = m_depth.at(i);

    // Depth-first order with back edges dropped, so a chain that loops on
    // itself still gets one pass rather than spiralling right forever. The
    // cursor is kept per node rather than per stack frame: a node is pushed at
    // most once, and nothing may hold a reference into the stack across a push.
    QVector<char> state(m_items.size(), 0);
    QVector<int> cursor(m_items.size(), 0);
    QVector<int> post;
    QVector<int> stack;

    const auto walk = [&](int start) {
        if (state.at(start) != 0) return;
        state[start] = 1;
        stack.append(start);
        while (!stack.isEmpty()) {
            const int cur = stack.last();
            if (cursor.at(cur) < m_execOut.at(cur).size()) {
                const int next = m_execOut.at(cur).at(cursor.at(cur)).node;
                cursor[cur] = cursor.at(cur) + 1;
                if (state.at(next) == 0) {
                    state[next] = 1;
                    stack.append(next);
                }
                continue;
            }
            state[cur] = 2;
            post.append(cur);
            stack.removeLast();
        }
    };

    const QVector<int> roots = rootOrder();
    for (int r : roots) walk(r);
    // Anything left is inside an exec cycle, which still has to be placed.
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).exec) walk(i);

    QVector<int> topo(m_items.size(), -1);
    for (int i = 0; i < post.size(); ++i) topo[post.at(post.size() - 1 - i)] = i;

    QVector<int> order = post;
    std::reverse(order.begin(), order.end());
    for (int i : order) {
        for (const Link &l : m_execOut.at(i)) {
            if (topo.at(l.node) <= topo.at(i)) continue; // back edge
            const int want = m_items.at(i).col + 1 + m_depth.at(l.node);
            if (m_items.at(l.node).col < want) m_items[l.node].col = want;
        }
    }
}

// The first exec output carries the row; every other one starts a row below,
// allocated only once the earlier branch has finished claiming rows.
void Placer::assignLanes()
{
    // Per node, not per stack frame: nothing may hold a reference into the
    // stack across a push, and a node is only ever pushed once anyway.
    QVector<int> cursor(m_items.size(), 0);
    QVector<char> usedFirst(m_items.size(), 0);
    QVector<int> stack;

    const auto walk = [&](int start) {
        if (m_items.at(start).lane >= 0) return;
        const int cluster = m_clusters.size();
        m_clusters.append(QVector<int>());
        const auto take = [&](int i, int lane) {
            m_items[i].lane = lane;
            m_items[i].cluster = cluster;
            m_clusters[cluster].append(i);
            m_anchors.append(i);
        };

        take(start, ++m_maxLane);
        stack.append(start);
        while (!stack.isEmpty()) {
            const int cur = stack.last();
            if (cursor.at(cur) >= m_execOut.at(cur).size()) {
                stack.removeLast();
                continue;
            }
            const int next = m_execOut.at(cur).at(cursor.at(cur)).node;
            cursor[cur] = cursor.at(cur) + 1;
            // Already placed: this is where two arms of a branch come back
            // together, and it keeps the row the first arm gave it.
            if (m_items.at(next).lane >= 0) continue;

            int lane = 0;
            if (!usedFirst.at(cur)) {
                lane = m_items.at(cur).lane;
                usedFirst[cur] = 1;
            } else {
                lane = ++m_maxLane;
            }
            take(next, lane);
            stack.append(next);
        }
    };

    const QVector<int> roots = rootOrder();
    for (int r : roots) walk(r);
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).exec) walk(i);
}

// Every pure node is placed by exactly one consumer, so a value used twice does
// not get two positions and is not counted twice when rows are measured.
void Placer::claimFeeders()
{
    QVector<int> work;
    const auto claimFrom = [&](int consumer) {
        for (const Link &l : m_feeds.at(consumer)) {
            const int f = l.node;
            if (m_items.at(f).exec) continue;
            if (m_items.at(f).lane >= 0 || m_items.at(f).owner >= 0) continue;
            m_items[f].owner = consumer;
            m_items[f].cluster = m_items.at(consumer).cluster;
            m_clusters[m_items.at(f).cluster].append(f);
            m_owned[consumer].append(f);
            work.append(f);
        }
    };

    for (int i = 0; i < m_anchors.size(); ++i) claimFrom(m_anchors.at(i));
    for (int i = 0; i < work.size(); ++i) claimFrom(work.at(i));

    // What is left feeds nothing inside the set. Disconnected clusters stack
    // below the flow rather than landing on top of it.
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).exec || m_items.at(i).lane >= 0 || m_items.at(i).owner >= 0)
            continue;
        m_items[i].lane = ++m_maxLane;
        m_items[i].cluster = m_clusters.size();
        m_clusters.append(QVector<int>{i});
        m_anchors.append(i);
        const int first = work.size();
        claimFrom(i);
        for (int k = first; k < work.size(); ++k) claimFrom(work.at(k));
    }
}

// A feeder sits left of everything it feeds, so it takes the leftmost column
// any of its consumers allows.
void Placer::relaxFeederColumns()
{
    QVector<int> col(m_items.size(), std::numeric_limits<int>::max());
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).exec || m_items.at(i).lane >= 0) col[i] = m_items.at(i).col;

    for (int pass = 0; pass <= m_items.size(); ++pass) {
        bool changed = false;
        for (int i = 0; i < m_items.size(); ++i) {
            if (col.at(i) == std::numeric_limits<int>::max()) continue;
            for (const Link &l : m_feeds.at(i)) {
                const int f = l.node;
                if (m_items.at(f).exec || m_items.at(f).lane >= 0) continue;
                const int want = col.at(i) - 1;
                if (col.at(f) <= want) continue;
                col[f] = want;
                changed = true;
            }
        }
        if (!changed) break;
    }

    for (int i = 0; i < m_items.size(); ++i)
        if (col.at(i) != std::numeric_limits<int>::max()) m_items[i].col = col.at(i);
}

double Placer::stackHeight(int i)
{
    if (m_stack.at(i) >= 0.0) return m_stack.at(i);
    m_stack[i] = m_items.at(i).size.height();
    if (m_owned.at(i).isEmpty()) return m_stack.at(i);

    double sum = kFeederDrop;
    for (int j : m_owned.at(i)) sum += stackHeight(j) + m_opt.rowGap;
    sum -= m_opt.rowGap;
    m_stack[i] = qMax(m_items.at(i).size.height(), sum);
    return m_stack.at(i);
}

void Placer::placeStack(int i, double top)
{
    m_items[i].y = top;
    double cur = top + kFeederDrop;
    for (int j : m_owned.at(i)) {
        placeStack(j, cur);
        cur += stackHeight(j) + m_opt.rowGap;
    }
}

void Placer::assignRows()
{
    QVector<double> laneHeight(m_maxLane + 1, 0.0);
    for (int a : m_anchors) {
        const int lane = m_items.at(a).lane;
        laneHeight[lane] = qMax(laneHeight.at(lane), stackHeight(a));
    }

    QVector<double> laneTop(m_maxLane + 1, 0.0);
    for (int l = 1; l <= m_maxLane; ++l)
        laneTop[l] = laneTop.at(l - 1) + laneHeight.at(l - 1) + laneGap();

    for (int a : m_anchors) placeStack(a, laneTop.at(m_items.at(a).lane));
}

// Column widths are per cluster. Sharing one width for the whole graph lines
// every cluster up on the same x, but one wide code node then pushes a gap of
// its own width into every other chain at that step, and a short function ends
// up mostly empty space.
//
// Within a column nothing may share vertical space, because the column is the
// only thing keeping those nodes apart horizontally. Sliding a node down here
// can pull it off its row; a row that reads well and a node buried under
// another is not a trade worth making.
void Placer::resolveColumns()
{
    for (QVector<int> &members : m_clusters) {
        if (members.isEmpty()) continue;

        int minCol = std::numeric_limits<int>::max();
        int maxCol = std::numeric_limits<int>::min();
        for (int i : members) {
            minCol = qMin(minCol, m_items.at(i).col);
            maxCol = qMax(maxCol, m_items.at(i).col);
        }
        const int columns = maxCol - minCol + 1;

        QVector<QVector<int>> byColumn(columns);
        QVector<double> widths(columns, kNodeWidth);
        for (int i : members) {
            const int c = m_items.at(i).col - minCol;
            m_items[i].col = c;
            byColumn[c].append(i);
            widths[c] = qMax(widths.at(c), m_items.at(i).size.width());
        }

        double x = 0.0;
        for (int c = 0; c < columns; ++c) {
            QVector<int> &list = byColumn[c];
            std::sort(list.begin(), list.end(), [this](int a, int b) {
                if (m_items.at(a).y != m_items.at(b).y)
                    return m_items.at(a).y < m_items.at(b).y;
                if (m_items.at(a).lane != m_items.at(b).lane)
                    return m_items.at(a).lane < m_items.at(b).lane;
                return m_items.at(a).gi < m_items.at(b).gi;
            });
            double cursor = -std::numeric_limits<double>::max();
            for (int i : list) {
                m_items[i].x = x;
                m_items[i].y = qMax(m_items.at(i).y, cursor);
                cursor = m_items.at(i).y + m_items.at(i).size.height() + m_opt.rowGap;
            }
            x += widths.at(c) + m_opt.columnGap;
        }
    }
}

// Clusters get their own x axis, so they can only be kept apart on y. Stacking
// them by what they actually ended up occupying is what makes that safe: rows
// were spaced from estimated heights, and resolveColumns may have pushed a node
// past the row it was meant for.
void Placer::stackClusters()
{
    double top = 0.0;
    for (const QVector<int> &members : m_clusters) {
        if (members.isEmpty()) continue;
        double lo = std::numeric_limits<double>::max();
        double hi = -std::numeric_limits<double>::max();
        for (int i : members) {
            lo = qMin(lo, m_items.at(i).y);
            hi = qMax(hi, m_items.at(i).y + m_items.at(i).size.height());
        }
        const double dy = top - lo;
        for (int i : members) m_items[i].y += dy;
        top = hi + dy + laneGap();
    }
}

void Placer::writeBack()
{
    // The result is anchored at the origin the caller asked for, then pushed
    // down as one block if that would bury it under something already on the
    // canvas. Moving it is better than moving nodes the caller did not select.
    double dy = m_opt.originY;
    QVector<QRectF> fixed;
    for (int i = 0; i < m_g.nodes.size(); ++i) {
        const GraphNode &n = m_g.nodes.at(i);
        if (m_ids.contains(n.id)) continue;
        const QSizeF s = sizeOf(n, shapeOf(n));
        fixed.append(QRectF(n.x, n.y, s.width(), s.height()));
    }
    // Each pass clears the block past at least one more fixed node's bottom
    // edge and never moves it back up, so one pass per fixed node is enough.
    const double clear = m_opt.rowGap;
    for (int pass = 0; pass <= fixed.size(); ++pass) {
        double push = 0.0;
        for (const Item &it : m_items) {
            const QRectF r(m_opt.originX + it.x, it.y + dy, it.size.width(),
                           it.size.height());
            for (const QRectF &f : fixed) {
                if (!r.adjusted(-clear, -clear, clear, clear).intersects(f)) continue;
                push = qMax(push, f.bottom() + clear - r.top());
            }
        }
        if (push <= 0.0) break;
        dy += push;
    }

    for (const Item &it : m_items) {
        GraphNode &n = m_g.nodes[it.gi];
        n.x = std::round(m_opt.originX + it.x);
        n.y = std::round(it.y + dy);
    }
}

void Placer::run()
{
    collect();
    if (m_items.isEmpty()) return;

    linkEdges();
    computeFeederDepths();
    assignColumns();
    assignLanes();
    claimFeeders();
    relaxFeederColumns();
    assignRows();
    resolveColumns();
    stackClusters();
    writeBack();
}

} // namespace

void layoutNodes(Graph &graph, const QSet<QString> &nodeIds, const LayoutOptions &opts)
{
    if (nodeIds.isEmpty()) return;
    Placer(graph, nodeIds, opts).run();
}

void layoutGraph(Graph &graph, const LayoutOptions &opts)
{
    QSet<QString> ids;
    ids.reserve(graph.nodes.size());
    for (const GraphNode &n : graph.nodes) ids.insert(n.id);
    layoutNodes(graph, ids, opts);
}
