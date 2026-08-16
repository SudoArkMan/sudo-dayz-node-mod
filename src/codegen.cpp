#include "codegen.h"

#include "scriptapi.h"
#include "templates.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

// What gets written into the .c file. Plain ASCII on purpose: this line ends
// up inside generated DayZ source, and the config parser is not UTF-8 safe, so
// a stray dash reports its error on the wrong line.
const QString USER_BEGIN =
    QStringLiteral("// >>> user code, kept when the graph regenerates");
const QString USER_END = QStringLiteral("// <<< user code");

// What the Electron build wrote. Files it generated are still out there, and
// their user regions have to survive a regeneration here, so reading accepts
// the old spelling even though writing no longer produces it. The dash in the
// literal below is part of that old text and has to stay exactly as it is.
const QString USER_BEGIN_LEGACY =
    QStringLiteral("// >>> user code — kept when the graph regenerates");

namespace {

QString ind(int n) { return QString(n, QLatin1Char('\t')); }

const QChar Tab = QLatin1Char('\t');

// Generated statements plus the node behind each one.
//
// Ownership has to be recorded while the text is built, not worked out from
// block sizes afterwards: a node's own lines sit around its children's, since a
// Branch writes `if (...)`, `{`, the sub-chain, then `}`. The two vectors are
// always the same length, and an empty owner means the line belongs to no node.
struct Emitted {
    QStringList lines;
    QVector<QString> owners;

    void add(const QString &line, const QString &owner = QString())
    {
        lines.append(line);
        owners.append(owner);
    }
    void add(const Emitted &other)
    {
        lines += other.lines;
        owners += other.owners;
    }
    int size() const { return lines.size(); }
    bool isEmpty() const { return lines.isEmpty(); }
};

// Lines a node writes for itself. A nested chain arrives already tagged with
// its own nodes, so the innermost owner wins wherever the two meet.
Emitted ownedBy(const QString &owner, const QStringList &lines)
{
    Emitted e;
    for (const QString &l : lines) e.add(l, owner);
    return e;
}

// Split any entry that carries an embedded newline, keeping its owner on every
// piece. Done once on the finished file rather than inside add(), so the line
// budget in emitChain still counts what it counted before.
void flatten(Emitted &e)
{
    Emitted out;
    for (int i = 0; i < e.lines.size(); ++i) {
        const QString &line = e.lines.at(i);
        if (!line.contains(QLatin1Char('\n'))) {
            out.add(line, e.owners.at(i));
            continue;
        }
        for (const QString &part : line.split(QLatin1Char('\n'))) out.add(part, e.owners.at(i));
    }
    e = out;
}

// Recursion and size limits. A graph is user data, so every walk over it has to
// terminate on a malformed one: a data cycle, an exec cycle and plain nesting
// depth all used to run the stack out and take the whole app down with the
// unsaved project. 64 nesting levels matches the analyser's own cutoff; the node
// and line budgets are far past any hand-built graph but bound the fan-out where
// one Sequence drives the same chain from all three outputs, which is
// exponential in the size of the text however cheap the walk is made.
constexpr int kMaxEmitDepth = 64;
constexpr int kMaxExprDepth = 64;
constexpr int kMaxEmittedNodes = 20000;
constexpr int kMaxEmittedLines = 200000;

struct Ctx {
    const Catalog *cat = nullptr;
    const Builtins *builtins = nullptr;
    const Graph *graph = nullptr;
    // The whole project, so cross-script calls resolve.
    const Project *project = nullptr;
    QStringList warnings;
    // "nodeId:pinId" -> expression referencing an already-emitted value.
    QHash<QString, QString> temps;
    int tempN = 0;
    // Nodes whose output cannot exist: a refused construction, for instance.
    // Statements that consume one are commented out instead of emitted, so the
    // generator never produces a plausible-looking null dereference.
    QSet<QString> poisoned;

    // The class this script really is, for every ancestry question: a modded
    // class reopens an existing one, a plain class only inherits from its base.
    QString self;
    // Return type of the method being emitted, so an early-out returns a value
    // when the enclosing method has one.
    QString retType = QStringLiteral("void");

