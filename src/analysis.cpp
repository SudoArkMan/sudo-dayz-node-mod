// Ported from the Electron build's src/analysis. Rule ids are carried over
// verbatim (DZ1xx correctness, DZ2xx dead code, DZ3xx DayZ) so a project
// checked in either tool reports the same findings under the same names.
//
// The context (resolved defs, adjacency, exec-only adjacency, reachability) is
// built once and shared, so adding a rule costs a pass over the nodes rather
// than a fresh graph walk. This runs on every graph mutation, so the indices
// matter: a 222-node import was enough to hang the reference panel back when
// rules walked the edge list themselves.
#include "analysis.h"

#include "enforce/lexer.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

// ---------------------------------------------------------------- result

namespace {

int severityRank(Severity s)
{
    switch (s) {
    case Severity::Error: return 0;
    case Severity::Warning: return 1;
    case Severity::Info: return 2;
    }
    return 2;
}

} // namespace

QVector<Diagnostic> AnalysisResult::forNode(const QString &nodeId) const
{
    QVector<Diagnostic> out;
    if (nodeId.isEmpty()) return out;
    for (const Diagnostic &d : diagnostics)
        if (d.nodeId == nodeId) out.append(d);
    return out;
}

// Info when the node is clean, so callers that only want a badge colour can
// ask hasIssue() first and treat this as "how loud should it be".
Severity AnalysisResult::worstFor(const QString &nodeId) const
{
    Severity worst = Severity::Info;
    if (nodeId.isEmpty()) return worst;
    for (const Diagnostic &d : diagnostics) {
        if (d.nodeId != nodeId) continue;
        if (severityRank(d.severity) < severityRank(worst)) worst = d.severity;
        if (worst == Severity::Error) break;
    }
    return worst;
}

bool AnalysisResult::hasIssue(const QString &nodeId) const
{
    if (nodeId.isEmpty()) return false;
    for (const Diagnostic &d : diagnostics)
        if (d.nodeId == nodeId) return true;
    return false;
}

// --------------------------------------------------------------- context

namespace {

// Builtin ids the rules ask about by name. builtins.h exports the handful other
// modules share; the rest are spelled out here and must match builtins.cpp.
const QLatin1String kServerOnly("bi.serverOnly");
const QLatin1String kSpawn("bi.spawn");
const QLatin1String kNew("bi.new");
const QLatin1String kNot("bi.not");
const QLatin1String kRawExpr("bi.rawExpr");
const QLatin1String kFnEntry("fn.entry.");
const QLatin1String kFnCall("fn.call.");
const QLatin1String kSvGet("sv.get.");
const QLatin1String kSvSet("sv.set.");

// Modules compile in sequence and each one only sees what came before it, so
// the rank is the whole of the load-order rule. `editor` is Workbench-only and
// never part of a mod's chain, which is why it has no rank here.
int moduleRank(const QString &name)
{
    const QString m = name.trimmed().toLower();
    if (m == QLatin1String("1_core")) return 1;
    if (m == QLatin1String("2_gamelib")) return 2;
    if (m == QLatin1String("3_game")) return 3;
    if (m == QLatin1String("4_world")) return 4;
    if (m == QLatin1String("5_mission")) return 5;
    return -1;
}

// fn.call.<scriptId>.<fnId>. Split on the FIRST dot after the prefix, the way
// codegen's splitScriptKey and scriptapi's resolveCall both do: matching the
// tail instead binds whichever function happens to end the same way.
bool splitCallKey(const QString &ref, QString *scriptId, QString *fnId)
{
    if (!ref.startsWith(kFnCall)) return false;
    const QString rest = ref.mid(kFnCall.size());
    const int dot = rest.indexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= rest.size()) return false;
    *scriptId = rest.left(dot);
    *fnId = rest.mid(dot + 1);
    return true;
}

struct Link {
    const GraphNode *node = nullptr; // the far end of the edge
    QString fromPin;
    QString toPin;
};

const QVector<Link> kNoLinks;
const QVector<QString> kNoIds;

Diagnostic diag(Severity severity, const QString &rule, const QString &message,
                const QString &hint, const QString &nodeId = QString(),
                const QString &pinId = QString())
{
    Diagnostic d;
    d.severity = severity;
    d.message = message;
    d.nodeId = nodeId;
    d.pinId = pinId;
    d.rule = rule;
    d.hint = hint;
    return d;
}

Pin dataPin(const QString &id, const QString &label, PinDir dir, const PinType &type)
{
    Pin p;
    p.id = id;
    p.label = label;
    p.dir = dir;
    p.type = type;
    if (dir == PinDir::In && inlineEditorFor(type) != InlineEditor::None) {
        p.def = defaultLiteral(type);
        p.hasDef = true;
    }
    return p;
}

Pin execPin(const QString &id, const QString &label, PinDir dir)
{
    Pin p;
    p.id = id;
    p.label = label;
    p.dir = dir;
    p.type = {PinKind::Exec, {}, false};
    return p;
}

QString functionSignature(const GraphFunction &f)
{
    QStringList params;
    for (const GraphParam &p : f.params)
        params << QStringLiteral("%1 %2").arg(p.type, p.name);
    return QStringLiteral("%1 %2(%3)").arg(f.returns.isEmpty() ? QStringLiteral("void")
                                                               : f.returns,
                                           f.name, params.join(QStringLiteral(", ")));
}

// Entry node for one of this script's own functions. Mirrors the reference
// build's functionEntryDef; the project's other scripts are not visible from
// here, so only functions declared on this graph resolve.
NodeDef functionEntryDef(const GraphFunction &f, const Catalog &cat)
{
    const auto isEnum = [&cat](const QString &n) { return cat.isEnum(n); };
    NodeDef def;
    def.key = QStringLiteral("fn.entry.%1").arg(f.id);
    def.title = QStringLiteral("Function %1").arg(f.name);
    def.subtitle = functionSignature(f);
    def.category = QStringLiteral("Lifecycle");
    def.accent = accents::event();
    def.pins.append(execPin(QStringLiteral("exec"), QString(), PinDir::Out));
    def.pins.append(dataPin(QStringLiteral("self"), QStringLiteral("self"), PinDir::Out,
                            {PinKind::Object, QStringLiteral("auto"), false}));
    for (int i = 0; i < f.params.size(); ++i)
        def.pins.append(dataPin(QStringLiteral("o%1").arg(i), f.params.at(i).name,
                                PinDir::Out, pinTypeOf(f.params.at(i).type, isEnum)));
    def.valid = true;
    return def;
}

// Call node for one of this script's functions. A value-returning helper with
// no side effects reads better without exec pins, the same convention the
// vanilla catalogue uses.
NodeDef functionCallDef(const GraphFunction &f, const QString &owner,
                        const QString &key, const Catalog &cat)
{
    const auto isEnum = [&cat](const QString &n) { return cat.isEnum(n); };
    static const QRegularExpression accessor(QStringLiteral("^(Get|Is|Has|Can|To|From)"));
    const bool voidRet = f.returns.isEmpty() || f.returns == QLatin1String("void");
    const bool pure = !voidRet && (f.isStatic || accessor.match(f.name).hasMatch());

    NodeDef def;
    def.key = key;
    def.title = f.isStatic ? QStringLiteral("%1.%2").arg(owner, f.name) : f.name;
    def.subtitle = f.isStatic ? QStringLiteral("static - %1").arg(f.returns) : owner;
    def.category = QStringLiteral("Functions");
    def.accent = pure ? accents::pure() : accents::call();
    def.pure = pure;
    if (!pure) {
        def.pins.append(execPin(QStringLiteral("exec"), QString(), PinDir::In));
        def.pins.append(execPin(QStringLiteral("exec"), QString(), PinDir::Out));
    }
    if (!f.isStatic)
        def.pins.append(dataPin(QStringLiteral("target"), QStringLiteral("target"),
                                PinDir::In, {PinKind::Object, owner, false}));
    for (int i = 0; i < f.params.size(); ++i)
        def.pins.append(dataPin(QStringLiteral("p%1").arg(i), f.params.at(i).name,
                                PinDir::In, pinTypeOf(f.params.at(i).type, isEnum)));
    if (!voidRet)
        def.pins.append(dataPin(QStringLiteral("ret"), QStringLiteral("return"),
                                PinDir::Out, pinTypeOf(f.returns, isEnum)));
    def.valid = true;
    return def;
}

// A call into a different script. The project is not handed to the analyser, so
// the real signature is out of reach; shaping the node as a bare exec
// pass-through keeps the chain after it alive instead of reporting half the
// graph dead, and stops the rules inventing requirements for pins they cannot
// see. A pure cross-script call has no exec edges, so the pins go unused.
NodeDef foreignCallDef(const QString &key)
{
    NodeDef def;
    def.key = key;
    def.title = QStringLiteral("Call into another script");
    def.category = QStringLiteral("Functions");
    def.accent = accents::call();
    def.pins.append(execPin(QStringLiteral("exec"), QString(), PinDir::In));
    def.pins.append(execPin(QStringLiteral("exec"), QString(), PinDir::Out));
    def.valid = true;
    return def;
}

// Entry points are anything the engine or a caller can start a flow from.
bool isEntry(const GraphNode &n)
{
    return n.kind == NodeKind::Event || n.ref == bi::Begin || n.ref == bi::End
           || n.ref.startsWith(kFnEntry);
}

// Node references this analyser can actually look up. Cross-script calls,
// script members and template nodes live in the project, which analyzeGraph is
// not handed, so an unresolved one is left alone rather than reported missing.
bool resolvableHere(const GraphNode &n)
{
    if (n.kind == NodeKind::Comment) return false;
    if (n.ref.isEmpty()) return false;
    if (n.ref.startsWith(QLatin1String("tpl."))) return false;
    if (n.ref.startsWith(QLatin1String("sv."))) return false;
    if (n.ref.startsWith(kFnCall)) {
        // A call into another script cannot be checked from one graph; it still
        // gets a pass-through def so the flow survives, but a missing target is
        // not something this graph can prove.
        return false;
    }
    return true;
}

// The member id a node reads or writes, for both the local var.get/var.set form
// and the cross-script sv.get/sv.set one. Empty when the node is not a member
// access.
QString memberIdOf(const GraphNode &n)
{
    if (n.ref.startsWith(kSvGet) || n.ref.startsWith(kSvSet)) {
        const int dot = n.ref.lastIndexOf(QLatin1Char('.'));
        return dot >= 0 ? n.ref.mid(dot + 1) : QString();
    }
    if (n.kind == NodeKind::VarGet || n.kind == NodeKind::VarSet
        || n.ref.startsWith(QLatin1String("var.get."))
        || n.ref.startsWith(QLatin1String("var.set.")))
        return variableIdOf(n.ref);
    return QString();
}

