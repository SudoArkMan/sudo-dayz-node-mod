#include "lower.h"

#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "layout.h"
#include "lexer.h"
#include "project.h"
#include "scriptapi.h"

#include <QFile>
#include <QHash>
#include <QMutex>
#include <QSet>

// Ids the builtins header does not name but this file has to place. They are
// the exact strings builtins.cpp registers; a typo here produces a node that
// resolves to nothing rather than a compile error.
namespace {

const QString IdSelf       = QStringLiteral("bi.self");
const QString IdOp         = QStringLiteral("bi.op");
const QString IdNot        = QStringLiteral("bi.not");
const QString IdSelect     = QStringLiteral("bi.select");
const QString IdNew        = QStringLiteral("bi.new");
const QString IdRawExpr    = QStringLiteral("bi.rawExpr");
const QString IdLitClass   = QStringLiteral("bi.litClass");
const QString IdServerOnly = QStringLiteral("bi.serverOnly");
const QString IdSetElement = QStringLiteral("bi.setElement");
const QString IdSetMember  = QStringLiteral("bi.setMember");

const QString PinExec   = QStringLiteral("exec");
const QString PinRet    = QStringLiteral("ret");
const QString PinValue  = QStringLiteral("v");
const QString PinTarget = QStringLiteral("target");

// A value on its way into an input pin: either an output pin to wire from, or
// a literal to type into the pin. The literal is held in the form an input
// takes (a string without its quotes), because that is what the inspector
// shows and what the generator quotes again on the way out.
struct Val {
    QString node;   // producing node, empty for a literal
    QString pin;
    QString text;   // literal text, when node is empty
    QString type;   // Enforce type, empty when it could not be worked out
    bool valid = false;

    bool isLiteral() const { return node.isEmpty(); }
};

// One name in scope. A local that is written once is `value`, wired straight
// from whatever produced it; one that is written more than once had to become a
// real variable and is reached through `varId`.
struct Local {
    QString type;
    Val value;
    QString varId;
    bool bound = false;
};

// What the first pass learned about a name, which is what decides whether it
// survives as a wire or has to become a variable.
struct Facts {
    bool declared = false;
    int declDepth = 0;
    int writes = 0;
    int reads = 0;
    int deepestWrite = -1;
    QString type;
};

// A lowered statement or block: where its exec chain starts, and the pin a
// following statement chains from. An empty `tailPin` means the chain cannot
// continue there, which is true of Branch, Cast To and Return.
struct Chain {
    QString entry;
    QString tailNode;
    QString tailPin;
    bool ok = false;
    // What the author left between the last statement of this block and the
    // brace that closes it. Whoever writes that brace has to write these first.
    QString endTrivia;
};

// The author's own text between two points in a body: what trails the line the
// previous statement ended on, and the whole lines standing above the next one.
struct Gap {
    QString trailing;
    QStringList before;
    QString indent; // what the statements of this block sit behind
};

// The lines of a gap, with the block's own indentation taken off the front of
// each comment so the generator can put it back at whatever depth the node ends
// up at. A blank line keeps whatever it held: there is no indentation to speak
// of on a line with nothing on it, and adding some leaves trailing spaces in
// the file. A comment standing at a different column from the code around it
// cannot be expressed this way, and the whole gap is turned down instead.
bool dedentGap(const QStringList &lines, const QString &indent, QStringList *out)
{
    if (!nodefmt::isCommentaryOnly(lines.join(QLatin1Char('\n')))) return false;
    for (const QString &l : lines) {
        if (nodefmt::isBlankLine(l)) {
            out->append(l);
            continue;
        }
        if (!l.startsWith(indent)) return false;
        out->append(l.mid(indent.size()));
    }
    return true;
}

// `from` is one past the previous statement, or the inside edge of the brace
// that opens the block. `atLineStart` says the first character of the range is
// already the start of a line, which is true of a method body and of nothing
// else. False means nothing here can be kept, and the body is then refused
// rather than regenerated without it.
bool splitGap(const QString &src, int from, int to, bool atLineStart, Gap *out)
{
    if (from < 0 || to < from || to > src.size()) return false;
    QString text = src.mid(from, to - from);
    if (!atLineStart) {
        const int nl = text.indexOf(QLatin1Char('\n'));
        // The two statements share a line. The generator writes one statement
        // to a line, so no field here could bring that back.
        if (nl < 0) return false;
        out->trailing = text.left(nl);
        text = text.mid(nl + 1);
    }
    QStringList lines = text.split(QLatin1Char('\n'));
    // The last piece is the indentation in front of the statement itself, which
    // the generator writes for the node rather than keeping here.
    out->indent = lines.takeLast();
    if (!nodefmt::isIndentText(out->indent)) return false;
    if (!nodefmt::isCommentaryOnly(out->trailing)) return false;
    return dedentGap(lines, out->indent, &out->before);
}

// The same, for the run between the last statement of a block and its end.
// `dropCloseIndent` is for a block whose closing brace is still in the text:
// the indent in front of that brace belongs to the generator.
bool splitEndGap(const QString &src, int from, int to, bool dropCloseIndent,
                 const QString &indent, Gap *out)
{
    if (from < 0 || to < from || to > src.size()) return false;
    const QString text = src.mid(from, to - from);
    const int nl = text.indexOf(QLatin1Char('\n'));
    if (nl < 0) {
        out->trailing = text;
        return nodefmt::isCommentaryOnly(text);
    }
    out->trailing = text.left(nl);
    if (!nodefmt::isCommentaryOnly(out->trailing)) return false;
    QStringList lines = text.mid(nl + 1).split(QLatin1Char('\n'));
    if (dropCloseIndent) {
        const QString closeIndent = lines.takeLast();
        if (!nodefmt::isIndentText(closeIndent)) return false;
    }
    return dedentGap(lines, indent, &out->before);
}

// Statements the generator always writes out as a header and a braced block,
// however few lines the author wrote them on. The first line it produces for
// one of these is the header, so a comment written under the whole construct
// must not be hung on it: coming back on top, the comment would be describing
// different code from the one it was written about.
bool writesBlock(StmtKind k)
{
    return k == StmtKind::If || k == StmtKind::For || k == StmtKind::ForEach
           || k == StmtKind::While || k == StmtKind::Switch || k == StmtKind::Block;
}

// The whitespace in front of the line `at` sits on, empty when the character
// before it is not whitespace.
QString indentAt(const QString &src, int at)
{
    if (at < 0 || at > src.size()) return {};
    int from = at > 0 ? src.lastIndexOf(QLatin1Char('\n'), at - 1) : -1;
    from = from < 0 ? 0 : from + 1;
    const QString lead = src.mid(from, at - from);
    return nodefmt::isIndentText(lead) ? lead : QString();
}

// The parser keeps `i++` and `++i` apart by spelling the postfix form
// "post++". Either way the statement means the same thing.
bool isIncDec(const QString &op)
{
    return op == QLatin1String("++") || op == QLatin1String("--")
           || op == QLatin1String("post++") || op == QLatin1String("post--");
}

bool isIncrement(const QString &op)
{
    return op.endsWith(QLatin1String("++"));
}

// An `out x` argument arrives wrapped, and what matters underneath is the name.
const Expr *unwrapDirection(const Expr *e)
{
    while (e && e->kind == ExprKind::Unary
           && (e->op == QLatin1String("out") || e->op == QLatin1String("inout")))
        e = e->target.get();
    return e;
}

QString opWithoutAssign(const QString &op)
{
    return op.endsWith(QLatin1Char('=')) ? op.left(op.size() - 1) : QString();
}

// The counted form, `for (int i = a; i < b; i++)`, which the For Loop node can
// hold; empty for any other shape. The first pass and the lowering both read
// this, and they have to agree: the step of a counted loop is the loop node's
// own counter, so counting it as a second write turns the counter into a class
// variable and the body then reads that instead of the index pin.
QString countedForCounter(const Stmt &s)
{
    const Stmt *init = s.forInit.get();
    const Expr *cond = s.forCond.get();
    const Expr *step = s.forStep.get();
    if (!init || !cond || !step) return {};
    if (init->kind != StmtKind::VarDecl || init->decls.size() != 1) return {};
    const QString var = init->decls.front().name;
    if (var.isEmpty() || !init->decls.front().init) return {};
    if (cond->kind != ExprKind::Binary || cond->op != QLatin1String("<")) return {};
    if (!cond->target || cond->target->kind != ExprKind::Name
        || cond->target->text != var || !cond->second)
        return {};
    if (step->kind != ExprKind::Unary || !isIncrement(step->op) || !step->target
        || step->target->kind != ExprKind::Name || step->target->text != var)
        return {};
    return var;
}

bool isClassIdent(const QString &s)
{
    if (s.isEmpty()) return false;
    for (const QChar c : s)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) return false;
    return true;
}

// Enforce type without the storage modifiers, for catalogue lookups. Same set
// the generator strips, so the two agree on what "array<ref ItemBase>" is.
QString bareType(const QString &t)
{
    QString out = t;
    static const QStringList mods = {
        QStringLiteral("ref "), QStringLiteral("autoptr "), QStringLiteral("notnull "),
        QStringLiteral("const "), QStringLiteral("owned "), QStringLiteral("local "),
        QStringLiteral("out "), QStringLiteral("inout "),
    };
    bool changed = true;
    while (changed) {
        changed = false;
        out = out.trimmed();
        for (const QString &m : mods) {
            if (!out.startsWith(m)) continue;
            out = out.mid(m.size());
            changed = true;
        }
    }
    return out.trimmed();
}

// The class the catalogue declares for a type as written. `array<ref Item>` is
// declared as the template class `array`, and TStringArray is the same thing
// under another name, so neither resolves without this.
QString catalogClass(const QString &type)
{
    QString t = bareType(type);
    if (t.endsWith(QLatin1String("[]"))) return QStringLiteral("array");
    static const QStringList arrays = {
        QStringLiteral("TStringArray"), QStringLiteral("TIntArray"),
        QStringLiteral("TFloatArray"),  QStringLiteral("TVectorArray"),
        QStringLiteral("TClassArray"),  QStringLiteral("TTypenameArray"),
        QStringLiteral("TBoolArray"),
    };
    if (arrays.contains(t)) return QStringLiteral("array");
    const int angle = t.indexOf(QLatin1Char('<'));
    if (angle > 0) return t.left(angle).trimmed();
    return t;
}

// Types where an assignment converts the value rather than aliasing it.
bool isPrimitiveType(const QString &t)
{
    static const QStringList prims = {
        QStringLiteral("int"), QStringLiteral("float"), QStringLiteral("bool"),
        QStringLiteral("string"), QStringLiteral("vector"),
    };
    return prims.contains(t);
}

QString literalTypeName(const QString &enforceType)
{
    const QString t = bareType(enforceType);
    if (Builtins::literalTypes().contains(t)) return t;
    return QStringLiteral("string");
}

// ------------------------------------------------------ measuring the refusals
//
// Instrumentation for the question "why does so little of a real mod lower".
// One tab separated row per statement turned down, one per body handed in, into
// the file named by SUDO_LOWER_DIAG. Nothing here runs when that is unset, and
// nothing here decides anything: the rows are written after the lowering has
// already made its choice.
namespace diag {

struct Sink {
    QFile file;
    QMutex lock;
    bool tried = false;
    bool on = false;
    qint64 run = 0;
};

Sink &sink()
{
    static Sink s;
    return s;
}

bool enabled()
{
    Sink &s = sink();
    QMutexLocker guard(&s.lock);
    if (!s.tried) {
        s.tried = true;
        const QByteArray path = qgetenv("SUDO_LOWER_DIAG");
        if (!path.isEmpty()) {
            s.file.setFileName(QString::fromLocal8Bit(path));
            s.on = s.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        }
    }
    return s.on;
}

void write(const QString &row)
{
    Sink &s = sink();
    QMutexLocker guard(&s.lock);
    if (!s.on) return;
    s.file.write(row.toUtf8());
    s.file.write("\n");
}

qint64 nextRun()
{
    Sink &s = sink();
    QMutexLocker guard(&s.lock);
    return ++s.run;
}

QString flat(const QString &text, int limit = 150)
{
    QString one = text;
    one.replace(QLatin1Char('\t'), QLatin1Char(' '));
    one = one.simplified();
    if (one.size() > limit) one = one.left(limit) + QStringLiteral(" ...");
    return one;
}

QString kindName(StmtKind k)
{
    switch (k) {
    case StmtKind::Expression: return QStringLiteral("Expression");
    case StmtKind::VarDecl:    return QStringLiteral("VarDecl");
    case StmtKind::If:         return QStringLiteral("If");
    case StmtKind::For:        return QStringLiteral("For");
    case StmtKind::ForEach:    return QStringLiteral("ForEach");
    case StmtKind::While:      return QStringLiteral("While");
    case StmtKind::Return:     return QStringLiteral("Return");
    case StmtKind::Break:      return QStringLiteral("Break");
    case StmtKind::Continue:   return QStringLiteral("Continue");
    case StmtKind::Block:      return QStringLiteral("Block");
    case StmtKind::Switch:     return QStringLiteral("Switch");
    case StmtKind::Delete:     return QStringLiteral("Delete");
    case StmtKind::Raw:        return QStringLiteral("Raw");
    }
    return QStringLiteral("?");
}

// How many statements a refusal takes down with it, counted the way the parser
// counts them, so the shares add up against the total it reported.
int countStmts(const Stmt &s)
{
    if (s.kind == StmtKind::Raw) return 1;
    int n = 1;
    for (const StmtPtr &c : s.body)
        if (c) n += countStmts(*c);
    for (const StmtPtr &c : s.elseBody)
        if (c) n += countStmts(*c);
    for (const SwitchCase &c : s.cases)
        for (const StmtPtr &b : c.body)
            if (b) n += countStmts(*b);
    if (s.forInit) n += countStmts(*s.forInit);
    return n;
}

// The sub shape of a statement, because "Expression" on its own says nothing:
// an assignment, a call and a bare ternary are three different problems.
QString shapeOf(const Stmt &s)
{
    if (s.kind == StmtKind::Expression) {
        const Expr *e = s.expr.get();
        if (!e) return QStringLiteral("empty");
        switch (e->kind) {
        case ExprKind::Assign:
            return e->op == QLatin1String("=") ? QStringLiteral("assign")
                                               : QStringLiteral("assign") + e->op;
        case ExprKind::Unary:
            return isIncDec(e->op) ? QStringLiteral("incdec") : QStringLiteral("unary");
        case ExprKind::Call:    return QStringLiteral("call");
        case ExprKind::New:     return QStringLiteral("new");
        case ExprKind::Member:  return QStringLiteral("member");
        case ExprKind::Name:    return QStringLiteral("name");
        case ExprKind::Ternary: return QStringLiteral("ternary");
        default:                return QStringLiteral("other");
        }
    }
    if (s.kind == StmtKind::If) {
        if (!s.elseBody.empty()) return QStringLiteral("if.else");
        return QStringLiteral("if");
    }
    if (s.kind == StmtKind::VarDecl) {
        bool anyInit = false;
        for (const Declarator &d : s.decls)
            if (d.init) anyInit = true;
        return anyInit ? QStringLiteral("decl.init") : QStringLiteral("decl");
    }
    if (s.kind == StmtKind::Return) return s.expr ? QStringLiteral("return.value")
                                                  : QStringLiteral("return");
    return QStringLiteral("-");
}

} // namespace diag