    // Cycle, depth and runaway guards.
    QSet<QString> evaluating; // "node:pin" data pins currently being resolved
    QSet<QString> emitting;   // nodes currently on the emit stack
    int emitDepth = 0;
    int emitted = 0; // statements walked
    int lines = 0;   // statements written, counted again per level they pass
    bool aborted = false;
    // Sub-chains that are safe to re-use verbatim, keyed by target node:depth.
    QHash<QString, Emitted> chainCache;
    // Bumped whenever an expression reads a temporary, so the cache can tell a
    // chain whose text depends on the surrounding locals from one that does not.
    int tempReads = 0;
    // Diagnostics that must be reported once rather than once per traversal.
    QSet<QString> noted;
};

QString tempKey(const QString &nodeId, const QString &pinId)
{
    return nodeId + QLatin1Char(':') + pinId;
}

// A warning that a cycle or a depth cut would otherwise repeat on every pass.
void note(Ctx &ctx, const QString &key, const QString &text)
{
    if (ctx.noted.contains(key)) return;
    ctx.noted.insert(key);
    ctx.warnings.append(text);
}

// A graph can be wired so that the same tail is reachable along an exponential
// number of paths: three Sequence outputs on one node triple the rest of the
// graph, and 17 of those was 2.5 GB of text and a dead process. Once this
// trips, every frame still on the stack unwinds without emitting anything more.
void abortGeneration(Ctx &ctx)
{
    ctx.aborted = true;
    note(ctx, QStringLiteral("budget"),
         QStringLiteral("This graph generates more code than the file can hold, so generation "
                        "stopped early. Look for a Sequence node whose outputs all drive the "
                        "same chain. Each one repeats everything downstream."));
}

std::function<bool(const QString &)> isEnumOf(const Ctx &ctx)
{
    const Catalog *cat = ctx.cat;
    return [cat](const QString &n) { return cat && cat->isEnum(n); };
}

// An empty `returns` is void: graphFromJson cannot tell an absent key from an
// empty string, and scriptapi.cpp (which shapes the pins the canvas draws)
// normalises it the same way.
QString returnTypeOf(const GraphFunction &f)
{
    return f.returns.isEmpty() ? QStringLiteral("void") : f.returns;
}

bool returnsValue(const GraphFunction &f)
{
    return returnTypeOf(f) != QLatin1String("void");
}

// Anything descending from `Object` is an engine entity. `new` gives you a
// script shell with no engine object behind it. The calls then hit a phantom.
// Entities only come from the engine, so this is refused rather than emitted.
bool checkConstructible(Ctx &ctx, const QString &cls)
{
    if (cls.isEmpty() || ctx.cat->classId(cls) < 0) return true; // unknown type: leave it alone
    if (ctx.cat->isA(cls, QStringLiteral("Object"))) {
        ctx.warnings.append(
            cls + QStringLiteral(" is an entity (it descends from Object) and cannot be created "
                                 "with `new`: you would get a script object with no engine entity "
                                 "behind it. Use the Spawn Entity node (GetGame().CreateObjectEx), "
                                 "or take a reference to one that already exists."));
        return false;
    }
    return true;
}

// Managed types are reference-counted, so a local holding one needs `ref`.
// Entities are the exception: they are owned by the engine, and a strong
// reference to one keeps the script object alive past Object::Delete, leaving
// a member that is not null and no longer backed by anything. Vanilla holds
// entity handles unref'd everywhere, so the inference has to stop at Object.
bool isManaged(const Ctx &ctx, const QString &cls)
{
    if (ctx.cat->classId(cls) < 0) return false;
    return ctx.cat->isA(cls, QStringLiteral("Managed"))
           && !ctx.cat->isA(cls, QStringLiteral("Object"));
}

// Exec output ids, for edges whose pin no longer resolves against a def. A
// file can carry an edge the current node shape has no pin for.
bool isExecPinId(const QString &id)
{
    static const QStringList ids = {
        QStringLiteral("exec"),  QStringLiteral("then0"), QStringLiteral("then1"),
        QStringLiteral("then2"), QStringLiteral("true"),  QStringLiteral("false"),
        QStringLiteral("body"),  QStringLiteral("done"),  QStringLiteral("success"),
        QStringLiteral("failed"),
    };
    return ids.contains(id);
}

// Enforce type without the storage modifiers, for catalogue lookups.
QString bareType(const QString &t)
{
    static const QRegularExpression modifiers(
        QStringLiteral("\\b(ref|autoptr|notnull|const|owned|local)\\b\\s*"));
    QString out = t;
    out.remove(modifiers);
    return out.trimmed();
}

// Containers are the types an `out` parameter is expected to arrive already
// allocated: the callee fills the instance you pass rather than assigning one.
bool isContainerType(const QString &t)
{
    static const QRegularExpression generic(
        QStringLiteral("^(array|set|map|multiMap)\\s*<.+>$"));
    static const QStringList aliases = {
        QStringLiteral("TStringArray"), QStringLiteral("TIntArray"),
        QStringLiteral("TFloatArray"),  QStringLiteral("TVectorArray"),
        QStringLiteral("TClassArray"),  QStringLiteral("TTypenameArray"),
        QStringLiteral("TBoolArray"),
    };
    const QString b = bareType(t);
    return generic.match(b).hasMatch() || aliases.contains(b);
}

// Literal an early `return` uses when the enclosing method has a value type.
QString defaultForType(const Ctx &ctx, const QString &type)
{
    if (type.isEmpty() || type == QLatin1String("void")) return QString();
    return defaultLiteral(pinTypeOf(type, isEnumOf(ctx)));
}

// The class the generated script is, as far as the catalogue is concerned. A
// modded class reopens an existing one; a plain class only has its base. Empty
// when the script is its own root, and unknown to the catalogue when the base
// is another script in this project. In both of those cases nothing can be
// proved about the ancestry, so callers must treat it as "cannot tell".
QString effectiveClassOf(const Graph &g)
{
    return g.modded ? g.className : g.baseClass;
}

bool selfKnown(const Ctx &ctx)
{
    return !ctx.self.isEmpty() && ctx.cat->classId(ctx.self) >= 0;
}

// The catalogue entry the user placed may be another class's declaration of the
// same hook, so a name match anywhere in the ancestry still counts as an
// override. Only worth the scan once the owner check has already failed.
bool ancestryDeclares(const Ctx &ctx, const QString &name)
{
    if (!selfKnown(ctx) || name.isEmpty()) return false;
    SearchOptions opts;
    opts.limit = 40;
    opts.ofClass = ctx.self;
    for (const SearchHit &h : ctx.cat->search(name, opts))
        if (h.title == name || h.title == QStringLiteral("Event ") + name) return true;
    return false;
}

// Whether `override` and `super.` are legal for this method here. A class with
// no base has nothing to override; a base that is another script in this
// project cannot be checked, so the benefit of the doubt goes to the author.
bool inheritsMethod(const Ctx &ctx, const MethodSig &m)
{
    if (ctx.self.isEmpty()) return false;
    if (!selfKnown(ctx)) return true;
    if (!m.owner.isEmpty() && ctx.cat->isA(ctx.self, m.owner)) return true;
    return ancestryDeclares(ctx, m.name);
}

// ------------------------------------------------------------------ lifecycle

// Begin's four modes come from Builtins; End has no accessor there, so its
// signature is mirrored from the Electron build's END_SIG. Both feed the
// override that gets emitted, so a change on either side has to be matched.
LifecycleSig endSignature()
{
    LifecycleSig sig;
    sig.method = QStringLiteral("EEDelete");
    sig.ret = QStringLiteral("void");
    sig.params.append({QStringLiteral("parent"), QStringLiteral("EntityAI")});
    sig.label = QStringLiteral("On Destroy: EEDelete()");
    return sig;
}

// Lifecycle builtins (Begin / End) are events too. They just carry their
// signature here rather than in the catalogue.
bool lifecycleSig(const Ctx &ctx, const GraphNode &n, LifecycleSig *out)
{
    if (n.ref == QLatin1String("bi.begin")) {
        const QString when = n.opts.value(QStringLiteral("when"), QStringLiteral("init"));
        LifecycleSig sig = ctx.builtins->beginMode(when);
        if (sig.method.isEmpty() && !sig.ctor)
            sig = ctx.builtins->beginMode(QStringLiteral("init"));
        *out = sig;
        return true;
    }
    if (n.ref == QLatin1String("bi.end")) {
        *out = endSignature();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ statements

NodeDef defOf(Ctx &ctx, const GraphNode &node);
Emitted emitNode(Ctx &ctx, const GraphNode &node, int depth);
QString expr(Ctx &ctx, const GraphNode &node, const QString &pinId);

// `expr`, bracketed when the value is built out of an operator loose enough
// that the brackets matter. `outer` is the binding strength of whatever the
// value is going into; 11 is tighter than every operator, which is what a
// target, an index or a `!` needs.
QString operandExpr(Ctx &ctx, const GraphNode &node, const QString &pinId, int outer,
                    bool rightHandSide);

// Variable nodes carry the id, either bare or behind "var.get." / "var.set.".
// Resolution is an EXACT id match in one shared helper: matching on a suffix
// binds the wrong member as soon as one id ends with another.
const GraphVariable *ownVariable(const Ctx &ctx, const GraphNode &node)
{
    return variableForRef(*ctx.graph, node.ref);
}

// Cast and New were authored against either spelling of the target class, and
// the builtins layer already accepts both. Reading only one silently builds
// the wrong type.
QString castClassOf(const GraphNode &node)
{
    const QString cls = node.opts.value(QStringLiteral("cls"));
    return cls.isEmpty() ? node.opts.value(QStringLiteral("class")) : cls;
}

NodeDef defOf(Ctx &ctx, const GraphNode &node)
{
    // Templates are placed with kind Builtin, so they have to resolve before
    // the builtin lookup, which knows nothing outside its own table.
    if (isTemplateKey(node.ref)) {
        const NodeTemplate t = findTemplate(node.ref);
        return t.valid ? templateDef(t, isEnumOf(ctx)) : NodeDef{};
    }
    if (node.kind == NodeKind::VarGet || node.kind == NodeKind::VarSet) {
        const GraphVariable *v = ownVariable(ctx, node);
        if (!v) return {};
        return ctx.builtins->variableDef(*v, node.kind == NodeKind::VarSet, *ctx.cat);
    }
    if (node.kind == NodeKind::Builtin) {
        if (!ctx.builtins->contains(node.ref)) return {};
        return ctx.builtins->defForNode(node, *ctx.cat);
    }
    if (ctx.project) {
        const NodeDef fromProject = scriptDefFor(node.ref, *ctx.project, isEnumOf(ctx));
        if (fromProject.valid) return fromProject;
    }
    return ctx.cat->defFor(node.ref);
}

Emitted emitChain(Ctx &ctx, const QVector<const GraphNode *> &chain, int depth)
{
    Emitted out;
    for (const GraphNode *n : chain) {
        if (ctx.aborted) break;
        // An exec cycle reaches a node that is still being emitted further up
        // the stack. execChain only cuts cycles inside one walk and never
        // contains the node it started from, so the cut has to happen here.
        if (ctx.emitting.contains(n->id)) {
            const NodeDef d = defOf(ctx, *n);
            const QString title = d.valid ? d.title : n->ref;
            note(ctx, QStringLiteral("cycle:") + n->id,
                 QLatin1Char('"') + title
                     + QStringLiteral("\" is part of an execution loop: the chain runs back "
                                      "into a node that is already running. The loop is cut in "
                                      "the generated code; use For Loop or While instead."));
            out.add(ind(depth) + QStringLiteral("// cycle cut: ") + title
                        + QStringLiteral(" already runs above this point"),
                    n->id);
            break;
        }
        ctx.emitting.insert(n->id);
        const Emitted produced = emitNode(ctx, *n, depth);
        ctx.emitting.remove(n->id);
        out.add(produced);
        // Counted per level so the cost of copying a sub-chain into each of its
        // parents is what the budget actually measures.
        ctx.lines += produced.size();
        if (ctx.lines > kMaxEmittedLines && !ctx.aborted) {
            abortGeneration(ctx);
            break;
        }
    }
    return out;
}

Emitted subChain(Ctx &ctx, const GraphNode &node, const QString &pin, int depth)
{
    if (ctx.aborted) return {};
    const GraphEdge *e = edgeFrom(*ctx.graph, node.id, pin);
    if (!e) return {};
    // The chain is decided by the node the edge lands on, not by which pin drove
    // it, so two outputs pointing at the same node share one result. Indentation
    // is baked into the text, so the depth is part of the identity.
    const QString key = e->to.node + QLatin1Char(':') + QString::number(depth);
    const auto hit = ctx.chainCache.constFind(key);
    if (hit != ctx.chainCache.constEnd()) return *hit;

    if (ctx.emitDepth >= kMaxEmitDepth) {
        note(ctx, QStringLiteral("depth"),
             QStringLiteral("The graph nests more than %1 levels deep, which the generator "
                            "cannot walk. Split the deepest part into a function and call it.")
                 .arg(kMaxEmitDepth));
        return ownedBy(node.id, {ind(depth)
                                 + QStringLiteral("// nesting is too deep to generate "
                                                  "(see Warnings)")});
    }

    ctx.emitDepth++;
    const int tempsBefore = ctx.tempN;
    const int readsBefore = ctx.tempReads;
    const int poisonedBefore = ctx.poisoned.size();
    const Emitted out = emitChain(ctx, execChain(*ctx.graph, node.id, pin), depth);
    ctx.emitDepth--;

    // Re-using a sub-chain is only safe when emitting it declared nothing: a
    // chain that allocated a temporary would declare it twice in the same
    // scope, and one that read a temporary depends on which loop it sits in.
    // That still covers the case this exists for: a Sequence whose outputs all
    // drive the same chain, which used to walk the tail once per output.
    if (!ctx.aborted && ctx.tempN == tempsBefore && ctx.tempReads == readsBefore
        && ctx.poisoned.size() == poisonedBefore)
        ctx.chainCache.insert(key, out);
    return out;
}

// A loop variable name carried in from an imported file. Anything that is not
// one plain identifier is not a name, and the generator invents its own.
QString carriedName(const GraphNode &node, const QString &key)
{
    const QString name = node.opts.value(key).trimmed();
    if (name.isEmpty()) return {};
    if (!name.at(0).isLetter() && name.at(0) != QLatin1Char('_')) return {};
    for (const QChar c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) return {};
    return name;
}

QString arrayElementType(Ctx &ctx, const GraphNode &node, const QString &pinId)
{
    const GraphEdge *e = edgeInto(*ctx.graph, node.id, pinId);
    if (!e) return QStringLiteral("auto");
    const GraphNode *src = ctx.graph->node(e->from.node);
    if (!src) return QStringLiteral("auto");
    MethodSig m = ctx.cat->method(src->ref);
    if (!m.valid) m = ctx.cat->globalFn(src->ref);
    if (!m.valid) return QStringLiteral("auto");
    const auto isEnumFn = [&ctx](const QString &n) { return ctx.cat->isEnum(n); };
    const PinType t = pinTypeOf(m.ret, isEnumFn);
    if (!t.cls.isEmpty()) return t.cls;
    return t.kind == PinKind::Any ? QStringLiteral("auto") : pinKindName(t.kind);
}

struct Call {
    QString text;
    QStringList pre;
    bool valid = false;
    // The method needs a target this class cannot supply; emitting it would be
    // a compile error in DayZ, so the caller comments it out instead.
    bool badTarget = false;
};

// Resolve the object a non-static call runs against.
//
// An unwired `target` used to become `this` unconditionally, which produces
// `IsServer()` for a CGame method (it has to be `GetGame().IsServer()`) and
// `GetStomach()` for a PlayerBase method dropped into an item. Both are compile
// errors that only show up when the mod is built.
QString callTarget(Ctx &ctx, const GraphNode &node, const MethodSig &sig, const NodeDef &def,
                   bool *bad)
{
    if (edgeInto(*ctx.graph, node.id, QStringLiteral("target")))
        return operandExpr(ctx, node, QStringLiteral("target"), 11, false);
    // The game singleton is reachable from anywhere, but only through GetGame().
    if (sig.owner == QLatin1String("CGame")) return QStringLiteral("GetGame()");
    // Nothing can be proved when the base class is another script in this
    // project or the class is its own root, so the old default stands.
    if (sig.owner.isEmpty() || !selfKnown(ctx)) return QStringLiteral("this");
    if (ctx.cat->isA(ctx.self, sig.owner)) return QStringLiteral("this");
    *bad = true;
    note(ctx, QStringLiteral("target:") + node.id,
         QLatin1Char('"') + (def.valid ? def.title : sig.name)
             + QStringLiteral("\" is declared on ") + sig.owner + QStringLiteral(", and ")
             + ctx.self
             + QStringLiteral(" does not descend from it, so there is nothing for the call to "
                              "run against. Wire its target pin to a ")
             + sig.owner + QStringLiteral("."));
    return QStringLiteral("this");
}

Call callExpression(Ctx &ctx, const GraphNode &node)
{
    Call call;
    const MethodSig m = ctx.cat->method(node.ref);
    const MethodSig g = ctx.cat->globalFn(node.ref);
    if (!m.valid && !g.valid) return call;
    const MethodSig &sig = m.valid ? m : g;

    QStringList args;
    // Index of the last argument the user actually supplied.
    int lastSupplied = -1;
    for (int i = 0; i < sig.params.size(); ++i) {
        const MethodSig::Param &p = sig.params.at(i);
        const QString pinId = QStringLiteral("p%1").arg(i);
        const bool wired = edgeInto(*ctx.graph, node.id, pinId) != nullptr;
        const bool typed = node.inputs.contains(pinId) && !node.inputs.value(pinId).isEmpty();
        if (p.dir != 0 || wired || typed || p.def.isEmpty()) lastSupplied = i;
    }
    for (int i = 0; i < sig.params.size(); ++i) {
        const MethodSig::Param &p = sig.params.at(i);
        const QString pinId = QStringLiteral("p%1").arg(i);
        if (p.dir == 1) {
            // pure out param: declare a local and pass it. A container has to
            // be allocated first: the callee fills the instance it is handed,
            // so a bare declaration passes null and either crashes or silently
            // returns nothing. Vanilla allocates at every such call site.
            const QString v = QStringLiteral("out%1").arg(ctx.tempN++);
            const QString t = bareType(p.type);
            if (isContainerType(t))
                call.pre.append(QStringLiteral("ref ") + t + QLatin1Char(' ') + v
                                + QStringLiteral(" = new ") + t + QStringLiteral("();"));
            else
                call.pre.append(p.type + QLatin1Char(' ') + v + QLatin1Char(';'));
            ctx.temps.insert(tempKey(node.id, QStringLiteral("o%1").arg(i)), v);
            args << v;
            continue;
        }
        if (p.dir == 2) {
            const QString v = QStringLiteral("io%1").arg(ctx.tempN++);
            call.pre.append(p.type + QLatin1Char(' ') + v + QStringLiteral(" = ")
                            + expr(ctx, node, pinId) + QLatin1Char(';'));
            ctx.temps.insert(tempKey(node.id, QStringLiteral("o%1").arg(i)), v);
            args << v;
            continue;
        }
        // Trailing optional parameters the user never touched are omitted rather
        // than filled with a made-up literal. `new Timer()` is correct; the 0 that
        // a synthesised default produces is a wrong call category.
        if (i > lastSupplied && !p.def.isEmpty()) continue;
        args << expr(ctx, node, pinId);
    }

    const QString argList = args.join(QStringLiteral(", "));
    call.valid = true;

    if (g.valid) {
        call.text = sig.name + QLatin1Char('(') + argList + QLatin1Char(')');
        return call;
    }

    const bool isStatic = m.flags & flag::Static;
    const bool isCtor = m.flags & flag::Ctor;
    if (isCtor) {
        if (!checkConstructible(ctx, m.owner)) {
            call.text = QStringLiteral("null /* ") + m.owner + QStringLiteral(" is an entity */");
            return call;
        }
        call.text = QStringLiteral("new ") + m.owner + QLatin1Char('(') + argList + QLatin1Char(')');
        return call;
    }
    if (isStatic) {
        call.text = m.owner + QLatin1Char('.') + sig.name + QLatin1Char('(') + argList
                    + QLatin1Char(')');
        return call;
    }

    const QString target = callTarget(ctx, node, sig, ctx.cat->defFor(node.ref), &call.badTarget);
    call.text = target == QLatin1String("this")
                    ? sig.name + QLatin1Char('(') + argList + QLatin1Char(')')
                    : target + QLatin1Char('.') + sig.name + QLatin1Char('(') + argList
                          + QLatin1Char(')');
    return call;
}

// A call whose result is read once, by the statement that runs straight after
// it, needs no local of its own: the reader can say the call itself. Writing
// `int v0 = GetQuantity(); m_Count = v0;` for `m_Count = GetQuantity();` is
// correct, is not what anyone wrote, and is most of why an imported method came
// back spelled differently and had to keep its text.
//
// The reader has to be the very next node in the exec chain, or inlining would
// move the call past whatever runs between them.
bool readOnceByNext(Ctx &ctx, const GraphNode &node)
{
    const GraphEdge *succ = edgeFrom(*ctx.graph, node.id, QStringLiteral("exec"));
    if (!succ) return false;
    int reads = 0;
    QString reader;
    for (const GraphEdge &e : ctx.graph->edges) {
        if (e.from.node != node.id || e.from.pin != QLatin1String("ret")) continue;
        reads++;
        reader = e.to.node;
    }
    return reads == 1 && reader == succ->to.node;
}

Emitted emitNode(Ctx &ctx, const GraphNode &node, int depth)
{
    if (ctx.aborted) return {};
    // The line budget in emitChain measures the text; this one bounds the walk
    // itself, for a graph whose statements are cheap but reachable along an
    // exponential number of paths.
    if (++ctx.emitted > kMaxEmittedNodes) {
        abortGeneration(ctx);
        return {};
    }

    const QString pad = ind(depth);

    // If anything feeding this node could not be produced, emitting the call
    // would just be a null dereference dressed up as working code. Only values
    // carry the problem forward: an exec edge is running order, so the next
    // statement in the chain is not affected by what the previous one failed to
    // produce.
    for (const GraphEdge &e : ctx.graph->edges) {
        if (e.to.node != node.id || !ctx.poisoned.contains(e.from.node)) continue;
        const GraphNode *src = ctx.graph->node(e.from.node);
        const NodeDef srcDef = src ? defOf(ctx, *src) : NodeDef{};
        const Pin *srcPin = srcDef.valid ? srcDef.pin(e.from.pin, PinDir::Out) : nullptr;
        if (srcPin ? srcPin->type.kind == PinKind::Exec : isExecPinId(e.from.pin)) continue;
        ctx.poisoned.insert(node.id);
        const NodeDef d = defOf(ctx, node);
        return ownedBy(node.id,
                       {pad + QStringLiteral("// skipped: ") + (d.valid ? d.title : node.ref)
                        + QStringLiteral(" depends on a value that cannot be created")});
    }

    const NodeDef def = defOf(ctx, node);
    if (!def.valid) {
        ctx.warnings.append(QStringLiteral("Node %1 references an unknown entry (%2); skipped.")
                                .arg(node.id, node.ref));
        return ownedBy(node.id, {pad + QStringLiteral("// [unresolved node ") + node.ref
                                 + QLatin1Char(']')});
    }

    // ---- a template used as a statement
    if (isTemplateKey(node.ref)) {
        const NodeTemplate t = findTemplate(node.ref);
        if (!t.valid)
            return ownedBy(node.id, {pad + QStringLiteral("// [unknown template ") + node.ref
                                     + QLatin1Char(']')});
        const QString rendered =
            renderTemplate(t, [&](const QString &pinId) { return expr(ctx, node, pinId); });
        // A pure template dropped onto an exec chain still has to be a statement.
        const QString text = t.pure ? rendered + QLatin1Char(';') : rendered;
        QStringList lines;
        for (const QString &l : text.split(QLatin1Char('\n'))) lines << pad + l;
        return ownedBy(node.id, lines);
    }

    if (node.ref == QLatin1String("bi.branch")) {
        const QString cond = expr(ctx, node, QStringLiteral("cond"));
        const Emitted t = subChain(ctx, node, QStringLiteral("true"), depth + 1);
        const Emitted f = subChain(ctx, node, QStringLiteral("false"), depth + 1);
        Emitted lines;
        lines.add(pad + QStringLiteral("if (") + cond + QLatin1Char(')'), node.id);
        lines.add(pad + QLatin1Char('{'), node.id);
        lines.add(t);
        lines.add(pad + QLatin1Char('}'), node.id);
        if (!f.isEmpty()) {
            lines.add(pad + QStringLiteral("else"), node.id);
            lines.add(pad + QLatin1Char('{'), node.id);
            lines.add(f);
            lines.add(pad + QLatin1Char('}'), node.id);
        }
        return lines;
    }

    if (node.ref == QLatin1String("bi.sequence")) {
        const QStringList thens = {QStringLiteral("then0"), QStringLiteral("then1"),
                                   QStringLiteral("then2")};
        // Three outputs landing on the same node means that chain is emitted
        // three times, which is what the wiring says but almost never what was
        // meant. It is also how one mistake turns into an unusable file.
        QSet<QString> targets;
        for (const QString &p : thens) {
            const GraphEdge *e = edgeFrom(*ctx.graph, node.id, p);
            if (!e) continue;
            if (targets.contains(e->to.node))
                note(ctx, QStringLiteral("seqdup:") + node.id,
                     QStringLiteral("A Sequence node drives the same chain from more than one "
                                    "output, so that chain is generated once per output. Wire "
                                    "each output to a different node."));
            targets.insert(e->to.node);
        }
        Emitted lines;
        for (const QString &p : thens) lines.add(subChain(ctx, node, p, depth));
        return lines;
    }

    if (node.ref == QLatin1String("bi.forLoop")) {
        // The counter keeps the name the imported loop gave it, so a converted
        // method comes back reading the way it was written.
        QString v = carriedName(node, QStringLiteral("var"));
        if (v.isEmpty()) v = QStringLiteral("i%1").arg(ctx.tempN++);
        ctx.temps.insert(tempKey(node.id, QStringLiteral("index")), v);
        const Emitted body = subChain(ctx, node, QStringLiteral("body"), depth + 1);
        // Hoisted rather than concatenated inline: resolving a pin allocates
        // temporaries and pushes warnings, and C++ does not sequence the
        // operands of `+`, so the order would be the compiler's choice.
        const QString first = expr(ctx, node, QStringLiteral("first"));
        const QString last = expr(ctx, node, QStringLiteral("last"));
        Emitted lines;
        lines.add(pad + QStringLiteral("for (int ") + v + QStringLiteral(" = ") + first
                      + QStringLiteral("; ") + v + QStringLiteral(" < ") + last
                      + QStringLiteral("; ") + v + QStringLiteral("++)"),
                  node.id);
        lines.add(pad + QLatin1Char('{'), node.id);
        lines.add(body);
        lines.add(pad + QLatin1Char('}'), node.id);
        lines.add(subChain(ctx, node, QStringLiteral("done"), depth));
        return lines;
    }

    if (node.ref == QLatin1String("bi.forEach")) {
        QString item = carriedName(node, QStringLiteral("item"));
        if (item.isEmpty()) item = QStringLiteral("item%1").arg(ctx.tempN++);
        const QString arrExpr = expr(ctx, node, QStringLiteral("array"));
        // An element type set on the node is what the loop was written with, so
        // it beats anything derived from whatever feeds the array pin.
        QString elemType = node.opts.value(QStringLiteral("type"));
        if (elemType.isEmpty()) elemType = arrayElementType(ctx, node, QStringLiteral("array"));
        ctx.temps.insert(tempKey(node.id, QStringLiteral("item")), item);
        // The node offers an index, so it has to be bound: the two-variable
        // foreach is the Enforce form for it. Only declared when something
        // reads it, since an unused loop variable is noise.
        QString index;
        if (edgeFrom(*ctx.graph, node.id, QStringLiteral("index"))) {
            index = carriedName(node, QStringLiteral("idx"));
            if (index.isEmpty()) index = QStringLiteral("idx%1").arg(ctx.tempN++);
            ctx.temps.insert(tempKey(node.id, QStringLiteral("index")), index);
        }
        const Emitted body = subChain(ctx, node, QStringLiteral("body"), depth + 1);
        Emitted lines;
        lines.add(pad + QStringLiteral("foreach (")
                      + (index.isEmpty() ? QString()
                                         : QStringLiteral("int ") + index + QStringLiteral(", "))
                      + elemType + QLatin1Char(' ') + item + QStringLiteral(" : ") + arrExpr
                      + QLatin1Char(')'),
                  node.id);
        lines.add(pad + QLatin1Char('{'), node.id);
        lines.add(body);
        lines.add(pad + QLatin1Char('}'), node.id);
        lines.add(subChain(ctx, node, QStringLiteral("done"), depth));
        return lines;
    }

    if (node.ref == QLatin1String("bi.while")) {
        const Emitted body = subChain(ctx, node, QStringLiteral("body"), depth + 1);
        Emitted lines;
        lines.add(pad + QStringLiteral("while (") + expr(ctx, node, QStringLiteral("cond"))
                      + QLatin1Char(')'),
                  node.id);
        lines.add(pad + QLatin1Char('{'), node.id);
        lines.add(body);
        lines.add(pad + QLatin1Char('}'), node.id);
        lines.add(subChain(ctx, node, QStringLiteral("done"), depth));
        return lines;
    }

    if (node.ref == QLatin1String("bi.return")) {
        // A value typed into the pin counts exactly as much as a wired one;
        // reading only the edge dropped it and emitted a bare `return;`.
        const bool wired = edgeInto(*ctx.graph, node.id, QStringLiteral("value")) != nullptr;
        const bool typed = node.inputs.contains(QStringLiteral("value"))
                           && !node.inputs.value(QStringLiteral("value")).isEmpty();
        QString line = pad + QStringLiteral("return");
        if (wired || typed) {
            line += QLatin1Char(' ');
            line += expr(ctx, node, QStringLiteral("value"));
        } else if (ctx.retType != QLatin1String("void")) {
            // `return;` does not compile in a method with a return type.
            line += QLatin1Char(' ') + defaultForType(ctx, ctx.retType);
            note(ctx, QStringLiteral("emptyreturn:") + node.id,
                 QStringLiteral("A Return node has no value, but the method returns ")
                     + ctx.retType
                     + QStringLiteral(". Give the Return node a value. The generated code "
                                      "returns the type's default."));
        }
        line += QLatin1Char(';');
        return ownedBy(node.id, {line});
    }

    if (node.ref == QLatin1String("bi.super")) {
        // handled by the event header; a stray node is a no-op worth flagging
        ctx.warnings.append(QStringLiteral(
            "A \"Call Super\" node is redundant: super is emitted automatically. "
            "Use the event's \"skip super\" option to suppress it."));
        return {};
    }

    if (node.ref == QLatin1String("bi.serverOnly")) {
        // Events fire on both sides; anything authoritative has to bail on the
        // client. The early-out has to carry a value when the enclosing method
        // has a return type, or the method does not compile at all.
        const QString v = defaultForType(ctx, ctx.retType);
        return ownedBy(node.id,
                       {pad + QStringLiteral("if (!GetGame().IsServer())"),
                        pad + Tab + QStringLiteral("return")
                            + (v.isEmpty() ? QString() : QLatin1Char(' ') + v) + QLatin1Char(';')});
    }

    if (node.ref == QLatin1String("bi.raw")) {
        const QString code = node.opts.value(QStringLiteral("code"));
        QStringList lines;
        for (const QString &l : code.split(QLatin1Char('\n'))) lines << pad + l;
        return ownedBy(node.id, lines);
    }

    if (node.ref == QLatin1String("bi.print")) {
        // Projects written against the Electron build store the argument under
        // p0, the id every other parameter-carrying node uses; the pin here is
        // named "value". Read whichever one the file actually carries.
        const bool onValue = edgeInto(*ctx.graph, node.id, QStringLiteral("value"))
                             || node.inputs.contains(QStringLiteral("value"));
        const bool onP0 = !onValue
                          && (edgeInto(*ctx.graph, node.id, QStringLiteral("p0"))
                              || node.inputs.contains(QStringLiteral("p0")));
        const QString pin = onP0 ? QStringLiteral("p0") : QStringLiteral("value");
        return ownedBy(node.id, {pad + QStringLiteral("Print(") + expr(ctx, node, pin)
                                 + QStringLiteral(");")});
    }

    if (node.ref == QLatin1String("bi.cast")) {
        QString cls = castClassOf(node);
        if (cls.isEmpty()) cls = QStringLiteral("ItemBase");
        const QString v = QStringLiteral("cast%1").arg(ctx.tempN++);
        ctx.temps.insert(tempKey(node.id, QStringLiteral("as")), v);
        const Emitted ok = subChain(ctx, node, QStringLiteral("success"), depth + 1);
        const Emitted bad = subChain(ctx, node, QStringLiteral("failed"), depth + 1);
        Emitted lines;
        lines.add(pad + cls + QLatin1Char(' ') + v + QLatin1Char(';'), node.id);
        lines.add(pad + QStringLiteral("if (Class.CastTo(") + v + QStringLiteral(", ")
                      + expr(ctx, node, QStringLiteral("obj")) + QStringLiteral("))"),
                  node.id);
        lines.add(pad + QLatin1Char('{'), node.id);
        lines.add(ok);
        lines.add(pad + QLatin1Char('}'), node.id);
        if (!bad.isEmpty()) {
            lines.add(pad + QStringLiteral("else"), node.id);
            lines.add(pad + QLatin1Char('{'), node.id);
            lines.add(bad);
            lines.add(pad + QLatin1Char('}'), node.id);
        }
        return lines;
    }

    if (node.ref == QLatin1String("bi.new")) {
        QString cls = castClassOf(node);
        if (cls.isEmpty()) cls = QStringLiteral("ItemBase");
        if (!checkConstructible(ctx, cls)) {
            ctx.poisoned.insert(node.id);
            return ownedBy(node.id,
                           {pad + QStringLiteral("// ") + cls
                            + QStringLiteral(" cannot be created with new (see Warnings)")});
        }
        if (readOnceByNext(ctx, node)) {
            ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")),
                             QStringLiteral("new ") + cls + QStringLiteral("()"));
            return {};
        }
        const QString v = QStringLiteral("obj%1").arg(ctx.tempN++);
        ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), v);
        const QString owned = isManaged(ctx, cls) ? QStringLiteral("ref ") : QString();
        if (!owned.isEmpty()
            && !edgeFrom(*ctx.graph, node.id, QStringLiteral("ret")))
            ctx.warnings.append(
                QStringLiteral("The new ") + cls
                + QStringLiteral(" is never used, so it is collected immediately. Wire its object "
                                 "pin, or store it in a class variable if it needs to outlive this "
                                 "call."));
        return ownedBy(node.id, {pad + owned + cls + QLatin1Char(' ') + v
                                 + QStringLiteral(" = new ") + cls + QStringLiteral("();")});
    }