// ------------------------------------------------------- raw code reading
//
// Everything below turns a block of hand-written Enforce into something the
// rules can ask questions of. The bias is deliberate and one-directional: a
// check that calls working code broken is worse than no check at all, so every
// case the tokeniser cannot settle resolves to silence.

// Whitespace and comments carry no meaning to a rule and only complicate the
// look-behind and look-ahead, so they come out once here.
QVector<Token> significantTokens(const QString &code)
{
    QVector<Token> out;
    for (const Token &t : EnforceLexer::tokenizeAll(code))
        if (t.kind != TokenKind::Whitespace && t.kind != TokenKind::Comment)
            out.append(t);
    return out;
}

// Keywords that put whatever follows them in a type or label position rather
// than a value one. `return` and `delete` are deliberately absent: what comes
// after those is a value, and a typo in it is worth catching.
const QSet<QString> &typeIntroducers()
{
    static const QSet<QString> s = {
        QStringLiteral("new"),       QStringLiteral("ref"),
        QStringLiteral("autoptr"),   QStringLiteral("notnull"),
        QStringLiteral("owned"),     QStringLiteral("out"),
        QStringLiteral("inout"),     QStringLiteral("const"),
        QStringLiteral("case"),      QStringLiteral("extends"),
        QStringLiteral("modded"),    QStringLiteral("class"),
        QStringLiteral("enum"),      QStringLiteral("typedef"),
        QStringLiteral("typeof"),    QStringLiteral("sizeof"),
        QStringLiteral("static"),    QStringLiteral("auto"),
        QStringLiteral("event"),     QStringLiteral("override"),
        QStringLiteral("proto"),     QStringLiteral("native"),
        QStringLiteral("private"),   QStringLiteral("protected"),
        QStringLiteral("public"),    QStringLiteral("external"),
        QStringLiteral("reference"), QStringLiteral("volatile"),
        QStringLiteral("sealed"),    QStringLiteral("thread"),
    };
    return s;
}

// Names a block brings into scope. Two identifiers in a row is a declaration in
// this language and nothing else, so `SUDO_Manager manager` and `int year` both
// land here, and `int year, month, day;` carries the whole declarator list.
// A closing `>` counts as a type ending, for `array<ref ItemBase> found`, and
// `auto` counts as one too: `auto vehicle = CarScript.Cast(x)` is how half of
// vanilla declares a local.
//
// Over-collecting here costs nothing but silence, which is the direction to err
// in, and `a > b` reading as a declaration of `b` is the whole of the price.
void collectDeclared(const QVector<Token> &sig, QSet<QString> &out)
{
    for (int i = 0; i + 1 < sig.size(); ++i) {
        const Token &type = sig.at(i);
        if (sig.at(i + 1).kind != TokenKind::Identifier) continue;
        const bool typePosition =
            type.kind == TokenKind::Identifier || type.kind == TokenKind::Type
            || (type.kind == TokenKind::Keyword && type.text == QLatin1String("auto"))
            || (type.kind == TokenKind::Operator
                && type.text.endsWith(QLatin1Char('>')));
        if (!typePosition) continue;

        out.insert(sig.at(i + 1).text);

        // `float width = 0.0, height = 0.0;` declares both, so the rest of the
        // declarator list has to be walked past whatever initialisers it
        // carries. Depth keeps a comma inside a call argument out of it.
        int depth = 0;
        for (int j = i + 2; j < sig.size(); ++j) {
            const Token &t = sig.at(j);
            if (t.kind != TokenKind::Punctuation) continue;
            if (t.text == QLatin1String("(") || t.text == QLatin1String("[")) {
                depth++;
            } else if (t.text == QLatin1String(")") || t.text == QLatin1String("]")) {
                if (depth == 0) break;
                depth--;
            } else if (depth > 0) {
                continue;
            } else if (t.text == QLatin1String(",")) {
                if (j + 1 < sig.size() && sig.at(j + 1).kind == TokenKind::Identifier)
                    out.insert(sig.at(j + 1).text);
            } else if (t.text == QLatin1String(";") || t.text == QLatin1String("{")
                       || t.text == QLatin1String("}")) {
                break;
            }
        }
    }
}

// Identifiers a block uses as a VALUE: an argument, an operand, the left side
// of an assignment, the thing being called. Type and static-class positions are
// left out on purpose, because analyzeGraph is handed one graph and never the
// project, and a sibling script's class name appears in exactly those places.
// Reading `SUDO_Const.TICK_INTERVAL` as an unknown name would be wrong in every
// project that has more than one script in it.
QStringList valueIdentifiers(const QVector<Token> &sig)
{
    QStringList out;
    for (int i = 0; i < sig.size(); ++i) {
        if (sig.at(i).kind != TokenKind::Identifier) continue;
        const Token *prev = i > 0 ? &sig.at(i - 1) : nullptr;
        const Token *next = i + 1 < sig.size() ? &sig.at(i + 1) : nullptr;
        const auto punct = [](const Token *t, const char *text) {
            return t && t->kind == TokenKind::Punctuation
                   && t->text == QLatin1String(text);
        };

        // x.y: what `y` is depends on the type of `x`, and there is no type
        // inference here to work that out with.
        if (punct(prev, ".")) continue;
        // X.y: X names a class or a namespace, which may be another script.
        if (punct(next, ".")) continue;
        // `Foo bar`: one of the pair is the type, the other is being declared,
        // and neither is a use.
        if (prev && (prev->kind == TokenKind::Identifier || prev->kind == TokenKind::Type))
            continue;
        if (next && (next->kind == TokenKind::Identifier || next->kind == TokenKind::Type))
            continue;
        if (prev && prev->kind == TokenKind::Keyword
            && typeIntroducers().contains(prev->text))
            continue;
        // #ifdef DIAG_DEVELOPER names a define, not a variable.
        if (prev && prev->kind == TokenKind::Preprocessor) continue;
        // array<X> and map<K, V>. The tokeniser runs operator characters
        // together, so `>>` on a nested template is one token.
        if (prev && prev->kind == TokenKind::Operator
            && prev->text.contains(QLatin1Char('<')))
            continue;
        if (next && next->kind == TokenKind::Operator
            && next->text.contains(QLatin1Char('>')))
            continue;
        // A capitalised name read as a value is a class, a typedef, an enum
        // value or an engine constant, and the catalogue carries only the
        // first of those: `typedef Magazine Magazine_Base` and
        // `ObjIntersectFire` are both invisible to it. A call is the exception
        // worth asking about, because methods and global functions are indexed
        // in full.
        if (!punct(next, "(") && sig.at(i).text.at(0).isUpper()) continue;

        out << sig.at(i).text;
    }
    return out;
}

// m_Quantity and friends. The catalogue indexes methods, not fields, so an
// inherited member is not something this analyser can look up, and a rule that
// flagged one would fire on every modded entity in the game.
bool fieldShaped(const QString &name)
{
    return name.startsWith(QLatin1String("m_")) || name.startsWith(QLatin1String("s_"))
           || name.startsWith(QLatin1String("g_"));
}

// SCREAMING_CASE is where the catalogue is thinnest: enum values are indexed
// under their enum rather than their own name, and a #define leaves no
// declaration to find at all.
bool constantShaped(const QString &name)
{
    bool letter = false;
    for (const QChar c : name) {
        if (c.isLower()) return false;
        if (c.isLetter()) letter = true;
    }
    return letter;
}

// item0, idx1, v2: codegen's own temporaries, which raw code sitting inside a
// generated loop body can legitimately read.
bool generatedLocal(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^(item|idx|v)[0-9]+$"));
    return re.match(name).hasMatch();
}

// One block of hand-written Enforce, scanned once. `node` is null for a
// function body the importer kept verbatim: those still vouch for the names
// they mention, but they are not a node anything can be reported against.
struct RawBlock {
    const GraphNode *node = nullptr;
    QString code;
    EnforceScan scan;
    QVector<Token> sig;
};

struct Ctx {
    Ctx(const Graph &g, const Catalog &c, const Builtins &b, const QString &sid);

    const Graph &graph;
    const Catalog &cat;
    const Builtins &builtins;
    QString scriptId;      // this graph's id in the project, empty when unknown
    // The vanilla class this script's code inherits from: the reopened class
    // for a modded script, the base class otherwise. Every ancestry question
    // (is this event ours to override, is this an entity) hangs off it.
    QString ancestorClass;

    QHash<QString, const GraphNode *> byId;
    QHash<QString, NodeDef> defs;        // by node id; only resolved nodes
    QHash<QString, MethodSig> sigByRef;  // catalogue signatures, one per ref
    QHash<QString, QVector<Link>> incoming;
    QHash<QString, QVector<Link>> outgoing;
    QHash<QString, QVector<QString>> execSucc;
    QHash<QString, QVector<QString>> execPred;
    QSet<QString> reachable;
    QVector<const GraphNode *> entries;
    QVector<RawBlock> raws;      // every scrap of hand-written Enforce, scanned
    QSet<QString> rawNames;      // every identifier and member any of it names
    QSet<QString> rawDeclared;   // everything any of it brings into scope

    const NodeDef *def(const QString &nodeId) const
    {
        const auto it = defs.constFind(nodeId);
        return it == defs.constEnd() ? nullptr : &it.value();
    }
    // Catalogue signature behind a node, or null when the node is not a
    // vanilla method call.
    const MethodSig *sig(const QString &nodeId) const
    {
        const GraphNode *n = byId.value(nodeId);
        if (!n) return nullptr;
        const auto it = sigByRef.constFind(n->ref);
        return it == sigByRef.constEnd() ? nullptr : &it.value();
    }
    QString methodName(const QString &nodeId) const
    {
        const MethodSig *m = sig(nodeId);
        return m ? m->name : QString();
    }
    const QVector<Link> &linksInto(const QString &nodeId) const
    {
        const auto it = incoming.constFind(nodeId);
        return it == incoming.constEnd() ? kNoLinks : it.value();
    }
    const QVector<Link> &linksFrom(const QString &nodeId) const
    {
        const auto it = outgoing.constFind(nodeId);
        return it == outgoing.constEnd() ? kNoLinks : it.value();
    }
    const QVector<QString> &succ(const QString &nodeId) const
    {
        const auto it = execSucc.constFind(nodeId);
        return it == execSucc.constEnd() ? kNoIds : it.value();
    }
    const QVector<QString> &pred(const QString &nodeId) const
    {
        const auto it = execPred.constFind(nodeId);
        return it == execPred.constEnd() ? kNoIds : it.value();
    }
    // The single edge feeding one input pin, or null when it is unwired.
    const Link *linkInto(const QString &nodeId, const QString &pin) const
    {
        for (const Link &l : linksInto(nodeId))
            if (l.toPin == pin) return &l;
        return nullptr;
    }
    bool isA(const QString &child, const QString &base) const
    {
        if (child.isEmpty() || cat.classId(child) < 0) return false;
        return cat.isA(child, base);
    }
};