class Lowerer
{
public:
    Lowerer(const Catalog &cat, const Builtins &builtins, const Graph &graph,
            const Project &project, const LowerOptions &opts)
        : m_cat(cat), m_builtins(builtins), m_graph(graph), m_project(project),
          m_opts(opts)
    {
    }

    LowerResult run(const std::vector<StmtPtr> &stmts);

private:
    // ---- graph building
    QString addNode(NodeKind kind, const QString &ref);
    GraphNode *node(const QString &id);
    void setOpt(const QString &id, const QString &key, const QString &value);
    // Lines above a node, put above whatever it already carries. A bare block
    // splices its statements into the block around it, so the first of them is
    // also the block's own entry and two gaps land on the same node: what the
    // author wrote above the brace, and what they wrote inside it. Writing one
    // over the other takes a comment out of their file.
    void addBefore(const QString &id, const QStringList &lines);
    void setInput(const QString &id, const QString &pin, const QString &value);
    void wire(const QString &fromNode, const QString &fromPin, const QString &toNode,
              const QString &toPin);
    NodeDef defForNode(const GraphNode &n) const;
    void bindInput(const QString &nodeId, const QString &pinId, const Val &v);

    // ---- scope
    void pushScope() { m_scopes.append(QHash<QString, Local>()); }
    void popScope() { if (!m_scopes.isEmpty()) m_scopes.removeLast(); }
    Local *findLocal(const QString &name);
    void declareLocal(const QString &name, const QString &type);

    // ---- first pass
    void scanStmts(const std::vector<StmtPtr> &stmts, int depth);
    void scanStmt(const Stmt &s, int depth);
    void scanExpr(const Expr *e, int depth);
    void noteDecl(const QString &name, const QString &type, int depth);
    void noteWrite(const QString &name, int depth);
    bool needsVariable(const QString &name) const;
    void createVariables();

    // ---- statements
    // `blockStart` and `blockEnd` bracket the block inside the text the
    // statements were parsed from; -1 for either turns the formatting work
    // off. `topLevel` is the method body, whose opening brace and closing
    // brace are both outside the text.
    Chain lowerBlock(const std::vector<StmtPtr> &stmts, int blockStart = -1,
                     int blockEnd = -1, bool topLevel = false);
    Chain lowerStmt(const Stmt &s);
    Chain lowerStmtInner(const Stmt &s);
    Chain lowerExprStmt(const Stmt &s);
    Chain lowerAssign(const Expr &e);
    Chain lowerIncDec(const Expr &e);
    Chain lowerVarDecl(const Stmt &s);
    Chain lowerIf(const Stmt &s);
    Chain lowerForEach(const Stmt &s);
    Chain lowerFor(const Stmt &s);
    Chain lowerWhile(const Stmt &s);
    Chain lowerReturn(const Stmt &s);
    Chain lowerSwitch(const Stmt &s);
    Chain rawStatement(const Stmt &s);
    bool rawIsSafe(const Stmt &s) const;
    Chain storeInto(const Expr &lhs, const Val &value, bool plain);
    bool valueIsOneTerm(const Val &v) const;
    // Records where a statement was turned down without a reason of its own.
    // Returns the same empty chain the call site returned before it.
    Chain no(int line);

    // ---- expressions
    Val lowerExpr(const Expr &e);
    Val literalVal(const Expr &e);
    Val nameVal(const QString &name);
    Val unaryVal(const Expr &e);
    Val binaryVal(const Expr &e);
    Val ternaryVal(const Expr &e);
    Val memberVal(const Expr &e);
    Val callVal(const Expr &e);
    Val newVal(const Expr &e);
    Val verbatimVal(const Expr &e);
    Val makeOp(const QString &op, const Val &a, const Val &b);
    Val fail(const QString &why);

    // ---- resolution
    QString methodOn(const QString &cls, const QString &name, int argc,
                     bool wantStatic, bool *ambiguous) const;
    QString globalFn(const QString &name, int argc) const;
    QString anyMethod(const QString &name, int argc, bool hasTarget,
                      bool *ambiguous) const;
    QString constantNamed(const QString &name, QString *type) const;
    QString typeOfName(const QString &name);
    const ScriptEntry *scriptByClass(const QString &cls) const;
    const ScriptEntry *selfScript() const;
    const GraphFunction *functionIn(const ScriptEntry &s, const QString &name,
                                    int argc) const;
    bool bindCallArgs(const QString &nodeId, const MethodSig &sig,
                      const std::vector<ExprPtr> &args);
    bool isStableName(const QString &name) const;
    bool isCallNode(const QString &nodeId) const;
    bool isVerbatimSafe(const Expr &e) const;
    bool castToPattern(const Expr &cond, QString *destName, const Expr **src,
                       QString *cls) const;
    bool serverGuard(const Stmt &s) const;

    // ---- chains
    void attach(Chain &c, const QString &nodeId, const QString &pinId);
    Chain chainWith(const QString &nodeId);
    Chain chainOf(const QStringList &pre, const QString &nodeId);
    void pruneSequences();

    const Catalog &m_cat;
    const Builtins &m_builtins;
    const Graph &m_graph;
    const Project &m_project;
    LowerOptions m_opts;

    LowerResult m_res;
    QHash<QString, Facts> m_facts;
    QHash<QString, QString> m_varForName; // local name -> new variable id
    QVector<QHash<QString, Local>> m_scopes;
    QStringList m_pending; // impure nodes this statement has to run first
    bool m_failed = false;
    // Set when a statement kept its code and that code reads a local this pass
    // replaced with a wire. The statement around it has to keep its code too.
    bool m_unsafeRaw = false;
    mutable QHash<QString, QString> m_constKey;
    mutable QHash<QString, QString> m_constType;

    // Diagnosis only. `m_failWhy` and `m_denyLine` say why the statement being
    // lowered right now was turned down; both are saved and restored around
    // each statement so an inner refusal never answers for the one around it.
    struct Decline {
        QString kind;
        QString shape;
        QString why;
        int line = 0;
        bool fromInside = false; // the statement itself lowered, something in it did not
        int weight = 1;          // statements it takes down, itself included
        int depth = 0;
        QString causeKind;       // the first refusal underneath it, when there was one
        QString causeWhy;
        QString text;
    };
    QVector<Decline> m_declines;
    // Every refusal, including the ones rolled back when the statement around
    // them kept its text as well. That is what says whether a refusal is a
    // cause or a consequence.
    QVector<Decline> m_shadow;
    QString m_failWhy;
    int m_denyLine = 0;
    int m_depth = 0;
};

Chain Lowerer::no(int line)
{
    if (m_denyLine == 0) m_denyLine = line;
    return {};
}

// ---------------------------------------------------------------- graph bits

QString Lowerer::addNode(NodeKind kind, const QString &ref)
{
    GraphNode n;
    n.id = nextId(QStringLiteral("n"));
    n.kind = kind;
    n.ref = ref;
    n.x = m_opts.originX;
    n.y = m_opts.originY;
    m_res.nodes.append(n);
    return n.id;
}