    if (node.ref == QLatin1String("bi.spawn")) {
        const QString v = QStringLiteral("ent%1").arg(ctx.tempN++);
        ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), v);
        const QString type = expr(ctx, node, QStringLiteral("type"));
        const bool havePos = edgeInto(*ctx.graph, node.id, QStringLiteral("pos")) != nullptr
                             || !node.inputs.value(QStringLiteral("pos")).isEmpty();
        // GetPosition() is an Object method, so the "spawn where I am" default
        // only exists when this class is an entity.
        QString pos;
        if (havePos) {
            pos = expr(ctx, node, QStringLiteral("pos"));
        } else if (!selfKnown(ctx) || ctx.cat->isA(ctx.self, QStringLiteral("Object"))) {
            pos = QStringLiteral("GetPosition()");
        } else {
            pos = QStringLiteral("\"0 0 0\"");
            note(ctx, QStringLiteral("spawnpos:") + node.id,
                 ctx.self
                     + QStringLiteral(" is not an entity, so it has no position of its own to "
                                      "spawn at. Wire the Spawn Entity position pin. The "
                                      "generated code spawns at the world origin."));
        }
        return ownedBy(node.id,
                       {pad + QStringLiteral("EntityAI ") + v
                        + QStringLiteral(" = EntityAI.Cast(GetGame().CreateObjectEx(") + type
                        + QStringLiteral(", ") + pos + QStringLiteral(", ECE_PLACE_ON_SURFACE));")});
    }

    if (node.ref == QLatin1String("bi.setElement")) {
        const QString arr = operandExpr(ctx, node, QStringLiteral("arr"), 11, false);
        const QString index = expr(ctx, node, QStringLiteral("index"));
        const QString value = expr(ctx, node, QStringLiteral("v"));
        return ownedBy(node.id, {pad + arr + QLatin1Char('[') + index + QStringLiteral("] = ")
                                 + value + QLatin1Char(';')});
    }

    if (node.ref == QLatin1String("bi.setMember")) {
        const QString name = node.opts.value(QStringLiteral("name")).trimmed();
        if (name.isEmpty()) {
            ctx.warnings.append(QStringLiteral(
                "A \"Set Member\" node has no member name, so it is not generated. Name it "
                "in Details."));
            return {};
        }
        const QString value = expr(ctx, node, QStringLiteral("v"));
        const QString target = edgeInto(*ctx.graph, node.id, QStringLiteral("target"))
                                   ? operandExpr(ctx, node, QStringLiteral("target"), 11, false)
                                         + QLatin1Char('.')
                                   : QString();
        return ownedBy(node.id, {pad + target + name + QStringLiteral(" = ") + value
                                 + QLatin1Char(';')});
    }

    if (node.kind == NodeKind::VarSet) {
        const GraphVariable *v = ownVariable(ctx, node);
        if (!v) return {};
        ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), v->name);
        return ownedBy(node.id, {pad + v->name + QStringLiteral(" = ")
                                 + expr(ctx, node, QStringLiteral("v")) + QLatin1Char(';')});
    }

    // ---- another script's member, written through a target
    const MemberTarget member =
        ctx.project ? resolveMember(node.ref, *ctx.project) : MemberTarget{};
    if (member.valid && member.setter) {
        const QString target = edgeInto(*ctx.graph, node.id, QStringLiteral("target"))
                                   ? operandExpr(ctx, node, QStringLiteral("target"), 11, false)
                                   : QStringLiteral("this");
        const QString value = expr(ctx, node, QStringLiteral("v"));
        return ownedBy(node.id, {pad + target + QLatin1Char('.') + member.variable->name
                                 + QStringLiteral(" = ") + value + QLatin1Char(';')});
    }

    // ---- a function on this or another project script
    const CallTarget scriptCall = ctx.project ? resolveCall(node.ref, *ctx.project) : CallTarget{};
    if (scriptCall.valid) {
        QStringList args;
        for (int i = 0; i < scriptCall.fn->params.size(); ++i)
            args << expr(ctx, node, QStringLiteral("p%1").arg(i));
        const QString argList = args.join(QStringLiteral(", "));
        // A static helper is qualified by its class; an instance call uses the
        // target pin, defaulting to `this`.
        const QString target =
            scriptCall.fn->isStatic
                ? scriptCall.script->name
                : (edgeInto(*ctx.graph, node.id, QStringLiteral("target"))
                       ? operandExpr(ctx, node, QStringLiteral("target"), 11, false)
                       : QStringLiteral("this"));
        const QString invocation =
            target == QLatin1String("this")
                ? scriptCall.fn->name + QLatin1Char('(') + argList + QLatin1Char(')')
                : target + QLatin1Char('.') + scriptCall.fn->name + QLatin1Char('(') + argList
                      + QLatin1Char(')');
        const bool consumed = edgeFrom(*ctx.graph, node.id, QStringLiteral("ret")) != nullptr;
        if (returnsValue(*scriptCall.fn) && consumed && readOnceByNext(ctx, node)) {
            ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), invocation);
            return {};
        }
        if (returnsValue(*scriptCall.fn) && consumed) {
            const QString v = QStringLiteral("v%1").arg(ctx.tempN++);
            ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), v);
            return ownedBy(node.id, {pad + returnTypeOf(*scriptCall.fn) + QLatin1Char(' ') + v
                                     + QStringLiteral(" = ") + invocation + QLatin1Char(';')});
        }
        return ownedBy(node.id, {pad + invocation + QLatin1Char(';')});
    }

    // ---- a catalogue call used as a statement
    const MethodSig m = ctx.cat->method(node.ref);
    const bool isCtor = m.valid && (m.flags & flag::Ctor);
    if (isCtor && !checkConstructible(ctx, m.owner)) {
        ctx.poisoned.insert(node.id);
        return ownedBy(node.id,
                       {pad + QStringLiteral("// ") + m.owner
                        + QStringLiteral(" cannot be created with new (see Warnings)")});
    }

    const Call call = callExpression(ctx, node);
    if (!call.valid)
        return ownedBy(node.id, {pad + QStringLiteral("// [could not build call for ") + node.ref
                                 + QLatin1Char(']')});

    // A call with no object to run against will not compile, so it is shown
    // commented out rather than emitted as something that looks fine.
    if (call.badTarget) {
        ctx.poisoned.insert(node.id);
        return ownedBy(node.id, {pad + QStringLiteral("// ") + call.text
                                 + QStringLiteral("; needs a target (see Warnings)")});
    }

    Emitted lines;
    // `out` parameters need a declared local before the call
    for (const QString &pre : call.pre) lines.add(pad + pre, node.id);

    const Pin *retPin = def.pin(QStringLiteral("ret"), PinDir::Out);
    const bool consumed = retPin && edgeFrom(*ctx.graph, node.id, QStringLiteral("ret")) != nullptr;
    // A constructor keeps its local: `new` in a statement of its own is what
    // holds the object alive, and the warnings below are written against that.
    if (consumed && !isCtor && call.pre.isEmpty() && readOnceByNext(ctx, node)) {
        ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), call.text);
        return {};
    }
    if (consumed) {
        const MethodSig sigM = m.valid ? m : ctx.cat->globalFn(node.ref);
        QString t = sigM.valid ? sigM.ret : QString();
        if (t.isEmpty()) t = QStringLiteral("auto");
        const QString v = QStringLiteral("v%1").arg(ctx.tempN++);
        ctx.temps.insert(tempKey(node.id, QStringLiteral("ret")), v);
        const QString owned = isManaged(ctx, t) ? QStringLiteral("ref ") : QString();
        lines.add(pad + owned + t + QLatin1Char(' ') + v + QStringLiteral(" = ") + call.text
                      + QLatin1Char(';'),
                  node.id);
        // Only a problem if it is never handed to a member. A Set node on a class
        // variable is exactly how you keep it alive, so that must not warn.
        bool storedInMember = false;
        for (const GraphEdge &e : ctx.graph->edges) {
            if (e.from.node != node.id || e.from.pin != QLatin1String("ret")) continue;
            const GraphNode *to = ctx.graph->node(e.to.node);
            if (to && to->kind == NodeKind::VarSet) storedInMember = true;
        }
        if (isCtor && !owned.isEmpty() && !storedInMember)
            ctx.warnings.append(
                QStringLiteral("The new ") + t
                + QStringLiteral(" only lives in a local, so it is collected when this call ends. "
                                 "Store it in a class variable if it has to keep running, as a "
                                 "Timer does."));
    } else {
        if (isCtor)
            ctx.warnings.append(
                QStringLiteral("The new ") + m.owner
                + QStringLiteral(" is created and immediately discarded: nothing is wired to its "
                                 "object pin, so it does nothing. Store it in a class variable, or "
                                 "remove the node."));
        lines.add(pad + call.text + QLatin1Char(';'), node.id);
    }
    return lines;
}