// What the catalogue knows about one bare method name.
struct VanillaName {
    bool exists = false;                    // some vanilla entry is called this
    QVector<QPair<QString, bool>> methods;  // owning class, is it proto native
};

// Answering "is there a vanilla method called this" costs a scan of all 35k
// search rows, and the same handful of names gets asked on every keystroke: on
// the 222-node SUDO_Manager script the 32 scans were 80 ms of the 84 ms the
// whole analysis took. The catalogue is immutable once loaded, so the distilled
// answer keeps between runs. thread_local rather than a shared static, so no
// lock is needed if analysis is ever moved off the GUI thread; the fingerprint
// throws the cache away if a rebuilt catalogue is loaded after a DayZ update.
VanillaName vanillaName(const Catalog &cat, const QString &name)
{
    thread_local QString fingerprint;
    thread_local QHash<QString, VanillaName> cache;

    const QString now = cat.source() + QLatin1Char('#')
                        + QString::number(cat.classCount()) + QLatin1Char('#')
                        + QString::number(quintptr(&cat), 16);
    if (fingerprint != now) {
        fingerprint = now;
        cache.clear();
    }
    const auto hit = cache.constFind(name);
    if (hit != cache.constEnd()) return hit.value();

    // The index titles an event hook "Event OnX", so both spellings count.
    const QString eventTitle = QStringLiteral("Event %1").arg(name);
    VanillaName v;
    for (const SearchHit &h : cat.search(name, {24, {}, {}})) {
        if (h.title != name && h.title != eventTitle) continue;
        v.exists = true;
        if (!h.key.startsWith(QLatin1Char('m'))) continue;
        const MethodSig m = cat.method(h.key);
        if (m.valid && m.name == name)
            v.methods.append({m.owner, bool(m.flags & flag::Native)});
    }
    cache.insert(name, v);
    return v;
}

// Two wires carry the same value when they leave the same output pin, or when
// they come from any access to the same member. Comparing that identity is what
// keeps "is this the container being iterated" and "is this the timer that was
// started" off an unrelated object.
QString valueIdentity(const Link &l)
{
    const QString member = memberIdOf(*l.node);
    if (!member.isEmpty()) return QStringLiteral("var:") + member;
    return QStringLiteral("pin:") + l.node->id + QLatin1Char('/') + l.fromPin;
}

// Everything the exec flow reaches from these nodes, including the nodes.
void collectFrom(const Ctx &ctx, const QVector<QString> &seeds, QSet<QString> &seen)
{
    QVector<QString> queue;
    for (const QString &id : seeds)
        if (!seen.contains(id)) { seen.insert(id); queue.append(id); }
    while (!queue.isEmpty()) {
        const QString cur = queue.takeLast();
        for (const QString &next : ctx.succ(cur))
            if (!seen.contains(next)) { seen.insert(next); queue.append(next); }
    }
}

// The nodes one named exec output pin leads to, transitively. Rules that care
// which branch a node sits on need this rather than "all successors".
QSet<QString> flowFrom(const Ctx &ctx, const QString &nodeId, const QString &pin)
{
    QVector<QString> seeds;
    for (const Link &l : ctx.linksFrom(nodeId))
        if (l.fromPin == pin) seeds.append(l.node->id);
    QSet<QString> out;
    collectFrom(ctx, seeds, out);
    return out;
}

// Resolves a node the same way the editor does, so a rule sees the pins the
// user sees.
NodeDef resolveDef(const GraphNode &n, const Graph &g, const Catalog &cat,
                   const Builtins &builtins, const QString &ownScriptId)
{
    if (n.kind == NodeKind::VarGet || n.kind == NodeKind::VarSet) {
        if (const GraphVariable *v = variableForRef(g, n.ref))
            return builtins.variableDef(*v, n.kind == NodeKind::VarSet, cat);
        return {};
    }

    if (n.kind == NodeKind::Builtin || n.kind == NodeKind::Comment
        || n.ref.startsWith(QLatin1String("bi.")))
        return builtins.defForNode(n, cat);

    if (n.ref.startsWith(kFnEntry)) {
        const QString id = n.ref.mid(kFnEntry.size());
        for (const GraphFunction &f : g.functions)
            if (f.id == id) return functionEntryDef(f, cat);
        return {};
    }

    // fn.call.<scriptId>.<fnId>: only a call into this script's own functions
    // can be shaped without the rest of the project. When the caller told us
    // which script this is, the id segment decides; when it did not, a function
    // id declared here is the best evidence available.
    if (n.ref.startsWith(kFnCall)) {
        QString scriptId, fnId;
        if (!splitCallKey(n.ref, &scriptId, &fnId)) return {};
        if (ownScriptId.isEmpty() || scriptId == ownScriptId)
            for (const GraphFunction &f : g.functions)
                if (!f.id.isEmpty() && f.id == fnId)
                    return functionCallDef(f, g.className, n.ref, cat);
        return foreignCallDef(n.ref);
    }

    return cat.defFor(n.ref);
}

Ctx::Ctx(const Graph &g, const Catalog &c, const Builtins &b, const QString &sid)
    : graph(g), cat(c), builtins(b), scriptId(sid),
      ancestorClass(g.modded ? g.className : g.baseClass)
{
    byId.reserve(g.nodes.size());
    defs.reserve(g.nodes.size());
    for (const GraphNode &n : g.nodes) {
        byId.insert(n.id, &n);
        const NodeDef d = resolveDef(n, g, c, b, sid);
        if (d.valid) defs.insert(n.id, d);
        if (!sigByRef.contains(n.ref)) {
            const MethodSig m = c.method(n.ref);
            if (m.valid) sigByRef.insert(n.ref, m);
        }
        if (isEntry(n)) entries.append(&n);
    }

    for (const GraphEdge &e : g.edges) {
        const GraphNode *from = byId.value(e.from.node);
        const GraphNode *to = byId.value(e.to.node);
        if (!from || !to) continue;
        incoming[to->id].append({from, e.from.pin, e.to.pin});
        outgoing[from->id].append({to, e.from.pin, e.to.pin});

        // A pin is an exec pin per the node's def, so the distinction gets
        // resolved once here rather than in every rule.
        const NodeDef *fromDef = def(from->id);
        const Pin *pin = fromDef ? fromDef->pin(e.from.pin, PinDir::Out) : nullptr;
        if (!pin || pin->type.kind != PinKind::Exec) continue;
        execSucc[from->id].append(to->id);
        execPred[to->id].append(from->id);
    }

    // Reachability: walk exec flow from every entry, then pull in the nodes
    // those statements read from: a data-only node feeding live code is still
    // live. Feeding a reachable node counts either way, so a stray chain wired
    // into the middle of a live one is not called dead.
    QVector<QString> queue;
    for (const GraphNode *e : entries)
        if (!reachable.contains(e->id)) {
            reachable.insert(e->id);
            queue.append(e->id);
        }
    while (!queue.isEmpty()) {
        const QString cur = queue.takeLast();
        for (const QString &next : succ(cur))
            if (!reachable.contains(next)) {
                reachable.insert(next);
                queue.append(next);
            }
    }

    struct Pull { QString id; int depth; };
    QVector<Pull> pull;
    for (const QString &id : reachable) pull.append({id, 0});
    while (!pull.isEmpty()) {
        const Pull cur = pull.takeLast();
        if (cur.depth > 64) continue; // guards against a pathological import
        for (const Link &link : linksInto(cur.id)) {
            if (reachable.contains(link.node->id)) continue;
            reachable.insert(link.node->id);
            pull.append({link.node->id, cur.depth + 1});
        }
    }

    // Every scrap of hand-written Enforce in this graph, tokenised once. The
    // rules read it rather than trusting it, and "is this name used anywhere?"
    // has to consult it before claiming something is dead.
    const auto addBlock = [this](const GraphNode *node, const QString &code) {
        RawBlock block;
        block.node = node;
        block.code = code;
        block.scan = scanEnforce(code);
        block.sig = significantTokens(code);
        for (const QString &name : block.scan.identifiers) rawNames.insert(name);
        for (const QString &name : block.scan.members) rawNames.insert(name);
        collectDeclared(block.sig, rawDeclared);
        // Code that writes to a bare name has told us the name exists. This is
        // what keeps an inherited field the catalogue does not index, such as
        // UIScriptedMenu::layoutRoot, from reading as unknown everywhere it is
        // used. It costs the ability to spot a typo in a write, which is the
        // rarer mistake and the one the compiler points straight at.
        for (const QString &name : block.scan.assignedTo) rawDeclared.insert(name);
        raws.append(block);
    };
    for (const GraphNode &n : g.nodes) {
        // Older builds stored comment prose under "code" as well, and a note
        // that happens to mention a variable would otherwise vouch for it.
        if (n.kind == NodeKind::Comment || n.ref == bi::Comment) continue;
        const QString code = n.opts.value(QStringLiteral("code"));
        // An empty Raw node is itself a finding, so it is kept; any other node
        // carrying no code has nothing to say.
        const bool rawNode = n.ref == bi::Raw || n.ref == kRawExpr;
        if (code.isEmpty() && !rawNode) continue;
        addBlock(&n, code);
    }
    for (const GraphFunction &f : g.functions)
        if (!f.rawBody.isEmpty()) addBlock(nullptr, f.rawBody);
}

// Whole-word search of the project's hand-written Enforce, over the names the
// tokeniser actually found in it. The substring match this replaced reported
// `m_Count` as used when the only mention in the file was `m_CountDown`.
// Getting it wrong is worse than saying nothing: a checker that calls live code
// dead stops being believed.
bool usedInRawCode(const Ctx &ctx, const QString &name)
{
    return !name.isEmpty() && ctx.rawNames.contains(name);
}

// ---------------------------------------------------------- correctness

// A node whose catalogue entry no longer exists, usually after a DayZ update.
void unresolvedNodes(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        if (ctx.defs.contains(n.id) || !resolvableHere(n)) continue;
        out.append(diag(Severity::Error, QStringLiteral("DZ101"),
                        QStringLiteral("This node points at \"%1\", which no longer exists.")
                            .arg(n.ref),
                        QStringLiteral("It was probably removed by a DayZ update, or its "
                                       "script/variable was deleted. Delete the node and "
                                       "place a current one."),
                        n.id));
    }
}