GraphNode *Lowerer::node(const QString &id)
{
    for (GraphNode &n : m_res.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

void Lowerer::setOpt(const QString &id, const QString &key, const QString &value)
{
    if (GraphNode *n = node(id)) n->opts.insert(key, value);
}

void Lowerer::addBefore(const QString &id, const QStringList &lines)
{
    if (lines.isEmpty()) return;
    GraphNode *n = node(id);
    if (!n) return;
    n->opts.insert(nodefmt::keyBefore(),
                   nodefmt::store(lines) + n->opts.value(nodefmt::keyBefore()));
}

void Lowerer::setInput(const QString &id, const QString &pin, const QString &value)
{
    if (GraphNode *n = node(id)) n->inputs.insert(pin, value);
}

void Lowerer::wire(const QString &fromNode, const QString &fromPin,
                   const QString &toNode, const QString &toPin)
{
    if (fromNode.isEmpty() || toNode.isEmpty() || fromNode == toNode) return;
    GraphEdge e;
    e.id = nextId(QStringLiteral("e"));
    e.from = {fromNode, fromPin};
    e.to = {toNode, toPin};
    m_res.edges.append(e);
}

// Variable nodes are shaped by the variable they point at, and a variable this
// pass just invented is not in the graph yet, so both lists have to be tried.
NodeDef Lowerer::defForNode(const GraphNode &n) const
{
    const auto isEnumFn = [this](const QString &s) { return m_cat.isEnum(s); };

    if (n.kind == NodeKind::VarGet || n.kind == NodeKind::VarSet) {
        const QString id = variableIdOf(n.ref);
        for (const GraphVariable &v : m_res.variables)
            if (v.id == id)
                return m_builtins.variableDef(v, n.kind == NodeKind::VarSet, m_cat);
        if (const GraphVariable *v = variableForRef(m_graph, n.ref))
            return m_builtins.variableDef(*v, n.kind == NodeKind::VarSet, m_cat);
        return {};
    }
    if (m_builtins.contains(n.ref)) return m_builtins.defForNode(n, m_cat);
    const NodeDef fromProject = scriptDefFor(n.ref, m_project, isEnumFn);
    if (fromProject.valid) return fromProject;
    return m_cat.defFor(n.ref);
}

void Lowerer::bindInput(const QString &nodeId, const QString &pinId, const Val &v)
{
    if (!v.valid) return;
    if (!v.isLiteral()) {
        wire(v.node, v.pin, nodeId, pinId);
        return;
    }

    PinType type;
    if (const GraphNode *n = node(nodeId)) {
        const NodeDef d = defForNode(*n);
        if (const Pin *p = d.valid ? d.pin(pinId, PinDir::In) : nullptr) type = p->type;
    }
    const bool typedPin = inlineEditorFor(type) != InlineEditor::None;

    // `null` is already what an unwired object pin generates, and typing it into
    // a string pin would come back out quoted, so it never goes in as text.
    if (v.type.isEmpty()
        && (v.text == QLatin1String("null") || v.text == QLatin1String("NULL"))) {
        if (!typedPin) return;
        const QString raw = addNode(NodeKind::Builtin, IdRawExpr);
        setOpt(raw, QStringLiteral("code"), v.text);
        wire(raw, PinRet, nodeId, pinId);
        return;
    }

    if (typedPin) {
        setInput(nodeId, pinId, v.text);
        return;
    }

    // The pin has no editor to type into, so the value needs a node of its own.
    if (v.type == QLatin1String("typename")) {
        const QString lit = addNode(NodeKind::Builtin, IdLitClass);
        setInput(lit, PinValue, v.text);
        wire(lit, PinRet, nodeId, pinId);
        return;
    }
    const QString lit = addNode(NodeKind::Builtin, bi::Literal);
    setOpt(lit, QStringLiteral("type"), literalTypeName(v.type));
    setInput(lit, PinValue, v.text);
    wire(lit, PinRet, nodeId, pinId);
}

// ------------------------------------------------------------------- scope

Local *Lowerer::findLocal(const QString &name)
{
    for (int i = m_scopes.size() - 1; i >= 0; --i) {
        const auto hit = m_scopes[i].find(name);
        if (hit != m_scopes[i].end()) return &hit.value();
    }
    return nullptr;
}

void Lowerer::declareLocal(const QString &name, const QString &type)
{
    if (m_scopes.isEmpty()) pushScope();
    Local l;
    l.type = bareType(type);
    l.varId = m_varForName.value(name);
    m_scopes.last().insert(name, l);
}

// -------------------------------------------------------------- first pass

void Lowerer::noteDecl(const QString &name, const QString &type, int depth)
{
    if (name.isEmpty()) return;
    Facts &f = m_facts[name];
    if (!f.declared) {
        f.declared = true;
        f.declDepth = depth;
    }
    if (f.type.isEmpty()) f.type = bareType(type);
}

void Lowerer::noteWrite(const QString &name, int depth)
{
    if (name.isEmpty()) return;
    Facts &f = m_facts[name];
    f.writes++;
    f.deepestWrite = qMax(f.deepestWrite, depth);
}

// A local stays a wire while it is written exactly once at the level it was
// declared. Anything else (a counter, a value assigned in one branch and read
// after it) needs somewhere to live between writes.
bool Lowerer::needsVariable(const QString &name) const
{
    const Facts f = m_facts.value(name);
    if (!f.declared) return false;
    if (f.writes > 1) return true;
    return f.deepestWrite > f.declDepth;
}

void Lowerer::createVariables()
{
    QSet<QString> taken;
    for (const GraphVariable &v : m_graph.variables) taken.insert(v.name);

    for (auto it = m_facts.constBegin(); it != m_facts.constEnd(); ++it) {
        if (!needsVariable(it.key())) continue;
        GraphVariable v;
        v.id = nextId(QStringLiteral("v"));
        v.name = it.key();
        while (taken.contains(v.name)) v.name += QStringLiteral("_1");
        taken.insert(v.name);
        v.type = it.value().type.isEmpty() ? QStringLiteral("int") : it.value().type;
        m_res.variables.append(v);
        m_varForName.insert(it.key(), v.id);
        if (v.name != it.key())
            m_res.notes.append(QStringLiteral("Local %1 is written more than once, so it "
                                              "became the variable %2.")
                                   .arg(it.key(), v.name));
        else
            m_res.notes.append(QStringLiteral("Local %1 is written more than once, so it "
                                              "became a variable.").arg(v.name));
    }
}

void Lowerer::scanStmts(const std::vector<StmtPtr> &stmts, int depth)
{
    for (const StmtPtr &s : stmts)
        if (s) scanStmt(*s, depth);
}

void Lowerer::scanStmt(const Stmt &s, int depth)
{
    switch (s.kind) {
    case StmtKind::VarDecl:
        for (const Declarator &d : s.decls) {
            noteDecl(d.name, s.typeName, depth);
            if (d.init) {
                noteWrite(d.name, depth);
                scanExpr(d.init.get(), depth);
            }
        }
        return;
    case StmtKind::Expression:
        scanExpr(s.expr.get(), depth);
        return;
    case StmtKind::If:
        scanExpr(s.expr.get(), depth);
        scanStmts(s.body, depth + 1);
        scanStmts(s.elseBody, depth + 1);
        return;
    case StmtKind::For: {
        const QString counter = countedForCounter(s);
        if (s.forInit) scanStmt(*s.forInit, depth + 1);
        scanExpr(s.forCond.get(), depth + 1);
        // The step of a counted loop is the loop node counting, not a write the
        // graph has to find somewhere to keep.
        if (counter.isEmpty()) scanExpr(s.forStep.get(), depth + 1);
        scanStmts(s.body, depth + 1);
        return;
    }
    case StmtKind::ForEach:
        noteDecl(s.eachValueName, s.typeName, depth + 1);
        noteWrite(s.eachValueName, depth + 1);
        if (!s.eachIndexName.isEmpty()) {
            noteDecl(s.eachIndexName, QStringLiteral("int"), depth + 1);
            noteWrite(s.eachIndexName, depth + 1);
        }
        scanExpr(s.eachCollection.get(), depth);
        scanStmts(s.body, depth + 1);
        return;
    case StmtKind::While:
        scanExpr(s.expr.get(), depth);
        scanStmts(s.body, depth + 1);
        return;
    case StmtKind::Block:
        scanStmts(s.body, depth + 1);
        return;
    case StmtKind::Switch:
        scanExpr(s.expr.get(), depth);
        for (const SwitchCase &c : s.cases) scanStmts(c.body, depth + 1);
        return;
    case StmtKind::Return:
    case StmtKind::Delete:
        scanExpr(s.expr.get(), depth);
        return;
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Raw:
        return;
    }
}

void Lowerer::scanExpr(const Expr *e, int depth)
{
    if (!e) return;
    switch (e->kind) {
    case ExprKind::Assign:
        if (e->target && e->target->kind == ExprKind::Name)
            noteWrite(e->target->text, depth);
        break;
    case ExprKind::Unary:
        if (isIncDec(e->op) && e->target && e->target->kind == ExprKind::Name)
            noteWrite(e->target->text, depth);
        break;
    case ExprKind::Call:
    case ExprKind::Cast: {
        // CastTo fills its destination, so that name is written here even
        // though the syntax makes it look like a read.
        QString dest;
        const Expr *src = nullptr;
        QString cls;
        if (castToPattern(*e, &dest, &src, &cls)) noteWrite(dest, depth);
        break;
    }
    default:
        break;
    }
    if (e->kind == ExprKind::Name && m_facts.contains(e->text)) m_facts[e->text].reads++;
    scanExpr(e->target.get(), depth);
    scanExpr(e->second.get(), depth);
    scanExpr(e->third.get(), depth);
    for (const ExprPtr &a : e->args) scanExpr(a.get(), depth);
}

// -------------------------------------------------------------- chain bits

void Lowerer::attach(Chain &c, const QString &nodeId, const QString &pinId)
{
    if (c.entry.isEmpty()) {
        c.entry = nodeId;
        return;
    }
    wire(c.tailNode, c.tailPin, nodeId, pinId);
}

Chain Lowerer::chainWith(const QString &nodeId)
{
    Chain c;
    for (const QString &p : m_pending) {
        attach(c, p, PinExec);
        c.tailNode = p;
        c.tailPin = PinExec;
    }
    if (!nodeId.isEmpty()) {
        attach(c, nodeId, PinExec);
        c.tailNode = nodeId;
        c.tailPin = PinExec;
    }
    c.ok = true;
    return c;
}

Chain Lowerer::chainOf(const QStringList &pre, const QString &nodeId)
{
    Chain c;
    for (const QString &p : pre) {
        attach(c, p, PinExec);
        c.tailNode = p;
        c.tailPin = PinExec;
    }
    attach(c, nodeId, PinExec);
    c.tailNode = nodeId;
    c.tailPin = PinExec;
    c.ok = true;
    return c;
}

// A Branch has no output to carry on from, so a block that continues past one
// runs it out of a Sequence. Where the Sequence turned out to carry nothing
// else, it is only noise on the canvas, so it is removed again.
void Lowerer::pruneSequences()
{
    for (int i = m_res.nodes.size() - 1; i >= 0; --i) {
        const GraphNode &n = m_res.nodes.at(i);
        if (n.ref != bi::Sequence) continue;
        // The last node keeps its spare output: that is the pin whatever runs
        // after this block gets spliced onto.
        if (n.id == m_res.exitNode) continue;
        int wired = 0;
        int then0 = -1;
        for (int e = 0; e < m_res.edges.size(); ++e) {
            const GraphEdge &edge = m_res.edges.at(e);
            if (edge.from.node != n.id) continue;
            wired++;
            if (edge.from.pin == QLatin1String("then0")) then0 = e;
        }
        if (wired != 1 || then0 < 0) continue;

        const EdgeEnd target = m_res.edges.at(then0).to;
        const QString id = n.id;
        m_res.edges.removeAt(then0);
        for (GraphEdge &edge : m_res.edges)
            if (edge.to.node == id && edge.to.pin == PinExec) edge.to = target;
        if (m_res.entryNode == id) m_res.entryNode = target.node;
        m_res.nodes.removeAt(i);
    }
}

// -------------------------------------------------------------- statements

Chain Lowerer::lowerBlock(const std::vector<StmtPtr> &stmts, int blockStart, int blockEnd,
                          bool topLevel)
{
    Chain out;
    const QString &src = m_opts.sourceText;
    const bool holdTrivia = !src.isEmpty() && blockStart >= 0;
    int prevEnd = blockStart;
    bool lineStart = topLevel;
    // The node whose first generated line is the line the previous statement
    // was written on, or empty when there is no such line to hang a trailing
    // comment back onto.
    QString prevEntry;
    // What the statements of this block stand behind, which is what a comment
    // between them has to be measured against.
    QString blockIndent;
    bool sawStatement = false;

    for (int i = 0; i < int(stmts.size()); ++i) {
        const Stmt *s = stmts.at(i).get();
        if (!s) continue;
        const Chain c = lowerStmt(*s);

        if (holdTrivia && s->srcStart >= 0 && s->srcEnd >= s->srcStart) {
            blockIndent = indentAt(src, s->srcStart);
            sawStatement = true;
            Gap gap;
            if (splitGap(src, prevEnd, s->srcStart, lineStart, &gap)) {
                if (!c.entry.isEmpty()) addBefore(c.entry, gap.before);
                if (!gap.trailing.isEmpty() && !prevEntry.isEmpty())
                    setOpt(prevEntry, nodefmt::keyTrailing(), gap.trailing);
            }
            prevEnd = s->srcEnd;
            lineStart = false;
            // A statement written over several lines has its closing brace on
            // the last of them, and a comment sitting after that brace would
            // move up to the header if it were kept on the node's first line.
            const bool oneLine =
                !src.mid(s->srcStart, s->srcEnd - s->srcStart).contains(QLatin1Char('\n'));
            prevEntry = !c.entry.isEmpty() && oneLine && !writesBlock(s->kind) ? c.entry
                                                                              : QString();
        }

        if (c.entry.isEmpty()) continue;

        if (c.tailPin.isEmpty() && i + 1 < int(stmts.size())) {
            const QString seq = addNode(NodeKind::Builtin, bi::Sequence);
            attach(out, seq, PinExec);
            wire(seq, QStringLiteral("then0"), c.entry, PinExec);
            out.tailNode = seq;
            out.tailPin = QStringLiteral("then1");
            continue;
        }
        attach(out, c.entry, PinExec);
        out.tailNode = c.tailNode;
        out.tailPin = c.tailPin;
    }

    if (holdTrivia && blockEnd >= prevEnd) {
        Gap gap;
        bool holdable = splitEndGap(src, prevEnd, blockEnd, !topLevel, blockIndent, &gap);
        // A block with no statement in it has no indentation of its own for a
        // comment to be measured against, so only its blank lines can be kept.
        if (holdable && !sawStatement)
            for (const QString &l : gap.before)
                if (!nodefmt::isBlankLine(l)) holdable = false;
        if (holdable) {
            if (!gap.trailing.isEmpty() && !prevEntry.isEmpty())
                setOpt(prevEntry, nodefmt::keyTrailing(), gap.trailing);
            out.endTrivia = nodefmt::store(gap.before);
        }
    }

    out.ok = true;
    return out;
}

Chain Lowerer::lowerStmt(const Stmt &s)
{
    const int nodeMark = m_res.nodes.size();
    const int edgeMark = m_res.edges.size();
    const QVector<QHash<QString, Local>> scopeMark = m_scopes;
    const int noteMark = m_res.notes.size();
    const int loweredMark = m_res.statementsLowered;
    const int rawMark = m_res.statementsRaw;

    // The flag belongs to this nesting level: a statement earlier in the block
    // that kept unusable code must not be forgotten because a later one kept
    // usable code, and the statement around them clears it by taking the text
    // of both.
    const bool outerUnsafe = m_unsafeRaw;
    const int declineMark = m_declines.size();
    const int shadowMark = m_shadow.size();
    const QString whyMark = m_failWhy;
    const int denyMark = m_denyLine;
    m_failWhy.clear();
    m_denyLine = 0;
    m_unsafeRaw = false;
    m_failed = false;
    m_pending.clear();
    m_depth++;
    Chain c = lowerStmtInner(s);
    m_depth--;

    if (m_failed || !c.ok) {
        // Half a statement is worse than none: everything it built is dropped
        // and the original text is kept verbatim.
        m_res.nodes.resize(nodeMark);
        m_res.edges.resize(edgeMark);
        m_scopes = scopeMark;
        m_res.statementsLowered = loweredMark;
        m_res.statementsRaw = rawMark;
        // Same rollback the counter gets: what an inner statement recorded is
        // dropped, because this one is about to keep the text of all of it.
        m_declines.resize(declineMark);
        if (diag::enabled()) {
            Decline d;
            d.kind = diag::kindName(s.kind);
            d.shape = diag::shapeOf(s);
            d.why = m_failWhy;
            d.line = m_denyLine;
            d.fromInside = c.ok;
            d.weight = diag::countStmts(s);
            d.depth = m_depth + 1;
            d.text = diag::flat(s.kind == StmtKind::Raw && !s.text.isEmpty()
                                    ? s.text
                                    : stmtToText(s, 0));
            // The first refusal recorded under this one, resolved all the way
            // down: an inner refusal that was itself a consequence carries the
            // reason it inherited, so the root reads out in one hop.
            if (shadowMark < m_shadow.size()) {
                const Decline &inner = m_shadow.at(shadowMark);
                d.causeKind = inner.causeKind.isEmpty()
                                  ? inner.kind + QLatin1Char('/') + inner.shape
                                  : inner.causeKind;
                d.causeWhy = !inner.causeWhy.isEmpty() ? inner.causeWhy
                             : inner.why.isEmpty()
                                 ? QStringLiteral("silent@%1").arg(inner.line)
                                 : inner.why;
            }
            m_declines.append(d);
            m_shadow.append(d);
        }
        // Everything this statement built is gone, so anything it left queued
        // to run first names a node that no longer exists.
        m_pending.clear();
        c = rawStatement(s);
        m_res.statementsRaw++;
        // The refusal ends here. The text is a statement of its own now, and a
        // block around it is still a block, so `if (a) { unreadable(); }` keeps
        // the Branch and turns one line into a raw node rather than turning the
        // whole method back into a text box.
        m_failed = false;
        // The text kept here declares whatever it reads, unless it reads a
        // local from an enclosing statement that the graph turned into a wire.
        // In that case this fallback is broken too, and the statement around it
        // has to keep its code instead. Cleared here because the text of the
        // enclosing statement will contain the declaration as well.
        m_unsafeRaw = outerUnsafe || !rawIsSafe(s);
        m_failWhy = whyMark;
        m_denyLine = denyMark;
        return c;
    }
    while (m_res.notes.size() > noteMark) m_res.notes.removeLast();
    m_res.statementsLowered++;
    m_unsafeRaw = outerUnsafe;
    m_failWhy = whyMark;
    m_denyLine = denyMark;
    return c;
}

Chain Lowerer::lowerStmtInner(const Stmt &s)
{
    switch (s.kind) {
    case StmtKind::Expression: return lowerExprStmt(s);
    case StmtKind::VarDecl:    return lowerVarDecl(s);
    case StmtKind::If:         return lowerIf(s);
    case StmtKind::ForEach:    return lowerForEach(s);
    case StmtKind::For:        return lowerFor(s);
    case StmtKind::While:      return lowerWhile(s);
    case StmtKind::Return:     return lowerReturn(s);
    case StmtKind::Switch:     return lowerSwitch(s);
    case StmtKind::Block: {
        pushScope();
        const Chain c = lowerBlock(s.body, s.bodyStart, s.bodyEnd);
        popScope();
        return c;
    }
    case StmtKind::Break:
        fail(QStringLiteral("There is no Break node, so the statement keeps its code."));
        return no(__LINE__);
    case StmtKind::Continue:
        fail(QStringLiteral("There is no Continue node, so the statement keeps its code."));
        return no(__LINE__);
    case StmtKind::Delete:
        fail(QStringLiteral("There is no Delete node, so the statement keeps its code."));
        return no(__LINE__);
    case StmtKind::Raw:
        fail(QStringLiteral("The parser could not read this statement, so it keeps its "
                            "code."));
        return no(__LINE__);
    }
    return no(__LINE__);
}

Chain Lowerer::rawStatement(const Stmt &s)
{
    const QString text = s.kind == StmtKind::Raw && !s.text.isEmpty()
                             ? s.text
                             : stmtToText(s, 0);
    const QString id = addNode(NodeKind::Builtin, bi::Raw);
    setOpt(id, QStringLiteral("code"), text);
    Chain c;
    c.entry = id;
    c.tailNode = id;
    c.tailPin = PinExec;
    c.ok = true;
    return c;
}

// Kept code only works if every name in it still exists in the generated file.
// A local wired straight from its producer does not, so text that reads one is
// not a fallback, it is a compile error waiting to happen.
bool Lowerer::rawIsSafe(const Stmt &s) const
{
    const QString text = s.kind == StmtKind::Raw && !s.text.isEmpty() ? s.text
                                                                     : stmtToText(s, 0);
    const EnforceScan scan = scanEnforce(text);
    for (const QString &name : scan.identifiers)
        if (!isStableName(name)) return false;
    return true;
}

Chain Lowerer::lowerExprStmt(const Stmt &s)
{
    const Expr *e = s.expr.get();
    if (!e) return no(__LINE__);

    if (e->kind == ExprKind::Assign) return lowerAssign(*e);
    if (e->kind == ExprKind::Unary && isIncDec(e->op)) return lowerIncDec(*e);

    if (e->kind == ExprKind::Call || e->kind == ExprKind::New) {
        const Val v = lowerExpr(*e);
        if (!v.valid || v.isLiteral()) return no(__LINE__);
        Chain c = chainWith(QString());
        // A pure node generates nothing on its own, so a call that resolved to
        // one cannot stand as a statement.
        if (c.tailNode != v.node) return no(__LINE__);
        return c;
    }
    return no(__LINE__);
}

Chain Lowerer::lowerAssign(const Expr &e)
{
    const Expr *lhs = e.target.get();
    const Expr *rhs = e.second.get();
    if (!lhs || !rhs) return no(__LINE__);

    Val value;
    if (e.op == QLatin1String("=")) {
        value = lowerExpr(*rhs);
    } else {
        const QString op = opWithoutAssign(e.op);
        if (!Builtins::binaryOperators().contains(op)) return no(__LINE__);
        const Val cur = lowerExpr(*lhs);
        const Val add = lowerExpr(*rhs);
        if (!cur.valid || !add.valid) return no(__LINE__);
        value = makeOp(op, cur, add);
    }
    if (!value.valid) return no(__LINE__);
    return storeInto(*lhs, value, e.op == QLatin1String("="));
}

Chain Lowerer::lowerIncDec(const Expr &e)
{
    const Expr *lhs = e.target.get();
    if (!lhs) return no(__LINE__);
    const Val cur = lowerExpr(*lhs);
    if (!cur.valid) return no(__LINE__);
    Val one;
    one.text = QStringLiteral("1");
    one.type = QStringLiteral("int");
    one.valid = true;
    const Val sum = makeOp(isIncrement(e.op) ? QStringLiteral("+")
                                             : QStringLiteral("-"),
                           cur, one);
    if (!sum.valid) return no(__LINE__);
    return storeInto(*lhs, sum, false);
}

// A value the generator writes out as one term: a literal, a member read, a
// name it kept verbatim. The Set Member node is the only assignment node whose
// statement has no other way of being written, so it is worth taking only when
// the line it produces is the line that was read in. Anything with a call or an
// operator in it is written the graph's way, which is a different line, and the
// raw statement it replaces said it better.
bool Lowerer::valueIsOneTerm(const Val &v) const
{
    if (!v.valid) return false;
    if (v.isLiteral()) return true;
    for (const GraphNode &n : m_res.nodes) {
        if (n.id != v.node) continue;
        if (n.kind == NodeKind::VarGet) return true;
        return n.ref == IdRawExpr || n.ref == IdSelf || n.ref == bi::Literal
               || n.ref == IdLitClass;
    }
    return false;
}

Chain Lowerer::storeInto(const Expr &lhs, const Val &value, bool plain)
{
    if (lhs.kind == ExprKind::Name) {
        const QString name = lhs.text;
        if (Local *l = findLocal(name)) {
            if (l->varId.isEmpty()) {
                l->value = value;
                if (l->value.type.isEmpty()) l->value.type = l->type;
                l->bound = true;
                return chainWith(QString());
            }
            const QString set = addNode(NodeKind::VarSet,
                                        QStringLiteral("var.set.%1").arg(l->varId));
            bindInput(set, PinValue, value);
            return chainWith(set);
        }
        if (const GraphVariable *v = m_graph.variable(name)) {
            const QString set = addNode(NodeKind::VarSet,
                                        QStringLiteral("var.set.%1").arg(v->id));
            bindInput(set, PinValue, value);
            return chainWith(set);
        }
        if (m_opts.knownLocals.contains(name)) {
            fail(QStringLiteral("%1 is a parameter, and a graph cannot assign to one.")
                     .arg(name));
            return no(__LINE__);
        }
        // Nothing here declares it, so it is a member of the base class. Reads
        // of one already resolve to the name itself; this is the write.
        if (!isClassIdent(name)) {
            fail(QStringLiteral("The left side of this assignment is not a name the graph "
                                "can write to."));
            return no(__LINE__);
        }
        if (!plain || !valueIsOneTerm(value)) {
            fail(QStringLiteral("%1 belongs to the base class, and the node that writes to "
                                "one spells the value out its own way.").arg(name));
            return no(__LINE__);
        }
        const QString set = addNode(NodeKind::Builtin, IdSetMember);
        setOpt(set, QStringLiteral("name"), name);
        bindInput(set, PinValue, value);
        return chainWith(set);
    }

    if (lhs.kind == ExprKind::Member && lhs.target) {
        const Val obj = lowerExpr(*lhs.target);
        if (!obj.valid) return no(__LINE__);
        if (const ScriptEntry *owner = scriptByClass(obj.type)) {
            for (const GraphVariable &v : owner->graph.variables) {
                if (v.name != lhs.text) continue;
                const QString set =
                    addNode(NodeKind::Call,
                            QStringLiteral("sv.set.%1.%2").arg(owner->id, v.id));
                bindInput(set, PinTarget, obj);
                bindInput(set, PinValue, value);
                return chainWith(set);
            }
        }
        // A field of an object whose class nothing here describes. The name is
        // still the name the generated file has to write.
        if (!plain || !valueIsOneTerm(value)) {
            fail(QStringLiteral("%1 is not a member this project declares, and the node "
                                "that writes to one spells the value out its own way.")
                     .arg(lhs.text));
            return no(__LINE__);
        }
        if (!isClassIdent(lhs.text)) {
            fail(QStringLiteral("%1 is not a member name the graph can write to.")
                     .arg(lhs.text));
            return no(__LINE__);
        }
        const QString set = addNode(NodeKind::Builtin, IdSetMember);
        setOpt(set, QStringLiteral("name"), lhs.text);
        bindInput(set, PinTarget, obj);
        bindInput(set, PinValue, value);
        return chainWith(set);
    }

    // One slot of an array, which is the single most common assignment the
    // graph had no node for.
    if (lhs.kind == ExprKind::Index && lhs.target && lhs.second) {
        if (!plain) {
            fail(QStringLiteral("A Set Element node spells out the whole value, so a "
                                "compound assignment to one slot keeps its code."));
            return no(__LINE__);
        }
        const Val arr = lowerExpr(*lhs.target);
        if (!arr.valid) return no(__LINE__);
        const Val index = lowerExpr(*lhs.second);
        if (!index.valid) return no(__LINE__);
        // The index pin is an int, and a literal goes into it as typed text, so
        // a map key would come back out of it stripped of its quotes.
        if (index.isLiteral() && index.type != QLatin1String("int")) {
            fail(QStringLiteral("This is indexed by a %1, and the Set Element node counts "
                                "in ints.")
                     .arg(index.type.isEmpty() ? QStringLiteral("value") : index.type));
            return no(__LINE__);
        }
        const QString set = addNode(NodeKind::Builtin, IdSetElement);
        bindInput(set, QStringLiteral("arr"), arr);
        bindInput(set, QStringLiteral("index"), index);
        bindInput(set, PinValue, value);
        return chainWith(set);
    }

    fail(QStringLiteral("The left side of this assignment is not something the graph can "
                        "write to."));
    return no(__LINE__);
}

Chain Lowerer::lowerVarDecl(const Stmt &s)
{
    Chain out;
    for (const Declarator &d : s.decls) {
        Val init;
        if (d.init) {
            init = lowerExpr(*d.init);
            if (!init.valid) return no(__LINE__);
            // `float frames = m_FrameCount;` is a widening, not a second name
            // for the same value, and wiring the int straight through would
            // drop it. Only a literal is safe to retype, because it is written
            // into the pin that consumes it.
            const QString declared = bareType(s.typeName);
            if (!init.isLiteral() && !declared.isEmpty() && !init.type.isEmpty()
                && declared != init.type && isPrimitiveType(declared)
                && isPrimitiveType(init.type)) {
                fail(QStringLiteral("%1 is declared %2 and given a %3, so the conversion "
                                    "has to stay in code.")
                         .arg(d.name, declared, init.type));
                return no(__LINE__);
            }
            // A local holding a call and read more than once is what stops the
            // same call being written out once per reader.
            if (!init.isLiteral() && m_facts.value(d.name).reads > 1
                && isCallNode(init.node) && !needsVariable(d.name)) {
                fail(QStringLiteral("%1 holds the result of a call and is read more than "
                                    "once, so it has to stay a local.").arg(d.name));
                return no(__LINE__);
            }
            // The declared type beats whatever the initialiser looked like:
            // `EntityAI e = GetSomething();` is an EntityAI from here on.
            if (!s.typeName.isEmpty()) init.type = declared;
        }
        // A name the caller says is already in scope is one that has to keep
        // its name: an event parameter, or a local a later block still reads.
        if (m_opts.knownLocals.contains(d.name)) {
            fail(QStringLiteral("%1 is read outside this block, so its declaration has to "
                                "stay as code.").arg(d.name));
            return no(__LINE__);
        }
        declareLocal(d.name, s.typeName);
        Local *l = findLocal(d.name);
        if (!l) return no(__LINE__);

        if (l->varId.isEmpty()) {
            if (d.init) {
                l->value = init;
                l->bound = true;
            }
            const Chain c = chainWith(QString());
            m_pending.clear();
            if (!c.entry.isEmpty()) {
                attach(out, c.entry, PinExec);
                out.tailNode = c.tailNode;
                out.tailPin = c.tailPin;
            }
            continue;
        }

        if (!d.init) continue;
        const QString set = addNode(NodeKind::VarSet,
                                    QStringLiteral("var.set.%1").arg(l->varId));
        bindInput(set, PinValue, init);
        const Chain c = chainWith(set);
        m_pending.clear();
        attach(out, c.entry, PinExec);
        out.tailNode = c.tailNode;
        out.tailPin = c.tailPin;
    }
    out.ok = true;
    return out;
}

// `if (!GetGame().IsServer()) return;` is the guard every authoritative event
// needs, and the graph has a node that says exactly that.
bool Lowerer::serverGuard(const Stmt &s) const
{
    if (!s.elseBody.empty() || s.body.size() != 1) return false;
    const Stmt *only = s.body.front().get();
    if (!only || only->kind != StmtKind::Return || only->expr) return false;
    const Expr *c = s.expr.get();
    if (!c || c->kind != ExprKind::Unary || c->op != QLatin1String("!")) return false;
    const Expr *inner = c->target.get();
    while (inner && inner->kind == ExprKind::Paren) inner = inner->target.get();
    if (!inner || inner->kind != ExprKind::Call) return false;
    const Expr *callee = inner->target.get();
    if (!callee || callee->kind != ExprKind::Member
        || callee->text != QLatin1String("IsServer"))
        return false;
    const Expr *obj = callee->target.get();
    return obj && obj->kind == ExprKind::Call && obj->target
           && obj->target->kind == ExprKind::Name
           && obj->target->text == QLatin1String("GetGame");
}

bool Lowerer::castToPattern(const Expr &cond, QString *destName, const Expr **src,
                            QString *cls) const
{
    const Expr *callee = cond.target.get();
    // The parser folds `T.CastTo(dest, value)` into a Cast: the destination is
    // `second` and the value being cast is `target`.
    if (cond.kind == ExprKind::Cast && cond.op == QLatin1String("CastTo")) {
        if (!cond.second || cond.second->kind != ExprKind::Name || !cond.target)
            return false;
        *destName = cond.second->text;
        *src = cond.target.get();
        *cls = cond.typeName;
        return true;
    }
    if (cond.kind != ExprKind::Call || cond.args.size() != 2) return false;
    if (!callee || callee->kind != ExprKind::Member
        || callee->text != QLatin1String("CastTo"))
        return false;
    const Expr *obj = callee->target.get();
    if (!obj || obj->kind != ExprKind::Name) return false;
    if (cond.args.at(0)->kind != ExprKind::Name) return false;
    *destName = cond.args.at(0)->text;
    *src = cond.args.at(1).get();
    *cls = obj->text;
    return true;
}

Chain Lowerer::lowerIf(const Stmt &s)
{
    const Expr *cond = s.expr.get();
    if (!cond) return no(__LINE__);

    if (serverGuard(s)) {
        const QString guard = addNode(NodeKind::Builtin, IdServerOnly);
        return chainWith(guard);
    }

    // The CastTo pattern: the local it fills is only meaningful inside the
    // success branch, which is what the Cast To node already says.
    QString destName;
    const Expr *src = nullptr;
    QString cls;
    if (castToPattern(*cond, &destName, &src, &cls) && src) {
        Local *dest = findLocal(destName);
        if (dest && dest->varId.isEmpty()) {
            const Val obj = lowerExpr(*src);
            if (!obj.valid) return no(__LINE__);
            const QStringList pre = m_pending;
            m_pending.clear();
            QString target = dest->type;
            if (target.isEmpty() || target == QLatin1String("Class")) target = cls;
            const QString cast = addNode(NodeKind::Builtin, bi::Cast);
            setOpt(cast, QStringLiteral("cls"), target);
            bindInput(cast, QStringLiteral("obj"), obj);

            pushScope();
            Val asVal;
            asVal.node = cast;
            asVal.pin = QStringLiteral("as");
            asVal.type = target;
            asVal.valid = true;
            declareLocal(destName, target);
            if (Local *bound = findLocal(destName)) {
                bound->value = asVal;
                bound->bound = true;
            }
            const Chain t = lowerBlock(s.body, s.bodyStart, s.bodyEnd);
            popScope();
            if (m_unsafeRaw) return no(__LINE__);
            if (!t.entry.isEmpty()) wire(cast, QStringLiteral("success"), t.entry, PinExec);

            pushScope();
            const Chain f = lowerBlock(s.elseBody, s.elseStart, s.elseEnd);
            popScope();
            if (!f.entry.isEmpty()) wire(cast, QStringLiteral("failed"), f.entry, PinExec);
            if (!t.endTrivia.isEmpty()) setOpt(cast, nodefmt::keyEnd(), t.endTrivia);
            if (!f.endTrivia.isEmpty()) setOpt(cast, nodefmt::keyEndElse(), f.endTrivia);
            if (m_unsafeRaw) return no(__LINE__);

            // The local outlives the `if` in the source, but the value only
            // exists on the success branch, so it stays bound to the cast.
            if (Local *after = findLocal(destName)) {
                after->value = asVal;
                after->bound = true;
            }
            Chain out = chainOf(pre, cast);
            out.tailPin.clear();
            return out;
        }
    }

    const Val c = lowerExpr(*cond);
    if (!c.valid) return no(__LINE__);
    const QStringList pre = m_pending;
    m_pending.clear();

    const QString br = addNode(NodeKind::Builtin, bi::Branch);
    bindInput(br, QStringLiteral("cond"), c);

    pushScope();
    const Chain t = lowerBlock(s.body, s.bodyStart, s.bodyEnd);
    popScope();
    if (!t.entry.isEmpty()) wire(br, QStringLiteral("true"), t.entry, PinExec);

    pushScope();
    const Chain f = lowerBlock(s.elseBody, s.elseStart, s.elseEnd);
    popScope();
    if (!f.entry.isEmpty()) wire(br, QStringLiteral("false"), f.entry, PinExec);
    if (!t.endTrivia.isEmpty()) setOpt(br, nodefmt::keyEnd(), t.endTrivia);
    if (!f.endTrivia.isEmpty()) setOpt(br, nodefmt::keyEndElse(), f.endTrivia);
    if (m_unsafeRaw) return no(__LINE__);

    Chain out = chainOf(pre, br);
    out.tailPin.clear();
    return out;
}

namespace {

bool mentionsName(const Expr *e, const QString &name);

bool mentionsName(const std::vector<StmtPtr> &stmts, const QString &name);

bool mentionsName(const Stmt &s, const QString &name)
{
    if (mentionsName(s.expr.get(), name)) return true;
    if (mentionsName(s.forCond.get(), name)) return true;
    if (mentionsName(s.forStep.get(), name)) return true;
    if (mentionsName(s.eachCollection.get(), name)) return true;
    if (s.forInit && mentionsName(*s.forInit, name)) return true;
    for (const Declarator &d : s.decls)
        if (mentionsName(d.init.get(), name)) return true;
    if (mentionsName(s.body, name)) return true;
    if (mentionsName(s.elseBody, name)) return true;
    for (const SwitchCase &c : s.cases) {
        if (mentionsName(c.value.get(), name)) return true;
        if (mentionsName(c.body, name)) return true;
    }
    return s.kind == StmtKind::Raw && s.text.contains(name);
}

bool mentionsName(const std::vector<StmtPtr> &stmts, const QString &name)
{
    for (const StmtPtr &s : stmts)
        if (s && mentionsName(*s, name)) return true;
    return false;
}

bool mentionsName(const Expr *e, const QString &name)
{
    if (!e) return false;
    if (e->kind == ExprKind::Name && e->text == name) return true;
    if (mentionsName(e->target.get(), name)) return true;
    if (mentionsName(e->second.get(), name)) return true;
    if (mentionsName(e->third.get(), name)) return true;
    for (const ExprPtr &a : e->args)
        if (mentionsName(a.get(), name)) return true;
    return false;
}

} // namespace

Chain Lowerer::lowerForEach(const Stmt &s)
{
    const Expr *coll = s.eachCollection.get();
    if (!coll) return no(__LINE__);
    // The For Each node binds a counter, and a map binds a key of whatever type
    // it was declared with. Generating `int` for a string key would change what
    // the loop does, so that shape keeps its code.
    if (!s.eachIndexType.isEmpty() && s.eachIndexType != QLatin1String("int")) {
        fail(QStringLiteral("foreach over a map binds a %1 key, which the For Each node "
                            "cannot carry.").arg(s.eachIndexType));
        return no(__LINE__);
    }
    // Both names come out of pins, so neither can be written to.
    for (const QString &bound : {s.eachValueName, s.eachIndexName}) {
        if (bound.isEmpty() || m_varForName.value(bound).isEmpty()) continue;
        fail(QStringLiteral("%1 is assigned inside the loop, and a For Each node binds it "
                            "fresh on every pass.").arg(bound));
        return no(__LINE__);
    }
    const Val arr = lowerExpr(*coll);
    if (!arr.valid) return no(__LINE__);
    const QStringList pre = m_pending;
    m_pending.clear();

    const QString fe = addNode(NodeKind::Builtin, bi::ForEach);
    bindInput(fe, QStringLiteral("array"), arr);
    // What the loop declared, kept as written. Without it the generator has
    // only the array pin to go on, and for anything but a catalogue call that
    // is `auto`: legal Enforce, but not the line the author wrote.
    if (!s.typeName.isEmpty()) setOpt(fe, QStringLiteral("type"), s.typeName);
    if (!s.eachValueName.isEmpty()) setOpt(fe, QStringLiteral("item"), s.eachValueName);

    pushScope();
    declareLocal(s.eachValueName, s.typeName);
    if (Local *item = findLocal(s.eachValueName)) {
        item->value = {fe, QStringLiteral("item"), QString(), bareType(s.typeName), true};
        item->bound = true;
    }
    // The counted form of foreach only exists in the generated code when the
    // index pin is wired, so it is only bound when the body actually reads it.
    if (!s.eachIndexName.isEmpty() && mentionsName(s.body, s.eachIndexName)) {
        setOpt(fe, QStringLiteral("idx"), s.eachIndexName);
        declareLocal(s.eachIndexName, QStringLiteral("int"));
        if (Local *idx = findLocal(s.eachIndexName)) {
            idx->value = {fe, QStringLiteral("index"), QString(),
                          QStringLiteral("int"), true};
            idx->bound = true;
        }
    }
    const Chain body = lowerBlock(s.body, s.bodyStart, s.bodyEnd);
    popScope();
    if (!body.entry.isEmpty()) wire(fe, QStringLiteral("body"), body.entry, PinExec);
    if (!body.endTrivia.isEmpty()) setOpt(fe, nodefmt::keyEnd(), body.endTrivia);
    if (m_unsafeRaw) return no(__LINE__);

    Chain out = chainOf(pre, fe);
    out.tailNode = fe;
    out.tailPin = QStringLiteral("done");
    return out;
}

// For Loop is the counted form and nothing else: `for (int i = a; i < b; i++)`.
// Any other shape keeps its exact meaning only as code.
Chain Lowerer::lowerFor(const Stmt &s)
{
    const QString var = countedForCounter(s);
    if (var.isEmpty()) return no(__LINE__);
    // The counter comes out of the loop node's index pin, which nothing can
    // assign to, so a body that writes it is a different loop.
    if (!m_varForName.value(var).isEmpty()) {
        fail(QStringLiteral("%1 is assigned inside the loop, and a For Loop node counts "
                            "on its own.").arg(var));
        return no(__LINE__);
    }
    const Expr *first = s.forInit->decls.front().init.get();
    const Expr *cond = s.forCond.get();

    const Val a = lowerExpr(*first);
    const Val b = lowerExpr(*cond->second);
    if (!a.valid || !b.valid) return no(__LINE__);
    const QStringList pre = m_pending;
    m_pending.clear();

    const QString loop = addNode(NodeKind::Builtin, bi::ForLoop);
    setOpt(loop, QStringLiteral("var"), var);
    bindInput(loop, QStringLiteral("first"), a);
    bindInput(loop, QStringLiteral("last"), b);

    pushScope();
    declareLocal(var, QStringLiteral("int"));
    if (Local *l = findLocal(var)) {
        l->value = {loop, QStringLiteral("index"), QString(), QStringLiteral("int"), true};
        l->bound = true;
    }
    const Chain body = lowerBlock(s.body, s.bodyStart, s.bodyEnd);
    popScope();
    if (!body.entry.isEmpty()) wire(loop, QStringLiteral("body"), body.entry, PinExec);
    if (!body.endTrivia.isEmpty()) setOpt(loop, nodefmt::keyEnd(), body.endTrivia);
    if (m_unsafeRaw) return no(__LINE__);

    Chain out = chainOf(pre, loop);
    out.tailNode = loop;
    out.tailPin = QStringLiteral("done");
    return out;
}

Chain Lowerer::lowerWhile(const Stmt &s)
{
    const Expr *cond = s.expr.get();
    if (!cond) return no(__LINE__);
    const Val c = lowerExpr(*cond);
    if (!c.valid) return no(__LINE__);
    // The condition is re-read every pass, and a call node placed ahead of the
    // loop would only run once, so that shape stays as code.
    if (!m_pending.isEmpty()) {
        fail(QStringLiteral("This while condition calls something, which a graph would "
                            "only run once, so the loop is kept as code."));
        return no(__LINE__);
    }

    const QString loop = addNode(NodeKind::Builtin, bi::While);
    bindInput(loop, QStringLiteral("cond"), c);
    pushScope();
    const Chain body = lowerBlock(s.body, s.bodyStart, s.bodyEnd);
    popScope();
    if (!body.entry.isEmpty()) wire(loop, QStringLiteral("body"), body.entry, PinExec);
    if (!body.endTrivia.isEmpty()) setOpt(loop, nodefmt::keyEnd(), body.endTrivia);
    if (m_unsafeRaw) return no(__LINE__);

    // Built from the empty list on purpose: lowering the body left its own
    // pending nodes behind, and none of them run before the loop.
    Chain out = chainOf({}, loop);
    out.tailNode = loop;
    out.tailPin = QStringLiteral("done");
    return out;
}

Chain Lowerer::lowerReturn(const Stmt &s)
{
    Val v;
    if (s.expr) {
        v = lowerExpr(*s.expr);
        if (!v.valid) return no(__LINE__);
    }
    const QString ret = addNode(NodeKind::Builtin, bi::Return);
    if (s.expr) bindInput(ret, QStringLiteral("value"), v);
    Chain out = chainWith(ret);
    out.tailPin.clear();
    return out;
}

// A switch becomes a chain of Branches, which is the same program as long as
// every case ends its own flow. Fallthrough between non-empty cases does not
// survive that, so it keeps its code instead.
Chain Lowerer::lowerSwitch(const Stmt &s)
{
    const Expr *subject = s.expr.get();
    if (!subject || s.cases.empty()) return no(__LINE__);
    if (subject->kind != ExprKind::Name && subject->kind != ExprKind::Member) return no(__LINE__);
    if (!isVerbatimSafe(*subject)) return no(__LINE__);

    struct Arm {
        QVector<const Expr *> values; // empty for default
        const std::vector<StmtPtr> *body = nullptr;
    };
    QVector<Arm> arms;
    QVector<const Expr *> pendingValues;
    for (int i = 0; i < int(s.cases.size()); ++i) {
        const SwitchCase &c = s.cases.at(i);
        if (c.body.empty()) {
            if (!c.value) return no(__LINE__); // an empty default says nothing
            pendingValues.append(c.value.get());
            continue;
        }
        Arm arm;
        arm.values = pendingValues;
        pendingValues.clear();
        if (c.value) arm.values.append(c.value.get());
        arm.body = &c.body;
        // Anything after the last statement of a case would fall into the next
        // one, and a Branch cannot do that.
        const Stmt *last = c.body.back().get();
        const bool ends = last
                          && (last->kind == StmtKind::Break || last->kind == StmtKind::Return
                              || last->kind == StmtKind::Continue);
        if (!ends && i + 1 < int(s.cases.size())) return no(__LINE__);
        arms.append(arm);
    }
    if (!pendingValues.isEmpty() || arms.isEmpty()) return no(__LINE__);

    Chain out;
    QString prevBranch;
    for (const Arm &arm : arms) {
        // The default arm has no test, so it hangs off the last false pin.
        const std::vector<StmtPtr> &body = *arm.body;

        // The test is built first: lowering the body leaves its own pending
        // nodes behind, and the test has to be able to see that nothing runs
        // before the comparison.
        Val test;
        m_pending.clear();
        for (const Expr *value : arm.values) {
            if (!value) return no(__LINE__);
            const Val subj = lowerExpr(*subject);
            const Val want = lowerExpr(*value);
            if (!subj.valid || !want.valid || !m_pending.isEmpty()) return no(__LINE__);
            const Val eq = makeOp(QStringLiteral("=="), subj, want);
            if (!eq.valid) return no(__LINE__);
            test = test.valid ? makeOp(QStringLiteral("||"), test, eq) : eq;
            if (!test.valid) return no(__LINE__);
        }

        Chain armChain;
        pushScope();
        {
            // The trailing break is what ends the case, and the Branch already
            // does that, so it is dropped rather than kept as a raw node.
            const Stmt *last = body.empty() ? nullptr : body.back().get();
            const bool dropLast = last && last->kind == StmtKind::Break;
            Chain acc;
            for (int i = 0; i < int(body.size()); ++i) {
                if (dropLast && i == int(body.size()) - 1) break;
                const Stmt *st = body.at(i).get();
                if (!st) continue;
                const Chain c = lowerStmt(*st);
                if (c.entry.isEmpty()) continue;
                const int lastIndex = int(body.size()) - (dropLast ? 2 : 1);
                if (c.tailPin.isEmpty() && i < lastIndex) {
                    const QString seq = addNode(NodeKind::Builtin, bi::Sequence);
                    attach(acc, seq, PinExec);
                    wire(seq, QStringLiteral("then0"), c.entry, PinExec);
                    acc.tailNode = seq;
                    acc.tailPin = QStringLiteral("then1");
                    continue;
                }
                attach(acc, c.entry, PinExec);
                acc.tailNode = c.tailNode;
                acc.tailPin = c.tailPin;
            }
            armChain = acc;
        }
        popScope();
        if (m_unsafeRaw) return no(__LINE__);

        if (arm.values.isEmpty()) {
            if (prevBranch.isEmpty()) return no(__LINE__);
            if (!armChain.entry.isEmpty())
                wire(prevBranch, QStringLiteral("false"), armChain.entry, PinExec);
            continue;
        }

        const QString br = addNode(NodeKind::Builtin, bi::Branch);
        bindInput(br, QStringLiteral("cond"), test);
        if (prevBranch.isEmpty()) out.entry = br;
        else wire(prevBranch, QStringLiteral("false"), br, PinExec);
        if (!armChain.entry.isEmpty())
            wire(br, QStringLiteral("true"), armChain.entry, PinExec);
        prevBranch = br;
    }
    if (out.entry.isEmpty()) return no(__LINE__);
    out.tailNode = prevBranch;
    out.tailPin.clear();
    out.ok = true;
    return out;
}

// ------------------------------------------------------------- expressions

Val Lowerer::fail(const QString &why)
{
    m_failed = true;
    // The first reason is the deciding one: everything after it is the refusal
    // travelling back up. The note list dedupes, so it cannot be read for this.
    if (m_failWhy.isEmpty() && !why.isEmpty()) m_failWhy = why;
    if (!why.isEmpty() && !m_res.notes.contains(why)) m_res.notes.append(why);
    return {};
}

Val Lowerer::lowerExpr(const Expr &e)
{
    switch (e.kind) {
    case ExprKind::Literal: return literalVal(e);
    case ExprKind::Paren:   return e.target ? lowerExpr(*e.target) : fail(QString());
    case ExprKind::Name:    return nameVal(e.text);
    case ExprKind::Unary:   return unaryVal(e);
    case ExprKind::Binary:  return binaryVal(e);
    case ExprKind::Ternary: return ternaryVal(e);
    case ExprKind::Member:  return memberVal(e);
    case ExprKind::Call:    return callVal(e);
    case ExprKind::New:     return newVal(e);
    case ExprKind::Cast:    return verbatimVal(e);
    case ExprKind::Index:   return verbatimVal(e);
    case ExprKind::Assign:
        return fail(QStringLiteral("An assignment used as a value has no node to be, so "
                                   "the statement is kept as code."));
    }
    return {};
}

Val Lowerer::literalVal(const Expr &e)
{
    Val v;
    v.valid = true;
    v.text = e.text;
    switch (e.literalType) {
    case LiteralType::Int:   v.type = QStringLiteral("int"); break;
    case LiteralType::Float: v.type = QStringLiteral("float"); break;
    case LiteralType::Bool:  v.type = QStringLiteral("bool"); break;
    case LiteralType::Null:
        v.type.clear();
        v.text = e.text.isEmpty() ? QStringLiteral("null") : e.text;
        break;
    case LiteralType::Vector:
    case LiteralType::String: {
        v.type = e.literalType == LiteralType::Vector ? QStringLiteral("vector")
                                                      : QStringLiteral("string");
        QString t = e.text;
        // A pin holds the text without its quotes, which is what the inspector
        // shows and what the generator quotes again. Two spellings have to keep
        // their quotes: an escape would have its backslash escaped a second
        // time, and a space at either end would be trimmed off, and both of
        // those change the string. `"error for " + name` is the reason.
        if (t.size() >= 2 && t.startsWith(QLatin1Char('"')) && t.endsWith(QLatin1Char('"'))
            && !t.contains(QLatin1Char('\\'))) {
            const QString inner = t.mid(1, t.size() - 2);
            if (inner.trimmed() == inner) t = inner;
        }
        v.text = t;
        break;
    }
    }
    return v;
}

Val Lowerer::nameVal(const QString &name)
{
    if (name.isEmpty()) return fail(QString());

    if (Local *l = findLocal(name)) {
        if (!l->varId.isEmpty()) {
            const QString get = addNode(NodeKind::VarGet,
                                        QStringLiteral("var.get.%1").arg(l->varId));
            return {get, PinRet, QString(), l->type, true};
        }
        if (!l->bound)
            return fail(QStringLiteral("%1 is read before anything gives it a value.")
                            .arg(name));
        return l->value;
    }

    if (m_opts.knownLocals.contains(name)) {
        // A parameter keeps its name in the generated method, so it is written
        // as itself. Callers that know the event node rewire these to its pin.
        const QString raw = addNode(NodeKind::Builtin, IdRawExpr);
        setOpt(raw, QStringLiteral("code"), name);
        return {raw, PinRet, QString(), bareType(m_opts.knownLocals.value(name)), true};
    }

    if (const GraphVariable *v = m_graph.variable(name)) {
        const QString get = addNode(NodeKind::VarGet,
                                    QStringLiteral("var.get.%1").arg(v->id));
        return {get, PinRet, QString(), bareType(v->type), true};
    }

    if (name == QLatin1String("this")) {
        const QString self = addNode(NodeKind::Builtin, IdSelf);
        return {self, PinRet, QString(), m_opts.selfClass, true};
    }

    QString constType;
    const QString constKey = constantNamed(name, &constType);
    if (!constKey.isEmpty()) {
        const QString c = addNode(NodeKind::Call, constKey);
        return {c, PinRet, QString(), bareType(constType), true};
    }

    if (m_cat.classId(name) >= 0 || m_cat.isEnum(name) || scriptByClass(name)) {
        Val v;
        v.text = name;
        v.type = QStringLiteral("typename");
        v.valid = true;
        return v;
    }

    // An inherited member, or an enum value the index did not record as a
    // constant. Both keep their name in the generated file, so the name itself
    // is the value.
    const QString raw = addNode(NodeKind::Builtin, IdRawExpr);
    setOpt(raw, QStringLiteral("code"), name);
    return {raw, PinRet, QString(), QString(), true};
}

Val Lowerer::makeOp(const QString &op, const Val &a, const Val &b)
{
    if (!a.valid || !b.valid) return fail(QString());
    const QString id = addNode(NodeKind::Builtin, IdOp);
    setOpt(id, QStringLiteral("op"), op);
    bindInput(id, QStringLiteral("a"), a);
    bindInput(id, QStringLiteral("b"), b);
    const QString type = Builtins::operatorYieldsBool(op) ? QStringLiteral("bool") : a.type;
    return {id, PinRet, QString(), type, true};
}

Val Lowerer::unaryVal(const Expr &e)
{
    const Expr *inner = e.target.get();
    if (!inner) return fail(QString());

    if (e.op == QLatin1String("!")) {
        const Val a = lowerExpr(*inner);
        if (!a.valid) return {};
        const QString id = addNode(NodeKind::Builtin, IdNot);
        bindInput(id, QStringLiteral("a"), a);
        return {id, PinRet, QString(), QStringLiteral("bool"), true};
    }
    if (e.op == QLatin1String("-")) {
        if (inner->kind == ExprKind::Literal
            && (inner->literalType == LiteralType::Int
                || inner->literalType == LiteralType::Float)) {
            Val v = literalVal(*inner);
            v.text = QLatin1Char('-') + v.text;
            return v;
        }
        const Val a = lowerExpr(*inner);
        if (!a.valid) return {};
        Val zero;
        zero.text = QStringLiteral("0");
        zero.type = a.type.isEmpty() ? QStringLiteral("int") : a.type;
        zero.valid = true;
        return makeOp(QStringLiteral("-"), zero, a);
    }
    return verbatimVal(e);
}

Val Lowerer::binaryVal(const Expr &e)
{
    if (!e.target || !e.second) return fail(QString());
    if (!Builtins::binaryOperators().contains(e.op)) return verbatimVal(e);
    const Val a = lowerExpr(*e.target);
    if (!a.valid) return {};
    const Val b = lowerExpr(*e.second);
    if (!b.valid) return {};
    return makeOp(e.op, a, b);
}

Val Lowerer::ternaryVal(const Expr &e)
{
    if (!e.target || !e.second || !e.third) return fail(QString());
    const Val c = lowerExpr(*e.target);
    const Val a = lowerExpr(*e.second);
    const Val b = lowerExpr(*e.third);
    if (!c.valid || !a.valid || !b.valid) return {};
    const QString id = addNode(NodeKind::Builtin, IdSelect);
    bindInput(id, QStringLiteral("cond"), c);
    bindInput(id, QStringLiteral("a"), a);
    bindInput(id, QStringLiteral("b"), b);
    return {id, PinRet, QString(), a.type, true};
}

Val Lowerer::memberVal(const Expr &e)
{
    const Expr *obj = e.target.get();
    if (!obj) return fail(QString());

    // A member of another script in this project is a real node, and it reads
    // through the target pin rather than by name, so it survives a rename.
    if (obj->kind == ExprKind::Name) {
        if (const ScriptEntry *owner = scriptByClass(typeOfName(obj->text))) {
            for (const GraphVariable &v : owner->graph.variables) {
                if (v.name != e.text) continue;
                const Val target = lowerExpr(*obj);
                if (!target.valid) return {};
                const QString get =
                    addNode(NodeKind::Call,
                            QStringLiteral("sv.get.%1.%2").arg(owner->id, v.id));
                bindInput(get, PinTarget, target);
                return {get, PinRet, QString(), bareType(v.type), true};
            }
        }
    }
    return verbatimVal(e);
}

// Only a name the generated file still spells the same way can be written out
// verbatim. A local that became a wire has no name there at all.
bool Lowerer::isStableName(const QString &name) const
{
    for (int i = m_scopes.size() - 1; i >= 0; --i) {
        const auto hit = m_scopes.at(i).constFind(name);
        if (hit == m_scopes.at(i).constEnd()) continue;
        return !hit.value().varId.isEmpty();
    }
    return true;
}

// A node that runs code rather than reading a value. The generator inlines a
// pure one at every pin it feeds, so a local holding one and read twice would
// become two calls rather than one.
bool Lowerer::isCallNode(const QString &nodeId) const
{
    for (const GraphNode &n : m_res.nodes) {
        if (n.id != nodeId) continue;
        if (n.ref.startsWith(QLatin1String("fn.call."))) return true;
        if (n.kind != NodeKind::Call) return false;
        return n.ref.startsWith(QLatin1Char('m')) || n.ref.startsWith(QLatin1Char('g'));
    }
    return false;
}

bool Lowerer::isVerbatimSafe(const Expr &e) const
{
    if (e.kind == ExprKind::Name && !isStableName(e.text)) return false;
    if (e.target && !isVerbatimSafe(*e.target)) return false;
    if (e.second && !isVerbatimSafe(*e.second)) return false;
    if (e.third && !isVerbatimSafe(*e.third)) return false;
    for (const ExprPtr &a : e.args)
        if (a && !isVerbatimSafe(*a)) return false;
    return true;
}

Val Lowerer::verbatimVal(const Expr &e)
{
    const QString text = exprToText(e);
    if (text.trimmed().isEmpty())
        return fail(QStringLiteral("An expression could not be written back out."));
    if (!isVerbatimSafe(e))
        return fail(QStringLiteral("%1 reads a local the graph turned into a wire, so it "
                                   "has no name to refer to.").arg(text));
    const QString raw = addNode(NodeKind::Builtin, IdRawExpr);
    setOpt(raw, QStringLiteral("code"), text);
    return {raw, PinRet, QString(), QString(), true};
}

Val Lowerer::newVal(const Expr &e)
{
    const QString cls = e.typeName;
    if (cls.isEmpty()) return fail(QString());

    // A constructor entry carries the argument pins; the New Object builtin has
    // none, so it can only stand in for a constructor that takes nothing.
    bool ambiguous = false;
    const QString ctor = methodOn(cls, cls, int(e.args.size()), false, &ambiguous);
    if (!ctor.isEmpty()) {
        const MethodSig sig = m_cat.method(ctor);
        if (sig.valid && (sig.flags & flag::Ctor)) {
            const QString id = addNode(NodeKind::Call, ctor);
            if (!bindCallArgs(id, sig, e.args)) return {};
            m_pending.append(id);
            return {id, PinRet, QString(), cls, true};
        }
    }
    if (!e.args.empty())
        return fail(QStringLiteral("new %1 takes arguments, and nothing in the catalogue "
                                   "declares that constructor.").arg(cls));
    const QString id = addNode(NodeKind::Builtin, IdNew);
    setOpt(id, QStringLiteral("cls"), cls);
    m_pending.append(id);
    return {id, PinRet, QString(), cls, true};
}

Val Lowerer::callVal(const Expr &e)
{
    const Expr *callee = e.target.get();
    if (!callee) return fail(QString());
    const int argc = int(e.args.size());

    // ---- F(args)
    if (callee->kind == ExprKind::Name) {
        const QString name = callee->text;

        if (name == QLatin1String("Print") && argc == 1) {
            const Val a = lowerExpr(*e.args.at(0));
            if (!a.valid) return {};
            const QString id = addNode(NodeKind::Builtin, bi::Print);
            bindInput(id, QStringLiteral("value"), a);
            m_pending.append(id);
            return {id, QString(), QString(), QString(), true};
        }

        const QString g = globalFn(name, argc);
        if (!g.isEmpty()) {
            const MethodSig sig = m_cat.globalFn(g);
            const QString id = addNode(NodeKind::Call, g);
            if (!bindCallArgs(id, sig, e.args)) return {};
            if (!m_cat.defFor(g).pure) m_pending.append(id);
            return {id, PinRet, QString(), bareType(sig.ret), true};
        }

        bool ambiguous = false;
        const QString own = methodOn(m_opts.selfClass, name, argc, false, &ambiguous);
        if (!own.isEmpty()) {
            const MethodSig sig = m_cat.method(own);
            const QString id = addNode(NodeKind::Call, own);
            if (!bindCallArgs(id, sig, e.args)) return {};
            if (!m_cat.defFor(own).pure) m_pending.append(id);
            return {id, PinRet, QString(), bareType(sig.ret), true};
        }

        if (const ScriptEntry *self = selfScript()) {
            if (const GraphFunction *fn = functionIn(*self, name, argc)) {
                const QString id =
                    addNode(NodeKind::Call,
                            QStringLiteral("fn.call.%1.%2").arg(self->id, fn->id));
                for (int i = 0; i < argc; ++i) {
                    const Val a = lowerExpr(*e.args.at(i));
                    if (!a.valid) return {};
                    bindInput(id, QStringLiteral("p%1").arg(i), a);
                }
                const auto isEnumFn = [this](const QString &s) { return m_cat.isEnum(s); };
                if (!scriptDefFor(id, m_project, isEnumFn).pure) m_pending.append(id);
                return {id, PinRet, QString(),
                        fn->returns.isEmpty() ? QString() : bareType(fn->returns), true};
            }
        }

        ambiguous = false;
        const QString any = anyMethod(name, argc, false, &ambiguous);
        if (!any.isEmpty() && !ambiguous) {
            const MethodSig sig = m_cat.method(any);
            const QString id = addNode(NodeKind::Call, any);
            if (!bindCallArgs(id, sig, e.args)) return {};
            if (!m_cat.defFor(any).pure) m_pending.append(id);
            return {id, PinRet, QString(), bareType(sig.ret), true};
        }
        if (ambiguous)
            return fail(QStringLiteral("%1() is declared on several classes and nothing "
                                       "here says which one is meant.").arg(name));
        return fail(QStringLiteral("Nothing the catalogue or this project declares is "
                                   "called %1().").arg(name));
    }

    // ---- obj.M(args) and Class.M(args)
    if (callee->kind != ExprKind::Member || !callee->target)
        return fail(QStringLiteral("A call through something other than a name is kept as "
                                   "code."));

    const QString name = callee->text;
    const Expr *obj = callee->target.get();

    const bool objIsValue = obj->kind != ExprKind::Name || findLocal(obj->text)
                            || m_graph.variable(obj->text)
                            || m_opts.knownLocals.contains(obj->text)
                            || obj->text == QLatin1String("this");
    if (!objIsValue) {
        const QString cls = obj->text;
        // Cast and CastTo are static on Class, so resolving them through the
        // catalogue would rewrite `PlayerBase.Cast(x)` as `Class.Cast(x)` and
        // lose the type the call exists for.
        if (name == QLatin1String("Cast") || name == QLatin1String("CastTo"))
            return verbatimVal(e);

        if (const ScriptEntry *owner = scriptByClass(cls)) {
            if (const GraphFunction *fn = functionIn(*owner, name, argc)) {
                if (fn->isStatic) {
                    const QString id =
                        addNode(NodeKind::Call,
                                QStringLiteral("fn.call.%1.%2").arg(owner->id, fn->id));
                    for (int i = 0; i < argc; ++i) {
                        const Val a = lowerExpr(*e.args.at(i));
                        if (!a.valid) return {};
                        bindInput(id, QStringLiteral("p%1").arg(i), a);
                    }
                    const auto isEnumFn = [this](const QString &s) {
                        return m_cat.isEnum(s);
                    };
                    if (!scriptDefFor(id, m_project, isEnumFn).pure) m_pending.append(id);
                    return {id, PinRet, QString(),
                            fn->returns.isEmpty() ? QString() : bareType(fn->returns),
                            true};
                }
            }
        }
        if (m_cat.classId(cls) >= 0) {
            bool ambiguous = false;
            const QString key = methodOn(cls, name, argc, true, &ambiguous);
            if (!key.isEmpty()) {
                const MethodSig sig = m_cat.method(key);
                const QString id = addNode(NodeKind::Call, key);
                if (!bindCallArgs(id, sig, e.args)) return {};
                if (!m_cat.defFor(key).pure) m_pending.append(id);
                return {id, PinRet, QString(), bareType(sig.ret), true};
            }
        }
        // A capital letter means a type, and a type nothing declares is not
        // something to guess at. A member this class inherited is a value, so
        // it carries on below.
        if (cls == QLatin1String("super") || cls.isEmpty()
            || cls.at(0).isUpper() || m_cat.isEnum(cls))
            return verbatimVal(e);
    }

    const Val target = lowerExpr(*obj);
    if (!target.valid) return {};

    if (const ScriptEntry *owner = scriptByClass(target.type)) {
        if (const GraphFunction *fn = functionIn(*owner, name, argc)) {
            const QString id = addNode(NodeKind::Call,
                                       QStringLiteral("fn.call.%1.%2").arg(owner->id, fn->id));
            bindInput(id, PinTarget, target);
            for (int i = 0; i < argc; ++i) {
                const Val a = lowerExpr(*e.args.at(i));
                if (!a.valid) return {};
                bindInput(id, QStringLiteral("p%1").arg(i), a);
            }
            const auto isEnumFn = [this](const QString &s) { return m_cat.isEnum(s); };
            if (!scriptDefFor(id, m_project, isEnumFn).pure) m_pending.append(id);
            return {id, PinRet, QString(),
                    fn->returns.isEmpty() ? QString() : bareType(fn->returns), true};
        }
    }

    bool ambiguous = false;
    QString key = methodOn(target.type, name, argc, false, &ambiguous);
    if (key.isEmpty()) key = anyMethod(name, argc, true, &ambiguous);
    if (key.isEmpty()) {
        if (ambiguous)
            return fail(QStringLiteral("%1() is declared on several classes and the type "
                                       "of what it is called on is not known here.")
                            .arg(name));
        return fail(QStringLiteral("Nothing the catalogue declares is called %1().")
                        .arg(name));
    }

    const MethodSig sig = m_cat.method(key);
    const QString id = addNode(NodeKind::Call, key);
    if (!(sig.flags & flag::Static)) bindInput(id, PinTarget, target);
    if (!bindCallArgs(id, sig, e.args)) return {};
    if (!m_cat.defFor(key).pure) m_pending.append(id);
    return {id, PinRet, QString(), bareType(sig.ret), true};
}

// Arguments go in positionally. An `out` parameter takes no input pin at all:
// the value comes back on the node's own output, so the local named at that
// position is bound to it instead.
bool Lowerer::bindCallArgs(const QString &nodeId, const MethodSig &sig,
                           const std::vector<ExprPtr> &args)
{
    if (int(args.size()) > sig.params.size()) {
        fail(QStringLiteral("%1() is declared with %2 parameters and is called with %3.")
                 .arg(sig.name).arg(sig.params.size()).arg(args.size()));
        return false;
    }
    for (int i = 0; i < int(args.size()); ++i) {
        const Expr *a = unwrapDirection(args.at(i).get());
        if (!a) return false;
        const int dir = i < sig.params.size() ? sig.params.at(i).dir : 0;

        if (dir == 1 || dir == 2) {
            if (a->kind != ExprKind::Name) {
                fail(QStringLiteral("%1() writes back through argument %2, and only a "
                                    "local can take that.").arg(sig.name).arg(i + 1));
                return false;
            }
            if (dir == 2) {
                const Val in = lowerExpr(*a);
                if (!in.valid) return false;
                bindInput(nodeId, QStringLiteral("p%1").arg(i), in);
            }
            Local *l = findLocal(a->text);
            if (!l || !l->varId.isEmpty()) {
                fail(QStringLiteral("%1() writes back into %2, which is not a local this "
                                    "block declares.").arg(sig.name, a->text));
                return false;
            }
            l->value = {nodeId, QStringLiteral("o%1").arg(i), QString(),
                        i < sig.params.size() ? bareType(sig.params.at(i).type) : QString(),
                        true};
            l->bound = true;
            continue;
        }

        const Val v = lowerExpr(*a);
        if (!v.valid) return false;
        bindInput(nodeId, QStringLiteral("p%1").arg(i), v);
    }
    return true;
}

// -------------------------------------------------------------- resolution

QString Lowerer::methodOn(const QString &cls, const QString &name, int argc,
                          bool wantStatic, bool *ambiguous) const
{
    *ambiguous = false;
    const QString owner = catalogClass(cls);
    if (owner.isEmpty() || name.isEmpty()) return {};
    if (m_cat.classId(owner) < 0) return {};

    SearchOptions opts;
    opts.limit = 80;
    opts.ofClass = owner;
    QString best;
    for (const SearchHit &h : m_cat.search(name, opts)) {
        const MethodSig sig = m_cat.method(h.key);
        if (!sig.valid || sig.name != name) continue;
        if (wantStatic && !(sig.flags & (flag::Static | flag::Ctor))) continue;
        int required = 0;
        for (const MethodSig::Param &p : sig.params)
            if (p.def.isEmpty()) required++;
        if (argc < required || argc > sig.params.size()) continue;
        if (best.isEmpty()) best = h.key;
    }
    return best;
}

QString Lowerer::globalFn(const QString &name, int argc) const
{
    if (name.isEmpty()) return {};
    SearchOptions opts;
    opts.limit = 40;
    opts.category = QStringLiteral("Globals");
    for (const SearchHit &h : m_cat.search(name, opts)) {
        const MethodSig sig = m_cat.globalFn(h.key);
        if (!sig.valid || sig.name != name) continue;
        int required = 0;
        for (const MethodSig::Param &p : sig.params)
            if (p.def.isEmpty()) required++;
        if (argc < required || argc > sig.params.size()) continue;
        return h.key;
    }
    return {};
}

// Two declarations of the same name that the generator would write out as the
// same line. Everything it reads off the signature has to agree: the call form
// (`Owner.Name(...)` for a static, `target.Name(...)` for an instance method),
// the type of the local a consumed result lands in, and the parameter list,
// since an `out` parameter has a declaration of its own emitted ahead of the
// call and a trailing default decides whether an argument is written at all.
bool sameEmittedCall(const MethodSig &a, const MethodSig &b)
{
    const int shape = flag::Static | flag::Ctor;
    if ((a.flags & shape) != (b.flags & shape)) return false;
    if ((a.flags & shape) && a.owner != b.owner) return false;
    if (a.ret != b.ret) return false;
    if (a.params.size() != b.params.size()) return false;
    for (int i = 0; i < a.params.size(); ++i) {
        const MethodSig::Param &pa = a.params.at(i);
        const MethodSig::Param &pb = b.params.at(i);
        if (pa.dir != pb.dir) return false;
        if (pa.def.isEmpty() != pb.def.isEmpty()) return false;
        if (pa.dir != 0 && pa.type != pb.type) return false;
    }
    return true;
}

// The last resort for a call whose object has no known type. Several matches
// used to be refused outright, on the grounds that picking one is a guess. It
// is only a guess when the choice shows: `Count()` is declared on array, set
// and map with the same signature, and the generator writes `target.Count()`
// whichever one is meant. So the test is whether the candidates would be
// written differently, not whether there is more than one of them.
//
// `hasTarget` says the call already carries the object it runs against. Without
// one the generator falls back to the owner to decide what to call the method
// on, so the owners have to agree there as well.
QString Lowerer::anyMethod(const QString &name, int argc, bool hasTarget,
                           bool *ambiguous) const
{
    *ambiguous = false;
    if (name.isEmpty()) return {};
    SearchOptions opts;
    opts.limit = 60;
    QString only;
    MethodSig first;
    QSet<QString> owners;
    bool allSame = true;
    for (const SearchHit &h : m_cat.search(name, opts)) {
        const MethodSig sig = m_cat.method(h.key);
        if (!sig.valid || sig.name != name) continue;
        int required = 0;
        for (const MethodSig::Param &p : sig.params)
            if (p.def.isEmpty()) required++;
        if (argc < required || argc > sig.params.size()) continue;
        if (only.isEmpty()) {
            only = h.key;
            first = sig;
        } else if (!sameEmittedCall(first, sig)) {
            allSame = false;
        }
        owners.insert(sig.owner);
    }
    if (owners.size() <= 1) return only;

    // A hook the class this script already declares beats the rest.
    bool onSelf = false;
    const QString mine = methodOn(m_opts.selfClass, name, argc, false, &onSelf);
    if (!mine.isEmpty()) return mine;

    if (allSame && !only.isEmpty()) {
        if (hasTarget) return only;
        // With nothing wired, `GetGame()` and `this` are both possible targets
        // and the owner is what chooses between them, so every candidate has to
        // be one this class can call on itself.
        bool everyOwnerIsSelf = !m_opts.selfClass.isEmpty();
        for (const QString &owner : owners)
            if (owner.isEmpty() || owner == QLatin1String("CGame")
                || !m_cat.isA(m_opts.selfClass, owner))
                everyOwnerIsSelf = false;
        if (everyOwnerIsSelf) return only;
    }
    *ambiguous = true;
    return {};
}

QString Lowerer::constantNamed(const QString &name, QString *type) const
{
    const auto hit = m_constKey.constFind(name);
    if (hit != m_constKey.constEnd()) {
        *type = m_constType.value(name);
        return *hit;
    }
    SearchOptions opts;
    opts.limit = 8;
    opts.category = QStringLiteral("Constants");
    QString key;
    QString found;
    for (const SearchHit &h : m_cat.search(name, opts)) {
        if (h.title != name) continue;
        key = h.key;
        found = h.sig;
        break;
    }
    m_constKey.insert(name, key);
    m_constType.insert(name, found);
    *type = found;
    return key;
}

// The declared type of a name, without building anything for it. Member
// resolution needs the type before it decides which node to place, and a
// speculative node placed and then abandoned would be left on the canvas.
QString Lowerer::typeOfName(const QString &name)
{
    if (Local *l = findLocal(name)) {
        if (!l->type.isEmpty()) return l->type;
        return l->bound ? l->value.type : QString();
    }
    if (m_opts.knownLocals.contains(name)) return bareType(m_opts.knownLocals.value(name));
    if (const GraphVariable *v = m_graph.variable(name)) return bareType(v->type);
    if (name == QLatin1String("this")) return m_opts.selfClass;
    return {};
}

const ScriptEntry *Lowerer::scriptByClass(const QString &cls) const
{
    if (cls.isEmpty()) return nullptr;
    for (const ScriptEntry &s : m_project.scripts)
        if (s.graph.className == cls || s.name == cls) return &s;
    return nullptr;
}

// The script these statements belong to. A class with no base has an empty
// selfClass, and a call to its own helper still has to resolve, so the class
// name of the graph itself is the fallback.
const ScriptEntry *Lowerer::selfScript() const
{
    if (const ScriptEntry *s = scriptByClass(m_opts.selfClass)) return s;
    return scriptByClass(m_graph.className);
}

const GraphFunction *Lowerer::functionIn(const ScriptEntry &s, const QString &name,
                                         int argc) const
{
    for (const GraphFunction &f : s.graph.functions)
        if (f.name == name && f.params.size() == argc) return &f;
    return nullptr;
}

// ------------------------------------------------------------------- driver

LowerResult Lowerer::run(const std::vector<StmtPtr> &stmts)
{
    m_res = LowerResult{};
    m_scopes.clear();
    pushScope();

    scanStmts(stmts, 0);
    createVariables();

    const Chain c = lowerBlock(stmts, m_opts.sourceText.isEmpty() ? -1 : 0,
                               m_opts.sourceText.size(), true);

    if (diag::enabled()) {
        const qint64 id = diag::nextRun();
        // A body the caller can use at all, by the same rule the importer
        // applies before it compares the regenerated text.
        const bool usable = !m_res.nodes.isEmpty() && !c.entry.isEmpty() && !m_unsafeRaw;
        QString sig;
        int total = 0;
        for (const StmtPtr &s : stmts) {
            if (!s) continue;
            sig += QString::number(int(s->kind)) + QLatin1Char(',');
            total += diag::countStmts(*s);
        }
        diag::write(QStringLiteral("R\t%1\t%2\t%3\t%4\t%5\t%6\t%7")
                        .arg(id)
                        .arg(total)
                        .arg(m_res.statementsLowered)
                        .arg(m_res.statementsRaw)
                        .arg(m_declines.size())
                        .arg(usable ? 1 : 0)
                        .arg(sig));
        // One .arg() a field: the multi argument form fills the lowest numbered
        // markers left, which silently ate the last column when it was used here.
        for (const Decline &d : m_declines) {
            QString row = QStringLiteral("S");
            const QStringList fields = {
                QString::number(id),
                d.kind,
                d.shape,
                d.fromInside ? QStringLiteral("1") : QStringLiteral("0"),
                QString::number(d.weight),
                QString::number(d.depth),
                QString::number(d.line),
                d.why.isEmpty() ? QStringLiteral("-") : diag::flat(d.why),
                d.causeKind.isEmpty() ? QStringLiteral("-") : d.causeKind,
                d.causeWhy.isEmpty() ? QStringLiteral("-") : diag::flat(d.causeWhy),
                d.text,
            };
            for (const QString &f : fields) row += QLatin1Char('\t') + f;
            diag::write(row);
        }
    }

    if (m_unsafeRaw) {
        // A statement kept its code and that code reads a local nothing
        // declares any more. Nothing here is safe to use.
        LowerResult out;
        out.notes = m_res.notes;
        out.notes.append(QStringLiteral("Part of this block could not be read, and what "
                                        "was left reads a local the rest of it would have "
                                        "replaced with a wire."));
        return out;
    }
    m_res.entryNode = c.entry;
    m_res.exitNode = c.tailNode;
    m_res.endTrivia = c.endTrivia;
    pruneSequences();
    return m_res;
}

// The pin a successor chains from. Loops carry on after they finish, a Sequence
// has a spare output, and a Branch has neither.
QString continuationPin(const Graph &g, const QString &nodeId)
{
    const GraphNode *n = g.node(nodeId);
    if (!n) return {};
    if (n->ref == bi::Branch || n->ref == bi::Cast || n->ref == bi::Return) return {};
    if (n->ref == bi::ForLoop || n->ref == bi::ForEach || n->ref == bi::While)
        return QStringLiteral("done");
    if (n->ref == bi::Sequence) {
        for (const QString &pin : {QStringLiteral("then1"), QStringLiteral("then2")})
            if (!edgeFrom(g, nodeId, pin)) return pin;
        return {};
    }
    return QStringLiteral("exec");
}

// The exec node that runs immediately before this one, or null at the top of
// the chain. Only exec wires say what runs first; a value wire does not.
const GraphNode *execPredecessor(const Graph &g, const GraphNode &n)
{
    for (const GraphEdge &e : g.edges) {
        if (e.to.node != n.id || e.to.pin != QLatin1String("exec")) continue;
        if (const GraphNode *from = g.node(e.from.node)) return from;
    }
    return nullptr;
}

// Locals a raw node earlier in the same chain declared. They are still in
// scope here and still spelled the same way in the generated file, so a call
// on one of them can resolve against its declared type.
void collectPriorLocals(const Graph &g, const QString &nodeId,
                        QHash<QString, QString> *out)
{
    QStringList blocks;
    QSet<QString> seen;
    const GraphNode *cur = g.node(nodeId);
    while (cur && !seen.contains(cur->id)) {
        seen.insert(cur->id);
        const GraphNode *prev = execPredecessor(g, *cur);
        if (!prev) break;
        if (prev->ref == bi::Raw)
            blocks.prepend(prev->opts.value(QStringLiteral("code")));
        cur = prev;
    }
    for (const QString &code : blocks) {
        if (code.trimmed().isEmpty()) continue;
        const ParseResult parsed = parseEnforceBody(code);
        if (!parsed.errors.isEmpty()) continue;
        // Only the top level: a local declared inside a block of that code
        // went out of scope again before this node runs.
        for (const StmtPtr &s : parsed.statements) {
            if (!s || s->kind != StmtKind::VarDecl) continue;
            for (const Declarator &d : s->decls)
                if (!d.name.isEmpty()) out->insert(d.name, s->typeName);
        }
    }
}

// Code in raw nodes that can still run after this one. A local declared here
// and read there only exists because both are text; wiring it straight through
// would delete the name the later node is written against.
QString laterRawCode(const Graph &g, const QString &nodeId)
{
    QSet<QString> reach;
    QStringList queue;
    queue << nodeId;
    reach.insert(nodeId);
    while (!queue.isEmpty()) {
        const QString cur = queue.takeFirst();
        for (const GraphEdge &e : g.edges) {
            if (e.from.node != cur || reach.contains(e.to.node)) continue;
            if (!g.node(e.to.node)) continue;
            reach.insert(e.to.node);
            queue << e.to.node;
        }
    }
    // Whatever feeds a node that runs later is part of what runs later. A name
    // can sit in a Raw Expression or typed into a pin, not only in a raw block.
    for (bool grew = true; grew;) {
        grew = false;
        for (const GraphEdge &e : g.edges) {
            if (!reach.contains(e.to.node) || reach.contains(e.from.node)) continue;
            if (!g.node(e.from.node)) continue;
            reach.insert(e.from.node);
            grew = true;
        }
    }
    reach.remove(nodeId);

    QStringList out;
    for (const GraphNode &n : g.nodes) {
        if (!reach.contains(n.id)) continue;
        const QString code = n.opts.value(QStringLiteral("code"));
        if (!code.isEmpty()) out << code;
        for (auto it = n.inputs.constBegin(); it != n.inputs.constEnd(); ++it)
            if (!it.value().isEmpty()) out << it.value();
    }
    return out.join(QLatin1Char('\n'));
}

// Locals this block declares that something downstream still reads. Their
// declarations have to stay as code, because the name is what the later block
// is written against; everything else here can still become nodes.
void sharedLocals(const Graph &g, const QString &nodeId, const QString &code,
                  QHash<QString, QString> *out)
{
    const QString later = laterRawCode(g, nodeId);
    if (later.isEmpty()) return;
    const ParseResult parsed = parseEnforceBody(code);
    if (!parsed.errors.isEmpty()) return;
    const EnforceScan scan = scanEnforce(later);
    for (const StmtPtr &s : parsed.statements) {
        if (!s || s->kind != StmtKind::VarDecl) continue;
        for (const Declarator &d : s->decls) {
            if (d.name.isEmpty() || !scan.identifiers.contains(d.name)) continue;
            out->insert(d.name, s->typeName);
        }
    }
}

// The event or function the raw node runs inside, so its parameters are names
// the lowered statements can use.
const GraphNode *enclosingEntry(const Graph &g, const QString &nodeId)
{
    QSet<QString> seen;
    const GraphNode *cur = g.node(nodeId);
    while (cur && !seen.contains(cur->id)) {
        seen.insert(cur->id);
        const GraphNode *prev = nullptr;
        for (const GraphEdge &e : g.edges) {
            if (e.to.node != cur->id) continue;
            const GraphNode *from = g.node(e.from.node);
            if (!from) continue;
            // Only exec wires say what runs before this; a value wire does not.
            if (e.to.pin != QLatin1String("exec")) continue;
            prev = from;
            break;
        }
        if (!prev) return cur;
        cur = prev;
    }
    return cur;
}

} // namespace

LowerResult lowerToNodes(const std::vector<StmtPtr> &statements, const Catalog &cat,
                         const Builtins &builtins, const Graph &graph,
                         const Project &project, const LowerOptions &opts)
{
    Lowerer lowerer(cat, builtins, graph, project, opts);
    return lowerer.run(statements);
}

LowerResult lowerEnforceCode(const QString &code, const Catalog &cat,
                             const Builtins &builtins, const Graph &graph,
                             const Project &project, const LowerOptions &opts)
{
    const ParseResult parsed = parseEnforceBody(code);
    if (!parsed.errors.isEmpty()) {
        // Unbalanced braces and the like: the text is the only thing that still
        // means what the author wrote, so nothing is lowered.
        LowerResult out;
        out.notes = parsed.errors;
        return out;
    }
    // The text is right here, so a "convert to nodes" on a raw block keeps the
    // author's blank lines and comments instead of quietly dropping them.
    LowerOptions withText = opts;
    if (withText.sourceText.isEmpty()) withText.sourceText = code;
    LowerResult out = lowerToNodes(parsed.statements, cat, builtins, graph, project, withText);
    out.notes = parsed.notes + out.notes;
    return out;
}

bool explodeRawNode(Graph &graph, const QString &nodeId, const Catalog &cat,
                    const Builtins &builtins, const Project &project, QStringList *notes)
{
    const GraphNode *raw = graph.node(nodeId);
    if (!raw || raw->ref != bi::Raw) return false;
    const QString code = raw->opts.value(QStringLiteral("code"));
    if (code.trimmed().isEmpty()) return false;

    // What this graph says before anything is touched. The importer decides
    // between text and nodes by generating both and comparing them, and this
    // path needs the same gate for a stronger reason: it writes into the graph
    // the user is about to save over their own mod, so anything the lowering
    // cannot carry is a line taken out of a file they still have to ship.
    const Graph asItStands = graph;
    const QString wasCode = generateEnforce(graph, cat, builtins, project).code;

    // Read off the node before anything is appended to the graph: this node is
    // about to be removed, and its opts go with it.
    const QString ownBefore = raw->opts.value(nodefmt::keyBefore());
    const QString ownTrailing = raw->opts.value(nodefmt::keyTrailing());

    const double originX = raw->x;
    const double originY = raw->y;

    LowerOptions opts;
    opts.selfClass = graph.modded ? graph.className : graph.baseClass;
    opts.originX = originX;
    opts.originY = originY;

    collectPriorLocals(graph, nodeId, &opts.knownLocals);
    // A local this block declares and a later one reads has to keep its name,
    // so its declaration stays as code while the rest of the block converts.
    sharedLocals(graph, nodeId, code, &opts.knownLocals);

    // Parameters of the enclosing event are in scope inside the raw code, and
    // the pins they arrive on are what the lowered nodes should read instead.
    QString entryId;
    QHash<QString, QString> pinOfParam;
    if (const GraphNode *entry = enclosingEntry(graph, nodeId)) {
        if (entry->id != nodeId) {
            entryId = entry->id;
            QVector<GraphParam> params;
            if (entry->ref == bi::Begin) {
                const LifecycleSig sig = builtins.beginMode(
                    entry->opts.value(QStringLiteral("when"), QStringLiteral("init")));
                params = sig.params;
            } else if (entry->ref == bi::End) {
                params = builtins.beginMode(QStringLiteral("end")).params;
            } else if (entry->ref.startsWith(QLatin1String("fn.entry."))) {
                const EntryTarget target = resolveEntry(entry->ref, project);
                if (target.valid)
                    for (const GraphParam &p : target.fn->params) params.append(p);
            } else {
                const MethodSig sig = cat.method(entry->ref);
                if (sig.valid && (sig.flags & flag::Event))
                    for (const MethodSig::Param &p : sig.params)
                        params.append({p.name, p.type});
            }
            for (int i = 0; i < params.size(); ++i) {
                opts.knownLocals.insert(params.at(i).name, params.at(i).type);
                pinOfParam.insert(params.at(i).name, QStringLiteral("o%1").arg(i));
            }
            if (entry->ref == bi::End && !params.isEmpty())
                pinOfParam.insert(params.first().name, QStringLiteral("parent"));
        }
    }

    const LowerResult r = lowerEnforceCode(code, cat, builtins, graph, project, opts);
    if (notes) *notes += r.notes;
    if (r.nodes.isEmpty() || r.entryNode.isEmpty() || r.statementsLowered == 0) {
        if (notes && r.notes.isEmpty())
            *notes << QStringLiteral("Nothing here could be read as nodes, so the code is "
                                     "left as it is.");
        return false;
    }

    // A blank line or a comment under the last statement belongs to the brace
    // that closes the block, and this node does not own that brace. Converting
    // anyway would take the author's own words out of their own mod, so the
    // block keeps its code instead.
    if (!r.endTrivia.isEmpty()) {
        if (notes)
            *notes << QStringLiteral("What is written under the last line here has no "
                                     "statement to belong to, so the code is left as it is.");
        return false;
    }

    // A comment at the end of this node's own line. It goes back at the end of
    // the last line whatever replaces this node writes, and no node has a field
    // that says that: trivia.trailing lands on a node's first line, which is a
    // different line as soon as the block becomes more than one. Rather than
    // move the author's words to a line they did not write them on, the block
    // keeps its code.
    if (!ownTrailing.isEmpty()) {
        if (notes)
            *notes << QStringLiteral("The comment at the end of this line has no single "
                                     "statement to sit behind once this is more than one "
                                     "node, so the code is left as it is.");
        return false;
    }

    int real = 0;
    for (const GraphNode &n : r.nodes)
        if (n.ref != bi::Raw) real++;
    // One raw node in place of one raw node is not a better graph.
    if (real == 0) {
        if (notes)
            *notes << QStringLiteral("Nothing here became a node, so the code is left as "
                                     "it is.");
        return false;
    }

    QVector<EdgeEnd> preds;
    for (const GraphEdge &e : graph.edges)
        if (e.to.node == nodeId && e.to.pin == QLatin1String("exec")) preds.append(e.from);
    EdgeEnd succ;
    if (const GraphEdge *e = edgeFrom(graph, nodeId, QStringLiteral("exec"))) succ = e->to;

    removeNode(graph, nodeId);

    QSet<QString> fresh;
    for (const GraphNode &n : r.nodes) {
        graph.nodes.append(n);
        fresh.insert(n.id);
    }
    for (const GraphEdge &e : r.edges) graph.edges.append(e);
    for (const GraphVariable &v : r.variables) graph.variables.append(v);

    QString head = r.entryNode;
    QString tailNode = r.exitNode;
    QString tailPin = continuationPin(graph, tailNode);

    if (!succ.node.isEmpty() && tailPin.isEmpty()) {
        // The chain ends on a Branch, which has nothing to carry on from, so
        // the two halves run out of a Sequence instead.
        GraphNode seq;
        seq.id = nextId(QStringLiteral("n"));
        seq.kind = NodeKind::Builtin;
        seq.ref = bi::Sequence;
        seq.x = originX;
        seq.y = originY;
        graph.nodes.append(seq);
        fresh.insert(seq.id);
        graph.edges.append({nextId(QStringLiteral("e")),
                            {seq.id, QStringLiteral("then0")},
                            {head, QStringLiteral("exec")}, {}});
        head = seq.id;
        tailNode = seq.id;
        tailPin = QStringLiteral("then1");
    }

    for (const EdgeEnd &p : preds)
        graph.edges.append({nextId(QStringLiteral("e")), p,
                            {head, QStringLiteral("exec")}, {}});
    if (!succ.node.isEmpty() && !tailPin.isEmpty())
        graph.edges.append({nextId(QStringLiteral("e")), {tailNode, tailPin}, succ, {}});

    // A parameter was written as its own name because nothing else could say
    // it; now that the event node is known, it becomes a wire from its pin.
    if (!entryId.isEmpty()) {
        for (int i = graph.nodes.size() - 1; i >= 0; --i) {
            const GraphNode &n = graph.nodes.at(i);
            if (!fresh.contains(n.id) || n.ref != IdRawExpr) continue;
            const QString pin = pinOfParam.value(n.opts.value(QStringLiteral("code")));
            if (pin.isEmpty()) continue;
            const QString id = n.id;
            for (GraphEdge &e : graph.edges)
                if (e.from.node == id) e.from = {entryId, pin};
            fresh.remove(id);
            graph.nodes.removeAt(i);
        }
        for (int i = graph.edges.size() - 1; i >= 0; --i)
            if (!graph.node(graph.edges.at(i).from.node)
                || !graph.node(graph.edges.at(i).to.node))
                graph.edges.removeAt(i);
    }

    // The lines the author wrote above this node. `head` is what now runs in
    // its place, so those lines go on it, above anything the block held of its
    // own. Done last because the rewiring above is what settles which node that
    // is, and because removeNode has already taken the old one away.
    if (!ownBefore.isEmpty()) {
        if (GraphNode *n = graph.node(head))
            n->opts.insert(nodefmt::keyBefore(),
                           ownBefore + n->opts.value(nodefmt::keyBefore()));
    }

    // Line endings need no answer here. A raw node built by the importer holds
    // bare newlines, because the file's ending was taken off once on the way in
    // and goes back on once on the way out, so what this writes lands in the
    // same file the raw node's text would have. A raw node holding carriage
    // returns from somewhere else is a block the gate below turns down, which
    // is the right outcome: those bytes belong to no file this graph knows.

    // The gate. Nothing above here is allowed to stand unless the file the
    // graph now generates is the file it generated a moment ago, character for
    // character. Refusing costs a conversion; converting wrongly costs the
    // author a comment out of their own mod, which is not recoverable from the
    // graph afterwards.
    if (generateEnforce(graph, cat, builtins, project).code != wasCode) {
        graph = asItStands;
        if (notes)
            *notes << QStringLiteral("Converting this would not write the same code back, so "
                                     "it is left as it is.");
        return false;
    }

    layoutNodes(graph, fresh, {originX, originY});
    return true;
}