// ----------------------------------------------------------------- expressions

// Hand-rolled rather than /^".*"$/ because PCRE also matches with a trailing
// newline inside the quotes, and a multi-line literal must be re-escaped.
bool alreadyQuoted(const QString &v)
{
    return v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))
           && !v.contains(QLatin1Char('\n')) && !v.contains(QLatin1Char('\r'));
}

// A backslash is an escape character to the Enforce parser, so a Windows path
// typed into a string pin silently changes meaning ("\t" becomes a tab) unless
// it is doubled. It has to be escaped before the quote, or the escapes we add
// get escaped in turn.
QString quoted(const QString &v)
{
    QString escaped = v;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

// An Any pin has no inline editor in the app, but a file written elsewhere can
// still carry text on one. Anything that is already a literal or an expression
// is passed through; loose prose is quoted rather than emitted as bare tokens.
bool looksLikeCode(const QString &v)
{
    if (v == QLatin1String("true") || v == QLatin1String("false")
        || v == QLatin1String("null"))
        return true;
    bool ok = false;
    v.toDouble(&ok);
    if (ok) return true;
    static const QRegularExpression identOrCall(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*(\\(.*\\))?$"));
    return identOrCall.match(v).hasMatch();
}

QString literal(const QString &raw, PinKind kind)
{
    const QString v = raw.trimmed();
    if (kind == PinKind::String) return alreadyQuoted(v) ? v : quoted(v);
    if (kind == PinKind::Vector)
        return alreadyQuoted(v) ? v : QLatin1Char('"') + v + QLatin1Char('"');
    if (kind == PinKind::Bool)
        return (v == QLatin1String("true") || v == QLatin1String("1")) ? QStringLiteral("true")
                                                                       : QStringLiteral("false");
    if (kind == PinKind::Any) {
        if (v.isEmpty() || alreadyQuoted(v) || looksLikeCode(v)) return v;
        return quoted(v);
    }
    return v;
}

// The operator a bi.op node applies. Placed from a per-operator palette entry
// it rides in the ref (bi.op.*), placed from the bare Operator entry it sits in
// opts. Reading only opts made every preset node generate a plus.
QString operatorOf(const GraphNode &node)
{
    const QString fromOpts = node.opts.value(QStringLiteral("op"));
    if (!fromOpts.isEmpty()) return fromOpts;
    const QLatin1String prefix("bi.op.");
    if (node.ref.startsWith(prefix)) return node.ref.mid(prefix.size());
    return QStringLiteral("+");
}

// Binding strength, on the same ladder the parser reads expressions with, so
// the two agree about what a tree means. Only the operators the Operator node
// offers are here; anything else reaches the generator as text.
int precedenceOf(const QString &op)
{
    if (op == QLatin1String("||")) return 1;
    if (op == QLatin1String("&&")) return 2;
    if (op == QLatin1String("==") || op == QLatin1String("!=")) return 6;
    if (op == QLatin1String("<") || op == QLatin1String("<=") || op == QLatin1String(">")
        || op == QLatin1String(">="))
        return 7;
    if (op == QLatin1String("+") || op == QLatin1String("-")) return 9;
    if (op == QLatin1String("*") || op == QLatin1String("/") || op == QLatin1String("%"))
        return 10;
    return 0;
}

// One term: something that can stand next to an operator without brackets round
// it. Hand-written text arrives here as a string with no tree behind it, so the
// only safe reading is the characters: an operator outside brackets and outside
// a string means more than one term.
bool looksAtomic(const QString &s)
{
    const QString t = s.trimmed();
    if (t.isEmpty()) return true;
    static const QString ops = QStringLiteral("+-*/%<>=!&|^?:~,");
    int depth = 0;
    bool inString = false;
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (inString) {
            if (c == QLatin1Char('\\')) i++;
            else if (c == QLatin1Char('"')) inString = false;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) depth++;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) depth--;
        else if (depth == 0 && ops.contains(c)) {
            // A sign or a `!` in front of the term belongs to it.
            if (i == 0 && (c == QLatin1Char('-') || c == QLatin1Char('+')
                           || c == QLatin1Char('!') || c == QLatin1Char('~')))
                continue;
            return false;
        }
    }
    return depth == 0;
}