// The class this script extends must actually exist.
void baseClassExists(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const Graph &g = ctx.graph;
    const auto known = [&ctx](const QString &n) {
        return !n.isEmpty() && ctx.cat.classId(n) >= 0;
    };

    if (g.modded) {
        if (!known(g.className))
            out.append(diag(Severity::Error, QStringLiteral("DZ102"),
                            QStringLiteral("`modded class %1` reopens a class that does "
                                           "not exist.").arg(g.className),
                            QStringLiteral("A modded class must name an existing class "
                                           "exactly. Check the spelling, or switch to "
                                           "`extends` to declare a new one.")));
        return;
    }

    if (!g.baseClass.isEmpty() && !known(g.baseClass))
        out.append(diag(Severity::Error, QStringLiteral("DZ103"),
                        QStringLiteral("`extends %1` names a class that does not exist.")
                            .arg(g.baseClass),
                        QStringLiteral("Pick a vanilla class or another script in this "
                                       "project.")));
    if (ctx.cat.classId(g.className) >= 0)
        out.append(diag(Severity::Error, QStringLiteral("DZ104"),
                        QStringLiteral("%1 is already a vanilla class, so declaring it "
                                       "again will collide.").arg(g.className),
                        QStringLiteral("Rename the class, or switch to modded if you meant "
                                       "to extend the existing %1.").arg(g.className)));
}

// Object pins are checked against the real inheritance chain, so feeding a
// PlayerBase into an ItemBase pin is caught before the compiler sees it.
void pinTypeMismatch(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphEdge &e : ctx.graph.edges) {
        const NodeDef *fromDef = ctx.def(e.from.node);
        const NodeDef *toDef = ctx.def(e.to.node);
        if (!fromDef || !toDef) continue;
        const Pin *a = fromDef->pin(e.from.pin, PinDir::Out);
        const Pin *b = toDef->pin(e.to.pin, PinDir::In);
        if (!a || !b) continue;
        if (a->type.kind != PinKind::Object || b->type.kind != PinKind::Object) continue;

        const QString src = a->type.cls;
        const QString dst = b->type.cls;
        if (src.isEmpty() || dst.isEmpty()) continue;
        if (src == QLatin1String("auto") || dst == QLatin1String("auto")) continue;
        if (src == QLatin1String("Class") || dst == QLatin1String("Class")) continue;
        if (src == dst) continue;
        // A project class cannot be resolved to the vanilla base it descends
        // from without the rest of the project, so it is left alone.
        if (ctx.cat.classId(src) < 0 || ctx.cat.classId(dst) < 0) continue;
        if (ctx.cat.isA(src, dst)) continue;

        out.append(diag(Severity::Error, QStringLiteral("DZ105"),
                        QStringLiteral("%1 is not a %2, so this connection will not "
                                       "compile.").arg(src, dst),
                        QStringLiteral("Put a Cast To %1 node between them and use its "
                                       "success branch.").arg(dst),
                        e.to.node, e.to.pin));
    }
}

// Non-optional inputs must be supplied.
void missingRequiredInput(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        const NodeDef *def = ctx.def(n.id);
        if (!def || !ctx.reachable.contains(n.id)) continue;
        QSet<QString> wired;
        for (const Link &l : ctx.linksInto(n.id)) wired.insert(l.toPin);

        for (const Pin &pin : def->pins) {
            if (pin.dir != PinDir::In || pin.type.kind == PinKind::Exec) continue;
            if (wired.contains(pin.id)) continue;
            // labelled with a default -> optional, and codegen omits it
            if (pin.label.contains(QLatin1Char('='))) continue;
            // `target` defaults to `this`
            if (pin.id == QLatin1String("target")) continue;
            if (!n.inputs.value(pin.id).isEmpty()) continue;
            // an editable pin left blank still yields a literal, so only object
            // and array pins are genuinely unsatisfiable
            if (inlineEditorFor(pin.type) != InlineEditor::None) continue;

            out.append(diag(Severity::Error, QStringLiteral("DZ106"),
                            QStringLiteral("\"%1\" needs a value on its `%2` pin.")
                                .arg(def->title, pin.label.isEmpty() ? pin.id : pin.label),
                            QStringLiteral("Nothing is connected, so it would be passed "
                                           "as null."),
                            n.id, pin.id));
        }
    }
}

// Exec loops that are not a For/While body will hang or misgenerate. Iterative
// three-colour DFS over the precomputed exec adjacency. The reference build's
// path-tracking recursion was superlinear and hung on a 222-node import.
void execCycle(const Ctx &ctx, QVector<Diagnostic> &out)
{
    enum Colour { White, Grey, Black };
    struct Frame { QString id; const QVector<QString> *succ; int next; };

    QHash<QString, int> colour;
    QSet<QString> reported;
    QVector<Frame> stack;

    for (const GraphNode &start : ctx.graph.nodes) {
        if (colour.value(start.id, White) != White) continue;
        colour.insert(start.id, Grey);
        stack.append({start.id, &ctx.succ(start.id), 0});

        while (!stack.isEmpty()) {
            Frame &top = stack.last();
            if (top.next >= top.succ->size()) {
                colour.insert(top.id, Black);
                stack.removeLast();
                continue;
            }
            const QString next = top.succ->at(top.next++);
            const int c = colour.value(next, White);
            if (c == Grey && !reported.contains(next)) {
                reported.insert(next);
                out.append(diag(Severity::Error, QStringLiteral("DZ107"),
                                QStringLiteral("This node is part of an execution loop "
                                               "that feeds back into itself."),
                                QStringLiteral("Use a For Loop or While node for "
                                               "repetition. A wired-back exec chain does "
                                               "not become a loop."),
                                next));
            }
            if (c == White) {
                colour.insert(next, Grey);
                stack.append({next, &ctx.succ(next), 0});
            }
        }
    }
}

// A function that promises a value has to produce one.
void functionMustReturn(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphFunction &f : ctx.graph.functions) {
        const QString key = QStringLiteral("fn.entry.%1").arg(f.id);
        const GraphNode *entry = nullptr;
        for (const GraphNode &n : ctx.graph.nodes)
            if (n.ref == key) { entry = &n; break; }

        if (!entry) {
            out.append(diag(Severity::Warning, QStringLiteral("DZ108"),
                            QStringLiteral("%1() is declared but has no Function node on "
                                           "the canvas, so it generates an empty body.")
                                .arg(f.name),
                            QStringLiteral("Drag the function from the Class panel onto "
                                           "the graph to give it a body.")));
            continue;
        }
        // An unset return type means void here, same as codegen treats it.
        if (f.returns.isEmpty() || f.returns == QLatin1String("void")) continue;

        QSet<QString> seen{entry->id};
        QVector<QString> queue{entry->id};
        bool hasReturn = false;
        while (!queue.isEmpty()) {
            const QString cur = queue.takeLast();
            const GraphNode *node = ctx.byId.value(cur);
            if (node && node->ref == bi::Return) { hasReturn = true; break; }
            for (const QString &next : ctx.succ(cur)) {
                if (seen.contains(next)) continue;
                seen.insert(next);
                queue.append(next);
            }
        }

        if (!hasReturn)
            out.append(diag(Severity::Error, QStringLiteral("DZ109"),
                            QStringLiteral("%1() returns %2 but no branch reaches a "
                                           "Return node.").arg(f.name, f.returns),
                            QStringLiteral("Add a Return node and wire the value you mean "
                                           "to hand back."),
                            entry->id));
    }
}

// proto native methods have no script body, so they cannot be overridden.
void cannotOverrideNative(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        if (n.kind != NodeKind::Event) continue;
        const MethodSig *m = ctx.sig(n.id);
        if (!m || !(m->flags & flag::Native)) continue;
        out.append(diag(Severity::Error, QStringLiteral("DZ110"),
                        QStringLiteral("%1::%2() is implemented in the engine "
                                       "(`proto native`) and cannot be overridden.")
                            .arg(m->owner, m->name),
                        QStringLiteral("Call it from another event instead. A modded "
                                       "override of a native method will not compile."),
                        n.id));
    }
}

// Diag-only API does not exist on a retail server.
void guardedApiInShippingCode(const Ctx &ctx, QVector<Diagnostic> &out)
{
    static const QLatin1String guardPrefix("Only compiled when");
    // explain() scans the search index, so the answer is cached per ref and
    // only asked for the keys that can carry a guard at all.
    QHash<QString, QString> noteByRef;
    QSet<QString> reported;

    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        if (!n.ref.startsWith(QLatin1Char('m')) && !n.ref.startsWith(QLatin1Char('g')))
            continue;

        QString note;
        const auto cached = noteByRef.constFind(n.ref);
        if (cached != noteByRef.constEnd()) {
            note = cached.value();
        } else {
            const NodeHelp help = ctx.cat.explain(n.ref);
            for (const QString &caution : help.cautions)
                if (caution.startsWith(guardPrefix)) { note = caution; break; }
            noteByRef.insert(n.ref, note);
        }
        if (note.isEmpty()) continue;

        const NodeDef *def = ctx.def(n.id);
        const QString key = def ? def->title : n.ref;
        if (reported.contains(key)) continue;
        reported.insert(key);

        note[0] = note.at(0).toLower();
        out.append(diag(Severity::Warning, QStringLiteral("DZ111"),
                        QStringLiteral("\"%1\" %2").arg(key, note),
                        QStringLiteral("It will not exist in a retail build. Remove it "
                                       "before shipping, or keep it behind the same "
                                       "define."),
                        n.id));
    }
}

// Entities come from the engine, never from `new`. This is the single most
// expensive mistake the graph can express: it compiles, and then every call on
// the result hits a script object with no engine entity behind it.
void entityConstruction(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        QString cls;
        if (n.ref == kNew) {
            cls = bi::castClass(n);
        } else {
            const MethodSig *m = ctx.sig(n.id);
            if (m && (m->flags & flag::Ctor)) cls = m->owner;
        }
        if (cls.isEmpty() || ctx.cat.classId(cls) < 0) continue;
        if (!ctx.cat.isA(cls, QStringLiteral("Object"))) continue;

        out.append(diag(Severity::Error, QStringLiteral("DZ112"),
                        QStringLiteral("%1 is an entity and cannot be created with "
                                       "`new`. You would get a script object with no "
                                       "engine entity behind it.").arg(cls),
                        QStringLiteral("Use a Spawn Entity node "
                                       "(GetGame().CreateObjectEx), or take a reference "
                                       "to an entity that already exists."),
                        n.id));
    }
}