// An operand of an operator, bracketed only where dropping the brackets would
// change what it means. The generator used to write `(a + b)` for every
// operator, which is correct and never what the author wrote, so a file that
// converted came back spelled differently and was turned down.
QString operandExpr(Ctx &ctx, const GraphNode &node, const QString &pinId, int outer,
                    bool rightHandSide)
{
    const QString text = expr(ctx, node, pinId);
    const GraphEdge *e = edgeInto(*ctx.graph, node.id, pinId);
    const GraphNode *src = e ? ctx.graph->node(e->from.node) : nullptr;
    if (src && src->ref.startsWith(QLatin1String("bi.op"))) {
        const int inner = precedenceOf(operatorOf(*src));
        const bool needs = inner < outer || (inner == outer && rightHandSide);
        return needs ? QLatin1Char('(') + text + QLatin1Char(')') : text;
    }
    // A Select is a ternary, which binds looser than every operator.
    if (src && src->ref == QLatin1String("bi.select"))
        return QLatin1Char('(') + text + QLatin1Char(')');
    return looksAtomic(text) ? text : QLatin1Char('(') + text + QLatin1Char(')');
}

QString pureExpr(Ctx &ctx, const GraphNode &node, const NodeDef &def, const QString &pinId)
{
    if (node.ref == QLatin1String("bi.self")) return QStringLiteral("this");
    // The general Literal node carries its type in opts rather than in the id,
    // unlike the per-type bi.lit* family below.
    if (node.ref == QLatin1String("bi.literal")) {
        const Catalog *cat = ctx.cat;
        const std::function<bool(const QString &)> isEnumFn =
            [cat](const QString &n) { return cat && cat->isEnum(n); };
        const PinType t = pinTypeOf(node.opts.value(QStringLiteral("type"),
                                                    QStringLiteral("string")), isEnumFn);
        const QString raw = node.inputs.value(QStringLiteral("v"));
        return raw.isEmpty() ? defaultLiteral(t) : literal(raw, t.kind);
    }
    if (node.ref == QLatin1String("bi.litBool"))
        return literal(node.inputs.value(QStringLiteral("v"), QStringLiteral("false")),
                       PinKind::Bool);
    if (node.ref == QLatin1String("bi.litInt"))
        return literal(node.inputs.value(QStringLiteral("v"), QStringLiteral("0")), PinKind::Int);
    if (node.ref == QLatin1String("bi.litFloat"))
        return literal(node.inputs.value(QStringLiteral("v"), QStringLiteral("0.0")),
                       PinKind::Float);
    if (node.ref == QLatin1String("bi.litString"))
        return literal(node.inputs.value(QStringLiteral("v"), QString()), PinKind::String);
    if (node.ref == QLatin1String("bi.litVector"))
        return literal(node.inputs.value(QStringLiteral("v"), QStringLiteral("0 0 0")),
                       PinKind::Vector);
    if (node.ref == QLatin1String("bi.litClass")) {
        const QString v = node.inputs.value(QStringLiteral("v"));
        return (v.isEmpty() ? QStringLiteral("string") : v).trimmed();
    }
    if (node.ref == QLatin1String("bi.rawExpr")) {
        const QString code = node.opts.value(QStringLiteral("code"));
        return code.isEmpty() ? QStringLiteral("null") : code;
    }
    // `!` binds tighter than every binary operator, so anything built out of one
    // keeps its brackets and a single term does not.
    if (node.ref == QLatin1String("bi.not"))
        return QLatin1Char('!') + operandExpr(ctx, node, QStringLiteral("a"), 11, false);
    // Each operand is resolved into a named value first: resolving one has side
    // effects, and the operands of `+` are unsequenced in C++, so building the
    // line in one expression leaves the evaluation order to the compiler.
    if (node.ref == QLatin1String("bi.select")) {
        const QString cond = operandExpr(ctx, node, QStringLiteral("cond"), 0, true);
        const QString a = operandExpr(ctx, node, QStringLiteral("a"), 0, true);
        const QString b = operandExpr(ctx, node, QStringLiteral("b"), 0, true);
        return cond + QStringLiteral(" ? ") + a + QStringLiteral(" : ") + b;
    }
    if (node.ref.startsWith(QLatin1String("bi.op"))) {
        const int prec = precedenceOf(operatorOf(node));
        const QString a = operandExpr(ctx, node, QStringLiteral("a"), prec, false);
        const QString b = operandExpr(ctx, node, QStringLiteral("b"), prec, true);
        return a + QLatin1Char(' ') + operatorOf(node) + QLatin1Char(' ') + b;
    }

    if (node.kind == NodeKind::VarGet) {
        const GraphVariable *v = ownVariable(ctx, node);
        return v ? v->name : QStringLiteral("null");
    }

    // another script's member, read through a target
    const MemberTarget member =
        ctx.project ? resolveMember(node.ref, *ctx.project) : MemberTarget{};
    if (member.valid && !member.setter) {
        const QString target = edgeInto(*ctx.graph, node.id, QStringLiteral("target"))
                                   ? operandExpr(ctx, node, QStringLiteral("target"), 11, false)
                                   : QStringLiteral("this");
        return target == QLatin1String("this")
                   ? member.variable->name
                   : target + QLatin1Char('.') + member.variable->name;
    }

    // a pure function on a project script, inlined
    const CallTarget scriptCall = ctx.project ? resolveCall(node.ref, *ctx.project) : CallTarget{};
    if (scriptCall.valid) {
        QStringList args;
        for (int i = 0; i < scriptCall.fn->params.size(); ++i)
            args << expr(ctx, node, QStringLiteral("p%1").arg(i));
        const QString argList = args.join(QStringLiteral(", "));
        const QString target =
            scriptCall.fn->isStatic
                ? scriptCall.script->name
                : (edgeInto(*ctx.graph, node.id, QStringLiteral("target"))
                       ? expr(ctx, node, QStringLiteral("target"))
                       : QStringLiteral("this"));
        return target == QLatin1String("this")
                   ? scriptCall.fn->name + QLatin1Char('(') + argList + QLatin1Char(')')
                   : target + QLatin1Char('.') + scriptCall.fn->name + QLatin1Char('(') + argList
                         + QLatin1Char(')');
    }

    // ---- a template used as an expression
    if (isTemplateKey(node.ref)) {
        const NodeTemplate t = findTemplate(node.ref);
        if (!t.valid) return QStringLiteral("null");
        const QString rendered =
            renderTemplate(t, [&](const QString &id) { return expr(ctx, node, id); });
        // Wrapped so a template like `a + b` composes safely inside a larger
        // expression without depending on Enforce's precedence rules.
        static const QRegularExpression callShape(QStringLiteral("^[\\w.]+\\([^()]*\\)$"));
        static const QRegularExpression nameShape(QStringLiteral("^[\\w.]+$"));
        return callShape.match(rendered).hasMatch() || nameShape.match(rendered).hasMatch()
                   ? rendered
                   : QLatin1Char('(') + rendered + QLatin1Char(')');
    }

    if (node.ref.startsWith(QLatin1String("en"))) {
        const QString name = ctx.cat->enumName(node.ref);
        return name.isEmpty() ? QStringLiteral("null") : name;
    }
    if (node.ref.startsWith(QLatin1String("co"))) {
        const QString name = ctx.cat->constName(node.ref);
        return name.isEmpty() ? QStringLiteral("null") : name;
    }

    const Call call = callExpression(ctx, node);
    if (!call.valid) return QStringLiteral("null");
    if (call.badTarget) return QStringLiteral("null");
    if (pinId != QLatin1String("ret")) {
        // reading an `out` param off a pure node is not expressible inline
        ctx.warnings.append(QLatin1Char('"') + def.title
                            + QStringLiteral("\" has output parameters; use it as a statement "
                                             "rather than inline."));
    }
    return call.text;
}