// A constructed object nobody keeps is collected the moment the call ends.
void discardedConstruction(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        const MethodSig *m = ctx.sig(n.id);
        const bool isCtor = m && (m->flags & flag::Ctor);
        if (n.ref != kNew && !isCtor) continue;

        const QString cls = n.ref == kNew ? bi::castClass(n) : m->owner;
        if (!cls.isEmpty() && ctx.cat.classId(cls) >= 0
            && ctx.cat.isA(cls, QStringLiteral("Object")))
            continue; // already an error

        bool used = false;
        for (const Link &l : ctx.linksFrom(n.id))
            if (l.fromPin == QLatin1String("ret")) { used = true; break; }
        if (used) continue;

        out.append(diag(Severity::Warning, QStringLiteral("DZ113"),
                        QStringLiteral("The new %1 is discarded immediately: nothing is "
                                       "wired to its output.")
                            .arg(cls.isEmpty() ? QStringLiteral("object") : cls),
                        QStringLiteral("Store it in a class variable if it needs to live "
                                       "on, or remove the node."),
                        n.id));
    }
}

// The text a raw node contributes is pasted into the .c file exactly as it
// stands, so an imbalance here is a compile failure in the generated script
// with nothing on screen to say where it came from.
QString imbalanceText(int balance, const QString &one, const QString &many,
                      const QString &open, const QString &close)
{
    const int n = qAbs(balance);
    const QString noun = n == 1 ? one : many;
    return balance > 0
               ? QStringLiteral("opens %1 %2 (`%3`) that it never closes")
                     .arg(n).arg(noun, open)
               : QStringLiteral("closes %1 %2 (`%3`) that it never opens")
                     .arg(n).arg(noun, close);
}

void rawCodeSyntax(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const QString balanceHint =
        QStringLiteral("A raw node has to close everything it opens. Its text goes "
                       "into the generated file as it stands, so the imbalance lands "
                       "in the .c and the compiler reports it somewhere else "
                       "entirely.");

    for (const RawBlock &b : ctx.raws) {
        // An unreachable node generates nothing, and DZ201 already speaks for
        // it, so a broken brace in one is not a compile failure yet.
        if (!b.node || !ctx.reachable.contains(b.node->id)) continue;

        // An unclosed quote swallows the rest of the line, brackets included,
        // so the balance it reports is a consequence of the quote rather than a
        // second mistake. Reporting both sends the reader after the wrong one.
        if (b.scan.unterminatedString || b.scan.unterminatedComment) {
            if (b.scan.unterminatedString)
                out.append(diag(Severity::Error, QStringLiteral("DZ115"),
                                QStringLiteral("This raw code opens a string that is "
                                               "never closed."),
                                QStringLiteral("Add the closing quote. A quote inside a "
                                               "string has to be written as \\\", and a "
                                               "string cannot run past the end of its "
                                               "line."),
                                b.node->id));
            if (b.scan.unterminatedComment)
                out.append(diag(Severity::Error, QStringLiteral("DZ115"),
                                QStringLiteral("This raw code opens a `/*` comment that "
                                               "is never closed."),
                                QStringLiteral("Close it with `*/`. Everything written "
                                               "after this node in the generated file "
                                               "would be swallowed by the comment."),
                                b.node->id));
            continue;
        }

        if (b.scan.braceBalance != 0)
            out.append(diag(Severity::Error, QStringLiteral("DZ114"),
                            QStringLiteral("This raw code %1.")
                                .arg(imbalanceText(b.scan.braceBalance,
                                                   QStringLiteral("brace"),
                                                   QStringLiteral("braces"),
                                                   QStringLiteral("{"),
                                                   QStringLiteral("}"))),
                            balanceHint, b.node->id));
        if (b.scan.parenBalance != 0)
            out.append(diag(Severity::Error, QStringLiteral("DZ114"),
                            QStringLiteral("This raw code %1.")
                                .arg(imbalanceText(b.scan.parenBalance,
                                                   QStringLiteral("parenthesis"),
                                                   QStringLiteral("parentheses"),
                                                   QStringLiteral("("),
                                                   QStringLiteral(")"))),
                            balanceHint, b.node->id));
    }
}

// True when the tokeniser could not make sense of the block, which means every
// name it reported is a guess.
bool rawIsBroken(const RawBlock &b)
{
    return b.scan.unterminatedString || b.scan.unterminatedComment
           || b.scan.braceBalance != 0 || b.scan.parenBalance != 0;
}

// A name in a Raw node that resolves to nothing the analyser can find.
//
// This is the rule most able to do harm, so it is the one that stays quiet the
// most: only names used as values are considered at all, member fields and
// constants are exempt because the catalogue does not index them, and a script
// built on a class from elsewhere in the project turns the whole rule off.
void rawCodeUnknownName(const Ctx &ctx, QVector<Diagnostic> &out)
{
    if (ctx.raws.isEmpty()) return;
    // A project base class cannot be traced to its members from one graph, so
    // everything this script inherits would read as unresolved. That is the
    // wrong answer far more often than it is the right one.
    const QString ancestor = ctx.ancestorClass;
    if (!ancestor.isEmpty() && ctx.cat.classId(ancestor) < 0) return;

    QSet<QString> known = ctx.rawDeclared;
    known.insert(ctx.graph.className);
    known.insert(ctx.graph.baseClass);
    for (const GraphVariable &v : ctx.graph.variables) known.insert(v.name);
    for (const GraphFunction &f : ctx.graph.functions) {
        known.insert(f.name);
        for (const GraphParam &p : f.params) known.insert(p.name);
    }
    // An event body is generated with the parameter names from the signature,
    // and raw code inside it reads them by those names.
    for (const GraphNode &n : ctx.graph.nodes)
        if (const MethodSig *m = ctx.sig(n.id))
            for (const MethodSig::Param &p : m->params) known.insert(p.name);

    for (const RawBlock &b : ctx.raws) {
        if (!b.node || !ctx.reachable.contains(b.node->id)) continue;
        // DZ114 and DZ115 already have this node, and a block that does not
        // tokenise cleanly will produce nonsense names on top.
        if (rawIsBroken(b)) continue;

        QStringList unknown;
        for (const QString &name : valueIdentifiers(b.sig)) {
            if (unknown.contains(name) || known.contains(name)) continue;
            if (fieldShaped(name) || constantShaped(name) || generatedLocal(name))
                continue;
            if (ctx.cat.classId(name) >= 0) continue;
            // The catalogue scan is the expensive answer, so it goes last.
            if (vanillaName(ctx.cat, name).exists) continue;
            unknown << name;
        }
        if (unknown.isEmpty()) continue;

        // A node full of unknown names is one mistake, not six, and the
        // problem list has to stay readable.
        const int shown = qMin(unknown.size(), 4);
        QStringList quoted;
        for (int i = 0; i < shown; ++i)
            quoted << QStringLiteral("`%1`").arg(unknown.at(i));
        QString names = quoted.join(QStringLiteral(", "));
        if (unknown.size() > shown)
            names += QStringLiteral(" and %1 more").arg(unknown.size() - shown);

        out.append(diag(Severity::Warning, QStringLiteral("DZ116"),
                        QStringLiteral("Cannot resolve %1 in this raw code.").arg(names),
                        QStringLiteral("Nothing this graph declares carries that name, "
                                       "and neither does the vanilla API. Check the "
                                       "spelling, add it in the Class panel, or declare "
                                       "it in the raw code before it is used."),
                        b.node->id));
    }
}

// ------------------------------------------------------------ dead code

// Nothing chains to it, so it never appears in the generated script.
void unreachableNodes(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        if (ctx.reachable.contains(n.id)) continue;
        if (n.kind == NodeKind::Comment || n.ref == bi::Comment) continue;
        const NodeDef *def = ctx.def(n.id);
        out.append(diag(Severity::Warning, QStringLiteral("DZ201"),
                        QStringLiteral("\"%1\" is not connected to anything that runs, so "
                                       "it generates no code.")
                            .arg(def ? def->title : n.ref),
                        QStringLiteral("Chain it from an event or function, or delete it."),
                        n.id));
    }
}

// A pure node exists only for its output; unused, it is decoration.
void unusedPureResult(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        const NodeDef *def = ctx.def(n.id);
        if (!def || !def->pure) continue;
        if (n.kind == NodeKind::Comment || n.ref == bi::Comment) continue;
        if (!ctx.reachable.contains(n.id)) continue; // already reported as unreachable
        if (!ctx.linksFrom(n.id).isEmpty()) continue;
        out.append(diag(Severity::Warning, QStringLiteral("DZ202"),
                        QStringLiteral("\"%1\" produces a value that nothing uses.")
                            .arg(def->title),
                        QStringLiteral("Wire its output, or remove it. A pure node with "
                                       "no consumer emits nothing."),
                        n.id));
    }
}

// A member whose declared type is not a type. `player PlayerRef;` looks
// reasonable in the variable table and fails at compile time with a message
// pointing at the declaration rather than at the name that is wrong.
void unknownVariableType(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphVariable &v : ctx.graph.variables) {
        if (v.id.isEmpty() || v.type.trimmed().isEmpty()) continue;

        const PinType t = pinTypeOf(v.type, [&ctx](const QString &n) {
            return ctx.cat.isEnum(n);
        });
        // Only object pins name a class. Everything else resolved to a
        // primitive, which by definition is spelled correctly.
        if (t.kind != PinKind::Object || t.cls.isEmpty()) continue;
        if (ctx.cat.classId(t.cls) >= 0 || ctx.cat.isEnum(t.cls)) continue;
        // Another script in the same project is a real type this graph cannot
        // see, and typedefs are not indexed, so only complain when nothing in
        // reach could explain the name.
        if (t.cls == ctx.graph.className) continue;

        // A near miss is worth naming: the correct spelling is usually one
        // case change away, and hunting for it in 6,000 classes is the part
        // that wastes the afternoon.
        QString suggestion;
        for (const SearchHit &hit : ctx.cat.search(t.cls, {6, {}, {}})) {
            if (hit.subtitle != QLatin1String("class") && ctx.cat.classId(hit.title) < 0)
                continue;
            suggestion = hit.title;
            break;
        }
        if (suggestion.isEmpty()) {
            const QStringList names = ctx.cat.classNames();
            for (const QString &name : names) {
                if (name.compare(t.cls, Qt::CaseInsensitive) != 0) continue;
                suggestion = name;
                break;
            }
        }

        out.append(diag(Severity::Error, QStringLiteral("DZ117"),
                        QStringLiteral("`%1` is declared as `%2`, which is not a type.")
                            .arg(v.name, v.type),
                        suggestion.isEmpty()
                            ? QStringLiteral("Pick the type from the list in the Variable "
                                             "Manager. Entity types are classes such as "
                                             "PlayerBase or ItemBase.")
                            : QStringLiteral("Did you mean `%1`?").arg(suggestion)));
    }
}