QString expr(Ctx &ctx, const GraphNode &node, const QString &pinId)
{
    const GraphEdge *e = edgeInto(*ctx.graph, node.id, pinId);
    if (!e) {
        const NodeDef def = defOf(ctx, node);
        const Pin *pin = def.valid ? def.pin(pinId, PinDir::In) : nullptr;
        const QString raw = node.inputs.value(pinId);
        if (node.inputs.contains(pinId) && !raw.isEmpty())
            return literal(raw, pin ? pin->type.kind : PinKind::Any);
        // A pin that declares its own default means it: the type-derived
        // fallback is only for pins that never declared one.
        if (pin && pin->hasDef) return pin->def;
        return pin ? defaultLiteral(pin->type) : QStringLiteral("null");
    }

    const QString key = tempKey(e->from.node, e->from.pin);
    const QString cached = ctx.temps.value(key);
    if (!cached.isEmpty()) {
        ctx.tempReads++;
        return cached;
    }

    const GraphNode *src = ctx.graph->node(e->from.node);
    if (!src) return QStringLiteral("null");
    const NodeDef srcDef = defOf(ctx, *src);
    if (!srcDef.valid) {
        // Substituting null for a node nobody can resolve is the one failure
        // that produces plausible-looking script, so it has to be said out loud.
        note(ctx, QStringLiteral("unresolved:") + src->id,
             QStringLiteral("Node %1 points at \"%2\", which no longer resolves; the value it "
                            "fed is generated as null.")
                 .arg(src->id, src->ref));
        return QStringLiteral("null");
    }

    if (srcDef.pure) {
        // Pure nodes are inlined by recursion, so a data cycle would recurse
        // until the stack dies, and four ordinary drags make one: A.ret -> B.a,
        // B.ret -> A.a. Re-entering a pin that is already being resolved cuts it.
        if (ctx.evaluating.contains(key)) {
            note(ctx, QStringLiteral("datacycle:") + key,
                 QLatin1Char('"') + srcDef.title
                     + QStringLiteral("\" is wired into itself through its own inputs, so its "
                                      "value cannot be worked out. The loop is generated as "
                                      "null; break the connection."));
            return QStringLiteral("null");
        }
        if (ctx.evaluating.size() >= kMaxExprDepth) {
            note(ctx, QStringLiteral("exprdepth"),
                 QStringLiteral("An expression chains more than %1 pure nodes together, which "
                                "the generator cannot resolve. Store an intermediate result in "
                                "a variable.")
                     .arg(kMaxExprDepth));
            return QStringLiteral("null");
        }
        ctx.evaluating.insert(key);
        const QString out = pureExpr(ctx, *src, srcDef, e->from.pin);
        ctx.evaluating.remove(key);
        return out;
    }

    // impure output that has not been emitted yet: the graph is wired so the
    // value is read before its node runs
    ctx.warnings.append(QLatin1Char('"') + srcDef.title
                        + QStringLiteral("\" produces a value used before it executes. Connect its "
                                         "exec pin upstream of the consumer."));
    return QStringLiteral("/* ") + srcDef.title + QStringLiteral(" not executed yet */ null");
}

// Pull the preserved region out of a previous generation, if present.
QString extractUserRegion(const QString &src)
{
    int a = src.indexOf(USER_BEGIN);
    if (a < 0) a = src.indexOf(USER_BEGIN_LEGACY);
    const int b = src.indexOf(USER_END);
    if (a < 0 || b < 0 || b < a)
        return ind(1) + QStringLiteral("// helpers you write here are preserved");
    const int start = src.indexOf(QLatin1Char('\n'), a) + 1;
    QString body = b > start ? src.mid(start, b - start) : QString();
    while (!body.isEmpty() && body.at(body.size() - 1).isSpace()) body.chop(1);
    return body.isEmpty() ? ind(1) : body;
}

} // namespace

GenResult generateEnforce(const Graph &graph, const Catalog &cat, const Builtins &builtins,
                          const Project &project, const QString &previous)
{
    Ctx ctx;
    ctx.cat = &cat;
    ctx.builtins = &builtins;
    ctx.graph = &graph;
    ctx.project = &project;
    ctx.self = effectiveClassOf(graph);

    // One entry per generated method, each carrying its own line ownership.
    QVector<Emitted> bodies;
    // Statements destined for the class constructor, from Begin(On Construct).
    Emitted ctorFromGraph;

    if (graph.className.trimmed().isEmpty())
        ctx.warnings.append(QStringLiteral(
            "This script has no class name, so the generated file cannot compile. Name the "
            "class in the Class panel."));

    // ----------------------------------------------- sync and persistence
    QVector<const GraphVariable *> synced;
    QVector<const GraphVariable *> persisted;
    for (const GraphVariable &v : graph.variables) {
        if (v.sync) synced.append(&v);
        if (v.persist) persisted.append(&v);
    }
    // RegisterNetSyncVariable* and the store hooks are declared on EntityAI and
    // nowhere else. Nothing can be proved when the base is another script in
    // this project, but a root class or a known non-entity certainly cannot
    // carry them, and generating them anyway produces a file that never builds.
    const bool couldBeEntity = selfKnown(ctx)
                                   ? cat.isA(ctx.self, QStringLiteral("EntityAI"))
                                   : !ctx.self.isEmpty();
    if (!couldBeEntity && (!synced.isEmpty() || !persisted.isEmpty())) {
        const QString who = ctx.self.isEmpty() ? graph.className : ctx.self;
        ctx.warnings.append(
            who
            + QStringLiteral(" is not an entity, and network sync and saved variables are "
                             "EntityAI-only, so neither block is generated. Extend an entity "
                             "class, or turn those options off."));
        synced.clear();
        persisted.clear();
    }
    const auto writeLines = [&](const QString &ctxName) {
        QStringList out;
        for (const GraphVariable *v : persisted)
            out << ind(2) + ctxName + QStringLiteral(".Write(") + v->name + QStringLiteral(");");
        return out;
    };
    const auto readLines = [&](const QString &ctxName) {
        QStringList out;
        for (const GraphVariable *v : persisted)
            out << ind(2) + QStringLiteral("if (!") + ctxName + QStringLiteral(".Read(") + v->name
                       + QStringLiteral(")) return false;");
        return out;
    };
    bool persistSaveEmitted = false;
    bool persistLoadEmitted = false;

    // Function entry nodes are also events, but they are emitted by the function
    // loop below, since the catalogue knows nothing about them.
    QVector<const GraphNode *> eventNodes;
    for (const GraphNode &n : graph.nodes) {
        LifecycleSig life;
        const bool isLife = lifecycleSig(ctx, n, &life);
        if ((n.kind != NodeKind::Event && !isLife)
            || n.ref.startsWith(QLatin1String("fn.entry.")))
            continue;
        eventNodes.append(&n);
    }

    QHash<QString, QString> seenMethods;

    for (const GraphNode *ev : eventNodes) {
        ctx.temps.clear();
        ctx.chainCache.clear();
        ctx.temps.insert(tempKey(ev->id, QStringLiteral("self")), QStringLiteral("this"));

        LifecycleSig life;
        const bool isLife = lifecycleSig(ctx, *ev, &life);

        MethodSig m;
        if (isLife) {
            m.name = life.method;
            m.ret = life.ret;
            for (const GraphParam &p : life.params) m.params.append({p.type, p.name, 0, QString()});
            m.flags = 0;
            m.valid = true;
        } else {
            m = cat.method(ev->ref);
        }

        if (!m.valid) {
            ctx.warnings.append(
                QStringLiteral("Event node %1 references a missing catalogue entry (%2)")
                    .arg(ev->id, ev->ref));
            continue;
        }

        // event parameters are addressable by name from the node's output pins
        for (int i = 0; i < m.params.size(); ++i)
            ctx.temps.insert(tempKey(ev->id, QStringLiteral("o%1").arg(i)), m.params.at(i).name);
        if (ev->ref == QLatin1String("bi.end"))
            ctx.temps.insert(tempKey(ev->id, QStringLiteral("parent")),
                             m.params.isEmpty() ? QStringLiteral("parent") : m.params.first().name);

        const QString sigRet = m.ret.isEmpty() ? QStringLiteral("void") : m.ret;
        // Statements inside the chain need the enclosing return type: a Server
        // Only guard or a bare Return has to produce a value here.
        ctx.retType = isLife && life.ctor ? QStringLiteral("void") : sigRet;

        const Emitted body = emitChain(ctx, execChain(graph, ev->id, QStringLiteral("exec")), 2);

        if (isLife && life.ctor) {
            // merged into the constructor further down, after sync registration
            ctorFromGraph.add(body);
            continue;
        }

        if (seenMethods.contains(m.name)) {
            ctx.warnings.append(m.name
                                + QStringLiteral("() is driven by two event nodes, so only the "
                                                 "first is emitted. Chain both from one node "
                                                 "instead."));
            continue;
        }
        seenMethods.insert(m.name, ev->id);

        QStringList paramDecls;
        QStringList paramNames;
        for (const MethodSig::Param &p : m.params) {
            paramDecls << (p.type + QLatin1Char(' ') + p.name);
            paramNames << p.name;
        }
        const QString superArgs = paramNames.join(QStringLiteral(", "));
        const QString superInvoke =
            QStringLiteral("super.") + m.name + QLatin1Char('(') + superArgs + QLatin1Char(')');

        // `override` and `super.` are only legal when the base really declares
        // the method. A hook from an unrelated class, or a class with no base at
        // all, produced two compile errors and no diagnostic.
        bool isOverride = !(m.flags & flag::Static);
        // A static method is not virtual, so there is no base version to call.
        bool callSuper =
            isOverride && ev->opts.value(QStringLiteral("noSuper")) != QLatin1String("1");
        if (isOverride && !inheritsMethod(ctx, m)) {
            isOverride = false;
            callSuper = false;
            const QString who = ctx.self.isEmpty()
                                    ? graph.className + QStringLiteral(" has no base class")
                                    : ctx.self + QStringLiteral(" does not declare it");
            ctx.warnings.append(
                m.name + QStringLiteral("() cannot be overridden here: ") + who
                + QStringLiteral(". The method is generated without override, so the engine "
                                 "will never call it. Remove the node, or change the class "
                                 "this script extends."));
        }

        // Persisted members generate the store hooks themselves; an event node
        // for the same method has to merge into that one body rather than add a
        // second override of it.
        const bool mergeSave = !persisted.isEmpty() && m.name == QLatin1String("OnStoreSave");
        const bool mergeLoad = !persisted.isEmpty() && m.name == QLatin1String("OnStoreLoad")
                               && sigRet == QLatin1String("bool");
        const QString ctxName =
            m.params.isEmpty() ? QStringLiteral("ctx") : m.params.first().name;

        // The method exists because this event node does, so its signature, its
        // braces and the super call it wraps the chain in all belong to it. The
        // store reads and writes come from variables rather than nodes, so they
        // stay unowned even when they are merged into an event's method.
        Emitted lines;
        if (mergeSave) {
            if (callSuper) lines.add(ind(2) + superInvoke + QLatin1Char(';'), ev->id);
            for (const QString &l : writeLines(ctxName)) lines.add(l);
            lines.add(body);
            persistSaveEmitted = true;
        } else if (mergeLoad) {
            if (callSuper)
                lines.add(ind(2) + QStringLiteral("if (!") + superInvoke
                              + QStringLiteral(") return false;"),
                          ev->id);
            for (const QString &l : readLines(ctxName)) lines.add(l);
            lines.add(body);
            lines.add(ind(2) + QStringLiteral("return true;"), ev->id);
            persistLoadEmitted = true;
        } else if (!callSuper) {
            lines = body;
        } else if (sigRet == QLatin1String("void")) {
            lines.add(ind(2) + superInvoke + QLatin1Char(';'), ev->id);
            lines.add(body);
        } else if (m.name == QLatin1String("OnStoreLoad") && sigRet == QLatin1String("bool")) {
            // A read context is a sequential stream: reading your own bytes
            // before the base has taken its own desynchronises it, and in DayZ
            // that means the item fails to load and is deleted.
            lines.add(ind(2) + QStringLiteral("if (!") + superInvoke
                          + QStringLiteral(") return false;"),
                      ev->id);
            lines.add(body);
            lines.add(ind(2) + QStringLiteral("return true;"), ev->id);
        } else {
            // Any other value-returning event: the base still has to run first,
            // and its result is what this method returns unless the graph
            // returns something of its own.
            lines.add(ind(2) + sigRet + QStringLiteral(" superRet = ") + superInvoke
                          + QLatin1Char(';'),
                      ev->id);
            lines.add(body);
            lines.add(ind(2) + QStringLiteral("return superRet;"), ev->id);
        }
        // An event with nothing chained off it used to join an empty list
        // between two newlines, which is a blank line inside the braces. Keeping
        // that line keeps the generated text identical.
        if (lines.isEmpty()) lines.add(QString(), ev->id);

        Emitted block;
        block.add(ind(1) + (isOverride ? QStringLiteral("override ") : QString()) + sigRet
                      + QLatin1Char(' ') + m.name + QLatin1Char('(')
                      + paramDecls.join(QStringLiteral(", ")) + QLatin1Char(')'),
                  ev->id);
        block.add(ind(1) + QLatin1Char('{'), ev->id);
        block.add(lines);
        block.add(ind(1) + QLatin1Char('}'), ev->id);
        bodies.append(block);
    }

    // ------------------------------------------- this script's own functions
    for (const GraphFunction &f : graph.functions) {
        ctx.temps.clear();
        ctx.chainCache.clear();
        // An unnamed function emits ` ( )`, a fragment that takes the whole
        // file down with it, so it is refused rather than written out.
        if (f.name.trimmed().isEmpty()) {
            ctx.warnings.append(QStringLiteral(
                "A function has no name and is not generated. Name it in the Variables panel, "
                "or delete it."));
            continue;
        }
        ctx.retType = returnTypeOf(f);
        QStringList modList;
        if (f.isPrivate) modList << QStringLiteral("private");
        if (f.isProtected) modList << QStringLiteral("protected");
        if (f.isStatic) modList << QStringLiteral("static");
        if (f.isOverride) modList << QStringLiteral("override");
        const QString mods = modList.join(QLatin1Char(' '));
        const QString head = (mods.isEmpty() ? QString() : mods + QLatin1Char(' '))
                             + functionSignature(f);

        // A body the importer could not express as nodes is carried verbatim, so
        // round-tripping a real file never silently drops code. An empty body is
        // a real body (`override void OnX() {}` imports as exactly that), so
        // the flag decides, not whether the text is empty.
        if (f.hasRawBody) {
            Emitted block;
            block.add(ind(1) + head);
            block.add(ind(1) + QLatin1Char('{'));
            for (const QString &l : f.rawBody.split(QLatin1Char('\n'))) block.add(l);
            block.add(ind(1) + QLatin1Char('}'));
            bodies.append(block);
            continue;
        }

        const GraphNode *entry = nullptr;
        for (const GraphNode &n : graph.nodes) {
            if (n.ref != QStringLiteral("fn.entry.") + f.id) continue;
            entry = &n;
            break;
        }
        if (entry) {
            ctx.temps.insert(tempKey(entry->id, QStringLiteral("self")), QStringLiteral("this"));
            for (int i = 0; i < f.params.size(); ++i)
                ctx.temps.insert(tempKey(entry->id, QStringLiteral("o%1").arg(i)),
                                 f.params.at(i).name);
        }
        const Emitted body =
            entry ? emitChain(ctx, execChain(graph, entry->id, QStringLiteral("exec")), 2)
                  : Emitted();
        if (!entry)
            ctx.warnings.append(f.name
                                + QStringLiteral("() has no Function node on the canvas, so an "
                                                 "empty body is generated."));
        // A function's entry node is what puts the method in the file, so it
        // owns the signature the same way an event node owns its override.
        const QString owner = entry ? entry->id : QString();
        Emitted block;
        block.add(ind(1) + head, owner);
        block.add(ind(1) + QLatin1Char('{'), owner);
        if (body.isEmpty())
            block.add(ind(2) + QStringLiteral("// place a Function ") + f.name
                          + QStringLiteral(" node and chain from it"),
                      owner);
        else
            block.add(body);
        block.add(ind(1) + QLatin1Char('}'), owner);
        bodies.append(block);
    }

    // ------------------------------------------------------------- members
    QStringList members;
    for (const GraphVariable &v : graph.variables) {
        // A member with no name or no type emits a bare ` ;`, which stops the
        // file from compiling and gives no clue why.
        if (v.name.trimmed().isEmpty() || v.type.trimmed().isEmpty()) {
            ctx.warnings.append(QStringLiteral(
                "A variable has no name or no type and is not generated. Fill both in, or "
                "delete it."));
            continue;
        }
        // Managed members must be `ref` or the instance is collected out from
        // under the class: the classic "my Timer stopped firing" bug. An
        // explicit ref flag wins over the inference, including an explicit
        // false, which is why the flag alone cannot carry the decision.
        const bool owned = v.hasRef ? v.isRef : isManaged(ctx, v.type);
        QStringList modList;
        if (v.isPrivate) modList << QStringLiteral("private");
        if (v.isProtected) modList << QStringLiteral("protected");
        if (v.isStatic) modList << QStringLiteral("static");
        if (v.isConst) modList << QStringLiteral("const");
        if (owned) modList << QStringLiteral("ref");
        const QString mods = modList.join(QLatin1Char(' '));
        members << ind(1) + (mods.isEmpty() ? QString() : mods + QLatin1Char(' ')) + v.type
                       + QLatin1Char(' ') + v.name
                       + (v.def.isEmpty() ? QString() : QStringLiteral(" = ") + v.def)
                       + QLatin1Char(';');
    }

    QStringList ctorLines;
    for (const GraphVariable *v : synced) {
        QString reg;
        if (v->type == QLatin1String("bool")) reg = QStringLiteral("RegisterNetSyncVariableBool");
        else if (v->type == QLatin1String("int")) reg = QStringLiteral("RegisterNetSyncVariableInt");
        else if (v->type == QLatin1String("float")) reg = QStringLiteral("RegisterNetSyncVariableFloat");
        if (!reg.isEmpty())
            ctorLines << ind(2) + reg + QStringLiteral("(\"") + v->name + QStringLiteral("\");");
        else
            ctx.warnings.append(QStringLiteral("Variable \"") + v->name + QStringLiteral("\" is ")
                                + v->type
                                + QStringLiteral("; only bool/int/float can be net-synced."));
    }
    // Sync registration and anything wired to Begin(On Construct) share one
    // constructor. Registration goes first, since the graph may already touch it.
    // The constructor is generated for the class, not for any one node: it can
    // exist with nothing but sync registrations in it, so its signature stays
    // unowned even when a Begin(On Construct) chain supplies the rest.
    Emitted allCtor;
    for (const QString &l : ctorLines) allCtor.add(l);
    allCtor.add(ctorFromGraph);
    if (!allCtor.isEmpty()) {
        Emitted block;
        block.add(ind(1) + QStringLiteral("void ") + graph.className + QStringLiteral("()"));
        block.add(ind(1) + QLatin1Char('{'));
        block.add(allCtor);
        block.add(ind(1) + QLatin1Char('}'));
        bodies.prepend(block);
    }

    // Only the hooks no event node already carries: the graph's own OnStoreSave
    // or OnStoreLoad has the reads and writes merged into it above, and a second
    // override of either is a compile error.
    if (!persisted.isEmpty() && !persistSaveEmitted) {
        Emitted block;
        block.add(ind(1) + QStringLiteral("override void OnStoreSave(ParamsWriteContext ctx)"));
        block.add(ind(1) + QLatin1Char('{'));
        block.add(ind(2) + QStringLiteral("super.OnStoreSave(ctx);"));
        for (const QString &l : writeLines(QStringLiteral("ctx"))) block.add(l);
        block.add(ind(1) + QLatin1Char('}'));
        bodies.append(block);
    }
    if (!persisted.isEmpty() && !persistLoadEmitted) {
        Emitted block;
        block.add(ind(1)
                  + QStringLiteral("override bool OnStoreLoad(ParamsReadContext ctx, int version)"));
        block.add(ind(1) + QLatin1Char('{'));
        block.add(ind(2) + QStringLiteral("if (!super.OnStoreLoad(ctx, version)) return false;"));
        for (const QString &l : readLines(QStringLiteral("ctx"))) block.add(l);
        block.add(ind(2) + QStringLiteral("return true;"));
        block.add(ind(1) + QLatin1Char('}'));
        bodies.append(block);
    }

    // --------------------------------------------------------------- header
    // A bare `class X` is its own root and does NOT derive from Class. That is a
    // real semantic difference in Enforce (JsonFileLoader<T> rejects a rootless
    // class), so an empty base must stay empty rather than defaulting to Managed.
    const QString header =
        graph.modded
            ? QStringLiteral("modded class ") + graph.className
            : (graph.baseClass.isEmpty()
                   ? QStringLiteral("class ") + graph.className
                   : QStringLiteral("class ") + graph.className + QStringLiteral(" extends ")
                         + graph.baseClass);

    if (graph.modded && !graph.baseClass.isEmpty() && graph.className == graph.baseClass)
        ctx.warnings.append(QStringLiteral(
            "A modded class reopens the existing class, so it must not also extend it."));

    const QString userBlock = extractUserRegion(previous);

    // Assembled line by line rather than as one string, so every line keeps the
    // owner it was written with. The class header, the member declarations and
    // the user region belong to the file itself, so their owners stay empty.
    Emitted file;
    file.add(header);
    file.add(QStringLiteral("{"));
    if (!members.isEmpty()) {
        for (const QString &m : members) file.add(m);
        file.add(QString()); // blank line between the members and the first method
    }
    if (bodies.isEmpty()) {
        file.add(ind(1)
                 + QStringLiteral("// add an Event node and chain nodes from its exec pin"));
    } else {
        for (int i = 0; i < bodies.size(); ++i) {
            if (i > 0) file.add(QString()); // methods are separated by a blank line
            file.add(bodies.at(i));
        }
    }
    file.add(QString());
    file.add(ind(1) + USER_BEGIN);
    for (const QString &l : userBlock.split(QLatin1Char('\n'))) file.add(l);
    file.add(ind(1) + USER_END);
    file.add(QStringLiteral("};"));
    // The file ends with a newline, so joining leaves a final empty line. It is
    // kept in `file` as well, since lineOwners is indexed by code.split('\n')
    // and that split produces the same empty last element.
    file.add(QString());

    // A raw-code node or a string pin can carry an embedded newline, which would
    // otherwise leave one entry standing for two lines of the finished file.
    flatten(file);

    GenResult result;
    result.code = file.lines.join(QLatin1Char('\n'));
    result.lineOwners = file.owners;
    result.warnings = ctx.warnings;
    return result;
}