// Declared members that no node ever touches.
void unusedVariables(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphVariable &v : ctx.graph.variables) {
        if (v.id.isEmpty()) continue;
        bool touched = false;
        for (const GraphNode &n : ctx.graph.nodes)
            if (memberIdOf(n) == v.id) { touched = true; break; }
        if (touched) continue;
        // a synced or persisted variable is meaningful even with no graph use,
        // because the engine reads and writes it
        if (v.sync || v.persist) continue;
        if (usedInRawCode(ctx, v.name)) continue;
        out.append(diag(Severity::Info, QStringLiteral("DZ203"),
                        QStringLiteral("Variable `%1` is declared but never read or "
                                       "written.").arg(v.name),
                        QStringLiteral("Drag it onto the graph to use it, or remove it "
                                       "from the Class panel.")));
    }
}

// Functions nobody calls, and which are not overriding anything.
void uncalledFunctions(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphFunction &f : ctx.graph.functions) {
        if (f.id.isEmpty()) continue;
        bool called = false;
        for (const GraphNode &n : ctx.graph.nodes) {
            QString scriptId, fnId;
            if (!splitCallKey(n.ref, &scriptId, &fnId)) continue;
            if (fnId != f.id) continue;
            // A call carrying a different script's id targets that script's
            // function, not ours, even when the two ids happen to match.
            if (!ctx.scriptId.isEmpty() && scriptId != ctx.scriptId) continue;
            called = true;
            break;
        }
        if (called) continue;
        if (f.isOverride) continue;
        // static helpers are typically called from other scripts' raw code
        if (usedInRawCode(ctx, f.name)) continue;
        // a function that shares a name with a vanilla method is probably an
        // override; the catalogue lookup is the expensive check, so it runs last
        if (vanillaName(ctx.cat, f.name).exists) continue;

        out.append(diag(Severity::Info, QStringLiteral("DZ204"),
                        QStringLiteral("%1() is never called.").arg(f.name),
                        QStringLiteral("Call it from an event, or from another script "
                                       "that holds a reference to this one.")));
    }
}

// A Branch with neither side wired is a no-op with extra steps.
void emptyBranch(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        const NodeDef *def = ctx.def(n.id);
        if (!def) continue;
        QVector<QString> execOuts;
        for (const Pin &p : def->pins)
            if (p.dir == PinDir::Out && p.type.kind == PinKind::Exec)
                execOuts.append(p.id);
        if (execOuts.size() < 2) continue;

        QSet<QString> wired;
        for (const Link &l : ctx.linksFrom(n.id)) wired.insert(l.fromPin);
        bool any = false;
        for (const QString &id : execOuts)
            if (wired.contains(id)) { any = true; break; }
        if (any) continue;

        out.append(diag(Severity::Warning, QStringLiteral("DZ205"),
                        QStringLiteral("\"%1\" has nothing wired to any of its outputs, "
                                       "so it decides nothing.").arg(def->title),
                        QStringLiteral("Wire at least one branch, or remove the node."),
                        n.id));
    }
}

// An event whose chain is empty generates a method that only calls super.
void emptyEvent(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode *n : ctx.entries) {
        bool chained = false;
        for (const Link &l : ctx.linksFrom(n->id))
            if (l.fromPin == QLatin1String("exec")) { chained = true; break; }
        if (chained) continue;
        // a function whose body is kept verbatim has no chain by design
        if (n->ref.startsWith(kFnEntry)) {
            const QString id = n->ref.mid(kFnEntry.size());
            bool verbatim = false;
            for (const GraphFunction &f : ctx.graph.functions)
                if (f.id == id) { verbatim = !f.rawBody.isEmpty(); break; }
            if (verbatim) continue;
        }
        const NodeDef *def = ctx.def(n->id);
        out.append(diag(Severity::Info, QStringLiteral("DZ206"),
                        QStringLiteral("\"%1\" has nothing chained to it, so it only "
                                       "calls `super`.").arg(def ? def->title : n->ref),
                        QStringLiteral("Chain nodes from its exec pin, or remove it. An "
                                       "override that only calls super is the same as no "
                                       "override."),
                        n->id));
    }
}

// Writing a variable twice in a row with no read between wastes the first.
void redundantVariableWrite(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &n : ctx.graph.nodes) {
        if (n.kind != NodeKind::VarSet || !ctx.reachable.contains(n.id)) continue;
        const GraphNode *next = nullptr;
        for (const Link &l : ctx.linksFrom(n.id))
            if (l.fromPin == QLatin1String("exec")) { next = l.node; break; }
        if (!next || next->kind != NodeKind::VarSet || next->ref != n.ref) continue;
        const NodeDef *def = ctx.def(n.id);
        out.append(diag(Severity::Warning, QStringLiteral("DZ207"),
                        QStringLiteral("\"%1\" is immediately overwritten by the next "
                                       "node with nothing reading it in between.")
                            .arg(def ? def->title : QStringLiteral("This Set")),
                        QStringLiteral("Remove one of the two writes."),
                        n.id));
    }
}

// A Raw node with nothing in it still sits in the chain looking like a step.
void rawCodeEmpty(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const RawBlock &b : ctx.raws) {
        if (!b.node || !ctx.reachable.contains(b.node->id)) continue;
        // scanEnforce counts one statement for any block that has a token in
        // it, so a zero here means comments and whitespace and nothing else.
        if (b.scan.statements > 0) continue;

        const bool blank = b.code.trimmed().isEmpty();
        out.append(diag(Severity::Info, QStringLiteral("DZ208"),
                        blank ? QStringLiteral("This raw node is empty, so it generates "
                                               "nothing.")
                              : QStringLiteral("This raw node holds only a comment, so it "
                                               "adds no behaviour."),
                        blank ? QStringLiteral("Write the Enforce you want here, or "
                                               "delete the node.")
                              : QStringLiteral("A Comment node is the place for a note "
                                               "that stays on the canvas. Put code here "
                                               "or delete it."),
                        b.node->id));
    }
}

// ----------------------------------------------------------------- DayZ
//
// Nothing here is a compile error. The script builds fine and then misbehaves
// on a live server, which is the expensive kind of wrong. These are the checks
// that need the game's semantics rather than the language's.

// Nodes that can reach `seeds` by following exec flow backwards, and the seeds
// themselves. One pass answers "does anything downstream of me do X" for every
// node at once, instead of a walk per question.
QSet<QString> reachesAny(const Ctx &ctx, const QVector<QString> &seeds)
{
    QSet<QString> seen;
    QVector<QString> queue;
    for (const QString &id : seeds)
        if (!seen.contains(id)) { seen.insert(id); queue.append(id); }
    while (!queue.isEmpty()) {
        const QString cur = queue.takeLast();
        for (const QString &prev : ctx.pred(cur))
            if (!seen.contains(prev)) { seen.insert(prev); queue.append(prev); }
    }
    return seen;
}

// A synced variable that is written but never pushed reaches nobody. A write
// counts whether it comes from a Set node or from a line of raw code, and so
// does the push: a chain ending in a raw SetSynchDirty() used to report every
// Set above it.
void syncWithoutDirty(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const QString dirty = QStringLiteral("SetSynchDirty");
    QHash<QString, const GraphVariable *> syncedByName;
    for (const GraphVariable &v : ctx.graph.variables)
        if (v.sync && !v.id.isEmpty()) syncedByName.insert(v.name, &v);
    if (syncedByName.isEmpty()) return;

    QVector<QString> pushes;
    for (const GraphNode &n : ctx.graph.nodes)
        if (ctx.methodName(n.id) == dirty) pushes.append(n.id);
    for (const RawBlock &b : ctx.raws)
        if (b.node && (b.scan.calls.contains(dirty) || b.scan.members.contains(dirty)))
            pushes.append(b.node->id);
    const QSet<QString> pushesAfter = reachesAny(ctx, pushes);

    const QString hint =
        QStringLiteral("Clients keep the old value until SetSynchDirty() runs. Chain "
                       "one after the last write.");

    for (const GraphNode &n : ctx.graph.nodes) {
        if (n.kind != NodeKind::VarSet || !ctx.reachable.contains(n.id)) continue;
        const GraphVariable *v = variableForRef(ctx.graph, n.ref);
        if (!v || !v->sync || pushesAfter.contains(n.id)) continue;

        out.append(diag(Severity::Warning, QStringLiteral("DZ301"),
                        QStringLiteral("`%1` is network-synced, but nothing calls "
                                       "SetSynchDirty() after this write.").arg(v->name),
                        hint, n.id));
    }

    for (const RawBlock &b : ctx.raws) {
        if (!b.node || !ctx.reachable.contains(b.node->id)) continue;
        if (pushesAfter.contains(b.node->id)) continue;
        for (const QString &name : b.scan.assignedTo) {
            const GraphVariable *v = syncedByName.value(name);
            if (!v) continue;
            out.append(diag(Severity::Warning, QStringLiteral("DZ301"),
                            QStringLiteral("This raw code writes `%1`, which is "
                                           "network-synced, but nothing calls "
                                           "SetSynchDirty() after it.").arg(v->name),
                            hint, b.node->id));
        }
    }
}

// What EntityAI can register for net sync, and nothing else:
//   RegisterNetSyncVariableBool / BoolSignal   entityai.c:2852, 2860
//   RegisterNetSyncVariableInt                 entityai.c:2870
//   RegisterNetSyncVariableFloat               entityai.c:2881
//   RegisterNetSyncVariableObject              entityai.c:2889
// Enums go over the Int registration, which is how vanilla syncs them
// (land_underground_entrance.c:75 registers an EUndegroundEntranceState member
// as an int). Strings, vectors and arrays have no registration at all.
void unsyncableType(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const auto isEnum = [&ctx](const QString &n) { return ctx.cat.isEnum(n); };
    for (const GraphVariable &v : ctx.graph.variables) {
        if (!v.sync) continue;
        const PinType t = pinTypeOf(v.type, isEnum);
        if (!t.isArray)
            switch (t.kind) {
            case PinKind::Bool:
            case PinKind::Int:
            case PinKind::Float:
            case PinKind::Enum:
                continue;
            case PinKind::Object: {
                // pinTypeOf folds `Managed` into an object pin with no class
                // name. Managed sits above Object in the spine, so it never
                // carries the network id the registration needs.
                if (t.cls.isEmpty()) break;
                // A project class cannot be traced to its vanilla base from one
                // graph, so an unknown name is left alone rather than guessed at.
                if (ctx.cat.classId(t.cls) < 0) continue;
                if (!ctx.cat.isA(t.cls, QStringLiteral("Object"))) break;
                out.append(diag(Severity::Info, QStringLiteral("DZ302"),
                                QStringLiteral("`%1` syncs an object reference, which "
                                               "only reaches the client once the engine "
                                               "has given %2 a network id.")
                                    .arg(v.name, t.cls),
                                QStringLiteral("RegisterNetSyncVariableObject does not "
                                               "handle the object despawning on the "
                                               "client either, so the reference can go "
                                               "stale. Syncing an int the client can "
                                               "look the object up from is steadier.")));
                continue;
            }
            default:
                break;
            }

        out.append(diag(Severity::Error, QStringLiteral("DZ302"),
                        QStringLiteral("`%1` is marked synced but %2 cannot be "
                                       "network-synced.").arg(v.name, v.type),
                        QStringLiteral("EntityAI registers bool, int, float and object "
                                       "members only. Sync a primitive that describes "
                                       "the state instead, or send it over RPC.")));
    }
}

// Authoritative work on the client is either ignored or desyncs. Events fire on
// both sides, so anything that changes world state needs a guard above it.
const QSet<QString> &serverOnlyMethods()
{
    static const QSet<QString> names = {
        QStringLiteral("SetHealth"), QStringLiteral("AddHealth"),
        QStringLiteral("DecreaseHealth"), QStringLiteral("SetQuantity"),
        QStringLiteral("AddQuantity"), QStringLiteral("CreateObject"),
        QStringLiteral("CreateObjectEx"), QStringLiteral("CreateInInventory"),
        QStringLiteral("Delete"), QStringLiteral("SetSynchDirty"),
        QStringLiteral("SetAllowDamage"), QStringLiteral("SetPosition"),
        QStringLiteral("ProcessDirectDamage"), QStringLiteral("DeleteSafe"),
    };
    return names;
}

void missingServerGuard(const Ctx &ctx, QVector<Diagnostic> &out)
{
    // Which side of a test is the server side. Only the pin that actually runs
    // there counts: seeding both pins of an IsServer Branch reads a world write
    // on the else side as guarded, which is the exact mistake the rule exists
    // to catch.
    QVector<QString> seeds;
    for (const GraphNode &n : ctx.graph.nodes) {
        if (n.ref == kServerOnly) {
            // A straight early-out, so the whole chain after it is server-side.
            for (const Link &l : ctx.linksFrom(n.id))
                if (l.fromPin == QLatin1String("exec")) seeds.append(l.node->id);
            continue;
        }
        if (n.ref != bi::Branch) continue;
        const Link *cond = ctx.linkInto(n.id, QStringLiteral("cond"));
        if (!cond) continue;

        // A Not node is the only inversion the graph can express without a
        // second call, and it swaps which pin is the server one.
        bool negated = false;
        const GraphNode *test = cond->node;
        if (test->ref == kNot) {
            const Link *inner = ctx.linkInto(test->id, QStringLiteral("a"));
            if (!inner) continue;
            negated = true;
            test = inner->node;
        }

        const QString name = ctx.methodName(test->id);
        QString pin;
        if (name == QLatin1String("IsServer")
            || name == QLatin1String("IsDedicatedServer"))
            pin = negated ? QStringLiteral("false") : QStringLiteral("true");
        else if (name == QLatin1String("IsClient"))
            pin = negated ? QStringLiteral("true") : QStringLiteral("false");
        else
            continue;

        for (const Link &l : ctx.linksFrom(n.id))
            if (l.fromPin == pin) seeds.append(l.node->id);
    }
    QSet<QString> guarded;
    collectFrom(ctx, seeds, guarded);

    // One badge per node: a second unguarded SetHealth is a second mistake, and
    // deduping by method name left every one after the first without a squiggle.
    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        const bool isSpawn = n.ref == kSpawn;
        const MethodSig *m = ctx.sig(n.id);
        if (!isSpawn && !(m && serverOnlyMethods().contains(m->name))) continue;
        if (guarded.contains(n.id)) continue;

        const QString label = isSpawn ? QStringLiteral("Spawn Entity") : m->name;
        out.append(diag(Severity::Warning, QStringLiteral("DZ303"),
                        QStringLiteral("\"%1\" changes world state but nothing guards "
                                       "this path to the server.").arg(label),
                        QStringLiteral("Events run on client and server. Put a Server "
                                       "Only node upstream, or branch on "
                                       "GetGame().IsServer()."),
                        n.id));
    }
}

// A Timer that is never Run does nothing at all. The tree declares sixteen
// unrelated Run methods (WorkbenchPlugin, TestHarness, the editor plugins), so
// the owner has to match Timer and the call has to point at THIS timer, or one
// stray Run silences the rule for every timer in the graph.
void timerNeverRun(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const auto isTimer = [&ctx](const QString &cls) {
        return ctx.isA(cls, QStringLiteral("Timer"));
    };

    QSet<QString> started; // identity of every timer a Run() call is aimed at
    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        const MethodSig *m = ctx.sig(n.id);
        if (!m || m->name != QLatin1String("Run") || !isTimer(m->owner)) continue;
        const Link *t = ctx.linkInto(n.id, QStringLiteral("target"));
        // An unwired target says nothing about which timer is meant, and the
        // catalogue flags Timer::Run as an event so the node may not even carry
        // a target pin. Stay quiet rather than accuse a timer that does run.
        if (!t) return;
        started.insert(valueIdentity(*t));
    }

    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        const MethodSig *m = ctx.sig(n.id);
        QString cls;
        if (m && (m->flags & flag::Ctor)) cls = m->owner;
        else if (n.ref == kNew) cls = bi::castClass(n);
        if (!isTimer(cls)) continue;

        // Where the new timer ends up: its own output, plus the member it is
        // stored in. Anything that hands it to code the analyser cannot read
        // ends the question, because Run() may well be in there.
        QSet<QString> identities{QStringLiteral("pin:") + n.id + QLatin1String("/ret")};
        bool opaque = false;
        for (const Link &l : ctx.linksFrom(n.id)) {
            if (l.fromPin != QLatin1String("ret")) continue;
            const QString member = memberIdOf(*l.node);
            if (!member.isEmpty()) {
                identities.insert(QStringLiteral("var:") + member);
                const GraphVariable *v = variableForRef(ctx.graph, l.node->ref);
                if (v && usedInRawCode(ctx, v->name)) opaque = true;
            } else if (l.node->ref == bi::Raw || l.node->ref == kRawExpr
                       || !resolvableHere(*l.node)) {
                opaque = true;
            }
        }
        if (opaque) continue;

        bool runs = false;
        for (const QString &id : identities)
            if (started.contains(id)) { runs = true; break; }
        if (runs) continue;

        out.append(diag(Severity::Warning, QStringLiteral("DZ304"),
                        QStringLiteral("This Timer is created but Run() is never called "
                                       "on it, so it never fires."),
                        QStringLiteral("Call Run(duration, this, \"MethodName\") on it. "
                                       "The constructor argument is the call category, "
                                       "not a delay."),
                        n.id));
    }
}

// Persisted state that is not synced never reaches the client.
void persistNotSynced(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphVariable &v : ctx.graph.variables) {
        if (!v.persist || v.sync) continue;
        if (v.type != QLatin1String("bool") && v.type != QLatin1String("int")
            && v.type != QLatin1String("float"))
            continue;
        out.append(diag(Severity::Info, QStringLiteral("DZ305"),
                        QStringLiteral("`%1` is saved to storage but not synced.")
                            .arg(v.name),
                        QStringLiteral("It survives a restart on the server, but clients "
                                       "never see it. Mark it synced too if the client "
                                       "needs to react to it.")));
    }
}

// Dropping super in a modded override replaces vanilla behaviour wholesale.
void moddedWithoutSuper(const Ctx &ctx, QVector<Diagnostic> &out)
{
    if (!ctx.graph.modded) return;
    for (const GraphNode *n : ctx.entries) {
        if (n->opts.value(QStringLiteral("noSuper")) != QLatin1String("1")) continue;
        const NodeDef *def = ctx.def(n->id);
        out.append(diag(Severity::Warning, QStringLiteral("DZ306"),
                        QStringLiteral("\"%1\" skips `super` in a modded class, which "
                                       "discards the vanilla behaviour entirely.")
                            .arg(def ? def->title : n->ref),
                        QStringLiteral("Unless you deliberately mean to replace it, turn "
                                       "super back on, and expect conflicts with other "
                                       "mods that touch the same method."),
                        n->id));
    }
}

// The container calls that change how many elements there are. Set(), Sort(),
// Invert() and the other reordering helpers leave the length alone, so they do
// not break an iteration and are deliberately absent.
const QSet<QString> &containerResizers()
{
    static const QSet<QString> names = {
        QStringLiteral("Insert"), QStringLiteral("InsertAt"),
        QStringLiteral("InsertAll"), QStringLiteral("InsertArray"),
        QStringLiteral("InsertSet"), QStringLiteral("Remove"),
        QStringLiteral("RemoveOrdered"), QStringLiteral("RemoveElement"),
        QStringLiteral("RemoveItem"), QStringLiteral("RemoveItemUnOrdered"),
        QStringLiteral("RemoveItems"), QStringLiteral("Clear"),
        QStringLiteral("Resize"), QStringLiteral("Copy"),
        QStringLiteral("Swap"), QStringLiteral("Init"),
    };
    return names;
}

// The four templates in 1_core/proto/enscript.c. Plenty of gameplay classes
// declare an Insert or a Remove of their own (ScriptInvoker, Container), and
// those are not the thing being walked.
bool isContainerClass(const QString &owner)
{
    return owner == QLatin1String("array") || owner == QLatin1String("set")
           || owner == QLatin1String("map") || owner == QLatin1String("multiMap");
}

// Adding to or removing from the array a For Each is walking. This is the real
// "delete mid-iteration" crash. Note that it is the container that must not
// change: EntityAI::Delete() is deferred onto the call queue (object.c:82), so
// calling Delete() on the item the loop handed you is safe.
void mutatedWhileIterating(const Ctx &ctx, QVector<Diagnostic> &out)
{
    for (const GraphNode &loop : ctx.graph.nodes) {
        if (loop.ref != bi::ForEach || !ctx.reachable.contains(loop.id)) continue;
        const Link *src = ctx.linkInto(loop.id, QStringLiteral("array"));
        if (!src) continue;
        const QString subject = valueIdentity(*src);

        // Anything the `done` pin also reaches runs outside the loop as well;
        // codegen would not put it in the body, so it is not this rule's to call.
        QSet<QString> body = flowFrom(ctx, loop.id, QStringLiteral("body"));
        for (const QString &after : flowFrom(ctx, loop.id, QStringLiteral("done")))
            body.remove(after);
        if (body.isEmpty()) continue;

        // Walking the node list rather than the set keeps the findings in the
        // order the nodes sit in, so the problem list reads the same way twice.
        for (const GraphNode &n : ctx.graph.nodes) {
            if (!body.contains(n.id)) continue;
            const MethodSig *m = ctx.sig(n.id);
            if (!m || !isContainerClass(m->owner)
                || !containerResizers().contains(m->name))
                continue;
            const Link *target = ctx.linkInto(n.id, QStringLiteral("target"));
            if (!target || valueIdentity(*target) != subject) continue;

            out.append(diag(Severity::Error, QStringLiteral("DZ307"),
                            QStringLiteral("%1() changes the array this For Each is "
                                           "walking, which crashes partway through.")
                                .arg(m->name),
                            QStringLiteral("Collect what you want to change into a "
                                           "second array inside the loop, then act on "
                                           "that array after the loop has finished. "
                                           "Calling Delete() on the item itself is "
                                           "fine, because the engine defers it."),
                            n.id));
        }
    }
}

// An event node generates `override void X()` with a `super.X()` inside it, so
// the class has to inherit X from somewhere. Placing an EntityAI hook on a
// plain Managed helper does not compile, and the message the compiler gives is
// not one a beginner can act on.
void eventNotInherited(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const QString cls = ctx.ancestorClass;
    if (cls.isEmpty() || ctx.cat.classId(cls) < 0) return;

    for (const GraphNode &n : ctx.graph.nodes) {
        if (n.kind != NodeKind::Event) continue;
        const MethodSig *m = ctx.sig(n.id);
        if (!m || m->owner.isEmpty()) continue;
        const int ownerId = ctx.cat.classId(m->owner);
        if (ownerId < 0) continue;
        // The catalogue marks array::Insert, Timer::Run and friends as events
        // by heuristic, and 1_core is engine templates rather than hooks anyone
        // overrides. DZ110 already speaks for the proto natives among them.
        if (ctx.cat.classInfo(ownerId).module == QLatin1String("1_core")) continue;
        if (ctx.cat.isA(cls, m->owner)) continue;

        out.append(diag(Severity::Error, QStringLiteral("DZ308"),
                        QStringLiteral("%1 does not inherit %2::%3(), so overriding it "
                                       "here will not compile.")
                            .arg(cls, m->owner, m->name),
                        QStringLiteral("Pick an event declared on %1 or one of its base "
                                       "classes. If you meant to call %2() on some other "
                                       "object rather than override it, use a call node "
                                       "and wire that object into its target pin.")
                            .arg(cls, m->name),
                        n.id));
    }
}

// Load order: 1_Core, 2_GameLib, 3_Game, 4_World, 5_Mission, each module seeing
// only what came before it. A mod registered in 3_Game cannot name ItemBase,
// because 4_world/entities/itembase.c has not been read yet, and the compiler
// error for that is an unhelpful "unknown type".
void moduleLoadOrder(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const int here = moduleRank(ctx.graph.module);
    if (here < 0) return;

    const auto moduleOf = [&ctx](const QString &cls) {
        const int id = ctx.cat.classId(cls);
        return id < 0 ? QString() : ctx.cat.classInfo(id).module;
    };
    const QString advice =
        QStringLiteral("Script modules compile in the order 1_Core, 2_GameLib, 3_Game, "
                       "4_World, 5_Mission, and each one only sees what came before it. "
                       "Move this script to a later module, or use a class that is "
                       "already declared by this point.");

    const QString base = ctx.ancestorClass;
    const QString baseModule = moduleOf(base);
    if (moduleRank(baseModule) > here)
        out.append(diag(Severity::Error, QStringLiteral("DZ309"),
                        QStringLiteral("This script is registered in %1, but %2 is "
                                       "declared in %3, which loads after it.")
                            .arg(ctx.graph.module, base, baseModule),
                        advice));

    // One finding per class, anchored to the first node that names it, so a
    // graph full of ItemBase calls does not fill the problem list.
    QSet<QString> reported;
    for (const GraphNode &n : ctx.graph.nodes) {
        if (!ctx.reachable.contains(n.id)) continue;
        QString cls;
        if (n.ref == kNew || n.ref == bi::Cast) cls = bi::castClass(n);
        else if (const MethodSig *m = ctx.sig(n.id)) cls = m->owner;
        if (cls.isEmpty() || cls == base || reported.contains(cls)) continue;

        const QString mod = moduleOf(cls);
        if (moduleRank(mod) <= here) continue;
        reported.insert(cls);
        out.append(diag(Severity::Error, QStringLiteral("DZ310"),
                        QStringLiteral("%1 is declared in %2, which loads after this "
                                       "script's module (%3).")
                            .arg(cls, mod, ctx.graph.module),
                        advice, n.id));
    }
}

// RegisterNetSyncVariable* (entityai.c:2852-2889) and OnStoreSave/OnStoreLoad
// (entityai.c:2930, 2994) are declared on EntityAI and nowhere else, so a
// helper class with a synced or saved member generates calls to API it does not
// have.
void syncOnNonEntity(const Ctx &ctx, QVector<Diagnostic> &out)
{
    bool synced = false;
    bool persisted = false;
    for (const GraphVariable &v : ctx.graph.variables) {
        synced = synced || v.sync;
        persisted = persisted || v.persist;
    }
    if (!synced && !persisted) return;

    // A project base class cannot be traced to its vanilla root from one graph.
    const QString cls = ctx.ancestorClass;
    if (cls.isEmpty() || ctx.cat.classId(cls) < 0) return;
    if (ctx.cat.isA(cls, QStringLiteral("EntityAI"))) return;

    const QString what = synced && persisted
                             ? QStringLiteral("synced and saved")
                             : (synced ? QStringLiteral("synced")
                                       : QStringLiteral("saved"));
    out.append(diag(Severity::Error, QStringLiteral("DZ311"),
                    QStringLiteral("%1 is not an entity, so the %2 members will not "
                                   "compile.").arg(ctx.graph.className, what),
                    QStringLiteral("RegisterNetSyncVariable, OnStoreSave and OnStoreLoad "
                                   "are declared on EntityAI. Either base this script on "
                                   "an entity, or clear the sync and save boxes and have "
                                   "the entity that owns this object carry the state.")));
}

// modded class on an engine container or math type. The methods behind those
// names are `proto native` with no script body, so nothing an override adds can
// take effect.
void moddedEngineType(const Ctx &ctx, QVector<Diagnostic> &out)
{
    if (!ctx.graph.modded) return;
    const int id = ctx.cat.classId(ctx.graph.className);
    if (id < 0) return;
    const ClassInfo info = ctx.cat.classInfo(id);
    if (!info.valid || !info.file.contains(QLatin1String("1_core/proto/"))) return;

    out.append(diag(Severity::Warning, QStringLiteral("DZ312"),
                    QStringLiteral("%1 is an engine type declared in %2, not gameplay "
                                   "script.").arg(info.name, info.file),
                    QStringLiteral("Its methods are implemented in the engine binary, so "
                                   "a modded class cannot change any of them. Mod the "
                                   "gameplay class that uses it instead.")));
}

// A function declared in the Class panel that lands on a `proto native` method
// of the base class. DZ110 catches this for event nodes; the Class panel is the
// other way in, and the compiler rejects both.
void functionOverridesNative(const Ctx &ctx, QVector<Diagnostic> &out)
{
    const QString cls = ctx.ancestorClass;
    if (cls.isEmpty() || ctx.cat.classId(cls) < 0) return;

    for (const GraphFunction &f : ctx.graph.functions) {
        if (f.name.isEmpty() || f.isStatic) continue;
        for (const auto &m : vanillaName(ctx.cat, f.name).methods) {
            if (!m.second || !ctx.cat.isA(cls, m.first)) continue;

            out.append(diag(Severity::Error, QStringLiteral("DZ313"),
                            QStringLiteral("%1() is inherited from %2 as `proto native`, "
                                           "which is implemented in the engine and "
                                           "cannot be overridden.").arg(f.name, m.first),
                            QStringLiteral("Rename this function and call %1() from it, "
                                           "or hook an event that the engine does route "
                                           "through script.").arg(f.name)));
            break;
        }
    }
}

} // namespace

// --------------------------------------------------------------- driver

AnalysisResult analyzeGraph(const Graph &graph, const Catalog &cat,
                            const Builtins &builtins, const QString &scriptId)
{
    const Ctx ctx(graph, cat, builtins, scriptId);

    AnalysisResult result;
    QVector<Diagnostic> &out = result.diagnostics;

    unresolvedNodes(ctx, out);
    baseClassExists(ctx, out);
    pinTypeMismatch(ctx, out);
    missingRequiredInput(ctx, out);
    execCycle(ctx, out);
    functionMustReturn(ctx, out);
    cannotOverrideNative(ctx, out);
    entityConstruction(ctx, out);
    discardedConstruction(ctx, out);
    guardedApiInShippingCode(ctx, out);
    rawCodeSyntax(ctx, out);
    rawCodeUnknownName(ctx, out);

    unreachableNodes(ctx, out);
    unusedPureResult(ctx, out);
    unknownVariableType(ctx, out);
    unusedVariables(ctx, out);
    uncalledFunctions(ctx, out);
    emptyBranch(ctx, out);
    emptyEvent(ctx, out);
    redundantVariableWrite(ctx, out);
    rawCodeEmpty(ctx, out);

    syncWithoutDirty(ctx, out);
    unsyncableType(ctx, out);
    missingServerGuard(ctx, out);
    timerNeverRun(ctx, out);
    persistNotSynced(ctx, out);
    moddedWithoutSuper(ctx, out);
    mutatedWhileIterating(ctx, out);
    eventNotInherited(ctx, out);
    moduleLoadOrder(ctx, out);
    syncOnNonEntity(ctx, out);
    moddedEngineType(ctx, out);
    functionOverridesNative(ctx, out);

    // Worst first, then by rule id; stable so findings from one rule keep the
    // order the nodes sit in, which is what makes the problem list read the
    // same way twice.
    std::stable_sort(out.begin(), out.end(),
                     [](const Diagnostic &a, const Diagnostic &b) {
                         const int ra = severityRank(a.severity);
                         const int rb = severityRank(b.severity);
                         return ra != rb ? ra < rb : a.rule < b.rule;
                     });

    for (const Diagnostic &d : out) {
        if (d.severity == Severity::Error) result.errors++;
        else if (d.severity == Severity::Warning) result.warnings++;
    }
    return result;
}
