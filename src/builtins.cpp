#include "builtins.h"

#include <QHash>

#include "catalog.h"

#include <QStringList>

namespace bi {
const QString Begin    = QStringLiteral("bi.begin");
const QString End      = QStringLiteral("bi.end");
const QString Branch   = QStringLiteral("bi.branch");
const QString Sequence = QStringLiteral("bi.sequence");
const QString ForLoop  = QStringLiteral("bi.forLoop");
const QString ForEach  = QStringLiteral("bi.forEach");
const QString While    = QStringLiteral("bi.while");
const QString Return   = QStringLiteral("bi.return");
const QString Comment  = QStringLiteral("bi.comment");
const QString Raw      = QStringLiteral("bi.raw");
const QString Cast     = QStringLiteral("bi.cast");
const QString Literal  = QStringLiteral("bi.literal");
const QString Print    = QStringLiteral("bi.print");

QString castClass(const GraphNode &node)
{
    const QString cls = node.opts.value(QStringLiteral("cls"));
    return cls.isEmpty() ? node.opts.value(QStringLiteral("class")) : cls;
}
} // namespace bi

namespace {

// Ids the header does not name but the reference build writes into .sdzn, so
// they have to keep working in both directions.
const QString IdSuper      = QStringLiteral("bi.super");
const QString IdSelf       = QStringLiteral("bi.self");
const QString IdLitBool    = QStringLiteral("bi.litBool");
const QString IdLitInt     = QStringLiteral("bi.litInt");
const QString IdLitFloat   = QStringLiteral("bi.litFloat");
const QString IdLitString  = QStringLiteral("bi.litString");
const QString IdLitVector  = QStringLiteral("bi.litVector");
const QString IdLitClass   = QStringLiteral("bi.litClass");
const QString IdOp         = QStringLiteral("bi.op");
const QString IdNot        = QStringLiteral("bi.not");
const QString IdSelect     = QStringLiteral("bi.select");
const QString IdNew        = QStringLiteral("bi.new");
const QString IdSpawn      = QStringLiteral("bi.spawn");
const QString IdServerOnly = QStringLiteral("bi.serverOnly");
const QString IdRawExpr    = QStringLiteral("bi.rawExpr");

// Palette groups. Builtins are the only nodes that choose their own category;
// catalogue nodes are grouped by what they are (Events, Functions, Enums).
const QString CatLifecycle = QStringLiteral("Lifecycle");
const QString CatFlow      = QStringLiteral("Flow");
const QString CatVariables = QStringLiteral("Variables");
const QString CatOperators = QStringLiteral("Operators");
const QString CatLiterals  = QStringLiteral("Literals");
const QString CatCasting   = QStringLiteral("Casting");
const QString CatUtility   = QStringLiteral("Utility");

// The escape-hatch nodes get their own accent rather than one of the roles in
// accents::. They are not a category of DayZ API, they are a way around it.
QColor rawAccent() { return QColor("#3a2c4a"); }

PinType execType() { return {PinKind::Exec, {}, false}; }
PinType prim(PinKind kind) { return {kind, {}, false}; }
PinType obj(const QString &cls) { return {PinKind::Object, cls, false}; }
PinType anyArray() { return {PinKind::Any, {}, true}; }

Pin execPin(const QString &id, const QString &label, PinDir dir)
{
    Pin p;
    p.id = id;
    p.label = label;
    p.dir = dir;
    p.type = execType();
    return p;
}

// An input that can be typed into carries its literal, so a graph with nothing
// wired still generates code that compiles. Same rule as Catalog::makePin.
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

NodeDef makeDef(const QString &key, const QString &title, const QString &subtitle,
                const QString &category, const QColor &accent,
                const QVector<Pin> &pins, bool pure = false)
{
    NodeDef d;
    d.key = key;
    d.title = title;
    d.subtitle = subtitle;
    d.category = category;
    d.accent = accent;
    d.pins = pins;
    d.pure = pure;
    d.valid = true;
    return d;
}

// Builtins have no vanilla declaration to document, so the inspector reads
// this off the def. Paragraph breaks, no markup: it is shown as plain text.
QString help(const QString &summary, const QStringList &effects,
             const QStringList &cautions = {})
{
    QStringList out;
    out << summary;
    out << effects;
    for (const QString &c : cautions)
        out << QStringLiteral("Caution: %1").arg(c);
    return out.join(QStringLiteral("\n\n"));
}

bool isLogicalOp(const QString &op)
{
    return op == QLatin1String("&&") || op == QLatin1String("||");
}

bool isComparisonOp(const QString &op)
{
    static const QStringList ops = {
        QStringLiteral("=="), QStringLiteral("!="), QStringLiteral("<"),
        QStringLiteral("<="), QStringLiteral(">"), QStringLiteral(">="),
    };
    return ops.contains(op);
}

// End has no alternatives to pick between, but it is the same kind of thing as
// a Begin mode and the generator needs the signature, so it lives here.
const LifecycleSig &endSignature()
{
    static const LifecycleSig sig = [] {
        LifecycleSig s;
        s.method = QStringLiteral("EEDelete");
        s.ret = QStringLiteral("void");
        s.params = {{QStringLiteral("parent"), QStringLiteral("EntityAI")}};
        s.label = QStringLiteral("On Destroy: EEDelete()");
        s.note = QStringLiteral(
            "The entity is being removed. Clean up timers, effects and anything "
            "you registered.");
        return s;
    }();
    return sig;
}

} // namespace

// Order matters twice over: it is what the operator picker shows, and it is the
// order the reference build lists, arithmetic then comparison then boolean.
// A word for each operator, so searching the palette for "multiply" or
// "divide" finds one. A modder looking for arithmetic does not search for "*".
QString operatorWord(const QString &symbol)
{
    static const QHash<QString, QString> words = {
        {QStringLiteral("+"), QStringLiteral("add")},
        {QStringLiteral("-"), QStringLiteral("subtract")},
        {QStringLiteral("*"), QStringLiteral("multiply")},
        {QStringLiteral("/"), QStringLiteral("divide")},
        {QStringLiteral("%"), QStringLiteral("modulo, remainder")},
        {QStringLiteral("=="), QStringLiteral("equals")},
        {QStringLiteral("!="), QStringLiteral("not equal")},
        {QStringLiteral("<"), QStringLiteral("less than")},
        {QStringLiteral("<="), QStringLiteral("less or equal")},
        {QStringLiteral(">"), QStringLiteral("greater than")},
        {QStringLiteral(">="), QStringLiteral("greater or equal")},
        {QStringLiteral("&&"), QStringLiteral("and")},
        {QStringLiteral("||"), QStringLiteral("or")},
    };
    return words.value(symbol, QStringLiteral("operator"));
}

QString operatorNote(const QString &symbol)
{
    if (symbol == QLatin1String("/"))
        return QStringLiteral("Dividing two ints truncates in Enforce, so make one of "
                              "them a float when you want a fraction.");
    if (symbol == QLatin1String("+"))
        return QStringLiteral("On strings this joins them, and a number joined to a "
                              "string converts itself.");
    if (symbol == QLatin1String("%"))
        return QStringLiteral("Integers only.");
    if (symbol == QLatin1String("&&") || symbol == QLatin1String("||"))
        return QStringLiteral("Both sides are bools, and the result is a bool.");
    return QStringLiteral("Outputs a bool.") ;
}

const QStringList &Builtins::binaryOperators()
{
    static const QStringList ops = {
        QStringLiteral("+"), QStringLiteral("-"), QStringLiteral("*"),
        QStringLiteral("/"), QStringLiteral("%"),
        QStringLiteral("=="), QStringLiteral("!="), QStringLiteral("<"),
        QStringLiteral("<="), QStringLiteral(">"), QStringLiteral(">="),
        QStringLiteral("&&"), QStringLiteral("||"),
    };
    return ops;
}

// The types the Literal node can take. Anything here has to survive
// pinTypeOf() and come back out of the generator as a valid Enforce literal,
// which rules out object and enum types: those are a Cast or a catalogue node,
// not something you type into a box.
const QStringList &Builtins::literalTypes()
{
    static const QStringList types = {
        QStringLiteral("bool"), QStringLiteral("int"), QStringLiteral("float"),
        QStringLiteral("string"), QStringLiteral("vector"),
        QStringLiteral("typename"),
    };
    return types;
}

bool Builtins::operatorYieldsBool(const QString &op)
{
    return isLogicalOp(op) || isComparisonOp(op);
}

Builtins::Builtins()
{
    const auto mode = [](const QString &method, bool ctor, const QString &label,
                         const QString &note) {
        LifecycleSig s;
        s.method = method;
        s.ret = QStringLiteral("void");
        s.ctor = ctor;
        s.label = label;
        s.note = note;
        return s;
    };

    // Enforce has no single "begin play". Picking the wrong moment is the
    // classic source of "my code runs but nothing happens", so the note on each
    // mode says what it is actually for. Inserted earliest moment first: that is
    // the order beginModeOrder() hands to the picker.
    addBeginMode(QStringLiteral("init"),
        mode(QStringLiteral("EEInit"), false,
             QStringLiteral("On Init: EEInit()"),
             QStringLiteral("The entity exists and is set up. The usual choice, "
                            "and the closest thing to Begin Play.")));
    addBeginMode(QStringLiteral("construct"),
        mode(QString(), true,
             QStringLiteral("On Construct: constructor"),
             QStringLiteral("Earliest point, before the entity is initialised. "
                            "Good for registering sync variables, wrong for "
                            "anything that touches attachments or config.")));
    addBeginMode(QStringLiteral("deferred"),
        mode(QStringLiteral("DeferredInit"), false,
             QStringLiteral("Deferred Init: DeferredInit()"),
             QStringLiteral("Runs a frame after init, once the world around the "
                            "entity is settled. Use it when On Init turns out to "
                            "be too early.")));
    addBeginMode(QStringLiteral("afterLoad"),
        mode(QStringLiteral("AfterStoreLoad"), false,
             QStringLiteral("After Load: AfterStoreLoad()"),
             QStringLiteral("Runs once persisted values have been restored. Use "
                            "it to re-apply state a saved item came back with.")));

    // ------------------------------------------------------------- lifecycle
    NodeDef begin = makeDef(bi::Begin, QStringLiteral("Begin"),
                            QStringLiteral("runs once on init"),
                            CatLifecycle, accents::event(),
                            {execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                             dataPin(QStringLiteral("self"), QStringLiteral("self"),
                                     PinDir::Out, obj(QStringLiteral("auto")))});
    begin.doc = help(
        QStringLiteral("Where a script starts. The first thing to wire on a new class."),
        {QStringLiteral("Pick the moment in Details; the node becomes that Enforce "
                        "method with `super` called for you."),
         QStringLiteral("Defaults to `EEInit()`, which fires once the entity exists "
                        "and is set up.")},
        {QStringLiteral("Runs on client and server. Guard anything authoritative "
                        "with `GetGame().IsServer()`."),
         QStringLiteral("On Construct is too early to touch attachments, config "
                        "values or the inventory.")});
    add(begin);

    NodeDef end = makeDef(bi::End, QStringLiteral("End"),
                          QStringLiteral("runs once on destroy"),
                          CatLifecycle, accents::event(),
                          {execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                           dataPin(QStringLiteral("self"), QStringLiteral("self"),
                                   PinDir::Out, obj(QStringLiteral("auto"))),
                           dataPin(QStringLiteral("parent"), QStringLiteral("parent"),
                                   PinDir::Out, obj(QStringLiteral("EntityAI")))});
    end.doc = help(
        QStringLiteral("Where a script cleans up. The entity is being destroyed."),
        {QStringLiteral("Becomes `override void EEDelete(EntityAI parent)`, with "
                        "`super` called for you."),
         QStringLiteral("The `parent` pin is whatever the entity was attached to, "
                        "if anything.")},
        {QStringLiteral("Kill timers, effects and anything you registered here, or "
                        "they outlive the entity.")});
    add(end);

    // ------------------------------------------------------------------ flow
    NodeDef branch = makeDef(bi::Branch, QStringLiteral("Branch"),
                             QStringLiteral("if / else"), CatFlow, accents::flow(),
                             {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                              dataPin(QStringLiteral("cond"), QStringLiteral("condition"),
                                      PinDir::In, prim(PinKind::Bool)),
                              execPin(QStringLiteral("true"), QStringLiteral("true"),
                                      PinDir::Out),
                              execPin(QStringLiteral("false"), QStringLiteral("false"),
                                      PinDir::Out)});
    branch.doc = help(
        QStringLiteral("Splits the flow in two based on a condition: an `if / else`."),
        {QStringLiteral("Emits `if (condition) { ... } else { ... }` around whatever each "
                        "exec pin leads to."),
         QStringLiteral("Leaving the false pin unwired emits a plain `if` with no "
                        "`else`.")});
    add(branch);

    NodeDef sequence = makeDef(bi::Sequence, QStringLiteral("Sequence"),
                               QStringLiteral("run in order"), CatFlow, accents::flow(),
                               {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                                execPin(QStringLiteral("then0"), QStringLiteral("then 0"),
                                        PinDir::Out),
                                execPin(QStringLiteral("then1"), QStringLiteral("then 1"),
                                        PinDir::Out),
                                execPin(QStringLiteral("then2"), QStringLiteral("then 2"),
                                        PinDir::Out)});
    sequence.doc = help(
        QStringLiteral("Runs several chains one after another from a single trigger."),
        {QStringLiteral("Each `then` pin runs to completion in order. Useful when one "
                        "event drives unrelated work.")});
    add(sequence);

    NodeDef forLoop = makeDef(bi::ForLoop, QStringLiteral("For Loop"),
                              QStringLiteral("int counter"), CatFlow, accents::flow(),
                              {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                               dataPin(QStringLiteral("first"), QStringLiteral("first"),
                                       PinDir::In, prim(PinKind::Int)),
                               dataPin(QStringLiteral("last"), QStringLiteral("last"),
                                       PinDir::In, prim(PinKind::Int)),
                               execPin(QStringLiteral("body"), QStringLiteral("body"),
                                       PinDir::Out),
                               dataPin(QStringLiteral("index"), QStringLiteral("index"),
                                       PinDir::Out, prim(PinKind::Int)),
                               execPin(QStringLiteral("done"), QStringLiteral("done"),
                                       PinDir::Out)});
    forLoop.doc = help(
        QStringLiteral("Counts from first to last, running the body each time."),
        {QStringLiteral("Emits `for (int i = first; i < last; i++)`."),
         QStringLiteral("The `index` pin carries the counter into the body.")},
        {QStringLiteral("`last` is exclusive, so use the array size, not size minus "
                        "one.")});
    add(forLoop);

    NodeDef forEach = makeDef(bi::ForEach, QStringLiteral("For Each"),
                              QStringLiteral("iterate an array"), CatFlow, accents::flow(),
                              {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                               dataPin(QStringLiteral("array"), QStringLiteral("array"),
                                       PinDir::In, anyArray()),
                               execPin(QStringLiteral("body"), QStringLiteral("body"),
                                       PinDir::Out),
                               dataPin(QStringLiteral("item"), QStringLiteral("item"),
                                       PinDir::Out, prim(PinKind::Any)),
                               dataPin(QStringLiteral("index"), QStringLiteral("index"),
                                       PinDir::Out, prim(PinKind::Int)),
                               execPin(QStringLiteral("done"), QStringLiteral("done"),
                                       PinDir::Out)});
    forEach.doc = help(
        QStringLiteral("Walks every element of an array."),
        {QStringLiteral("Emits `foreach (Type item : array)`."),
         QStringLiteral("Wiring the `index` pin switches to the counted form, "
                        "`foreach (int i, Type item : array)`."),
         QStringLiteral("The element type is taken from whatever feeds the array pin.")},
        {QStringLiteral("A null array will throw at runtime. Check it first if the "
                        "source can return null.")});
    add(forEach);

    NodeDef whileNode = makeDef(bi::While, QStringLiteral("While"),
                                QStringLiteral("loop while true"), CatFlow, accents::flow(),
                                {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                                 dataPin(QStringLiteral("cond"), QStringLiteral("condition"),
                                         PinDir::In, prim(PinKind::Bool)),
                                 execPin(QStringLiteral("body"), QStringLiteral("body"),
                                         PinDir::Out),
                                 execPin(QStringLiteral("done"), QStringLiteral("done"),
                                         PinDir::Out)});
    whileNode.doc = help(
        QStringLiteral("Repeats the body for as long as the condition holds."),
        {QStringLiteral("Emits `while (condition) { ... }`.")},
        {QStringLiteral("Nothing changes the condition for you, so make sure the body "
                        "can end the loop.")});
    add(whileNode);

    NodeDef ret = makeDef(bi::Return, QStringLiteral("Return"),
                          QStringLiteral("exit the function"), CatFlow, accents::flow(),
                          {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                           dataPin(QStringLiteral("value"), QStringLiteral("value"),
                                   PinDir::In, prim(PinKind::Any))});
    ret.doc = help(
        QStringLiteral("Leaves the current event or function immediately."),
        {QStringLiteral("Emits `return;`, or `return value;` when the value pin is "
                        "wired.")});
    add(ret);

    NodeDef serverOnly = makeDef(IdServerOnly, QStringLiteral("Server Only"),
                                 QStringLiteral("early-out on the client"),
                                 CatFlow, accents::flow(),
                                 {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                                  execPin(QStringLiteral("exec"), QString(), PinDir::Out)});
    serverOnly.doc = help(
        QStringLiteral("Stops the flow here when running on a client."),
        {QStringLiteral("Emits `if (!GetGame().IsServer()) return;`.")},
        {QStringLiteral("Events like Begin fire on client and server. Anything "
                        "touching health, inventory or spawning belongs behind this.")});
    add(serverOnly);

    NodeDef super = makeDef(IdSuper, QStringLiteral("Call Super"),
                            QStringLiteral("super.Event(...)"), CatFlow, accents::flow(),
                            {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                             execPin(QStringLiteral("exec"), QString(), PinDir::Out)});
    super.doc = help(
        QStringLiteral("Calls the base class implementation."),
        {QStringLiteral("Redundant in most graphs: `super` is emitted at the top of "
                        "every event already.")},
        {QStringLiteral("Use the event's \"skip super\" option instead of placing "
                        "this node.")});
    add(super);

    // ------------------------------------------------------------- variables
    // Get/Set nodes are built per variable by variableDef(); Self is the one
    // instance reference that is the same in every graph.
    NodeDef self = makeDef(IdSelf, QStringLiteral("Self"), QStringLiteral("this"),
                           CatVariables, accents::variable(),
                           {dataPin(QStringLiteral("ret"), QStringLiteral("self"),
                                    PinDir::Out, obj(QStringLiteral("auto")))},
                           true);
    self.doc = help(
        QStringLiteral("The instance the current script is running on: `this`."),
        {QStringLiteral("Wire it into any `target` pin to act on the object itself.")});
    add(self);

    // ------------------------------------------------------------- operators
    NodeDef op = makeDef(IdOp, QStringLiteral("Operator"), QStringLiteral("a op b"),
                         CatOperators, accents::pure(),
                         {dataPin(QStringLiteral("a"), QStringLiteral("a"), PinDir::In,
                                  prim(PinKind::Any)),
                          dataPin(QStringLiteral("b"), QStringLiteral("b"), PinDir::In,
                                  prim(PinKind::Any)),
                          dataPin(QStringLiteral("ret"), QString(), PinDir::Out,
                                  prim(PinKind::Any))},
                         true);
    op.doc = help(
        QStringLiteral("Combines two values with an arithmetic, comparison or logical "
                       "operator."),
        {QStringLiteral("Pick the operator in Details. Comparison and logical operators "
                        "output a bool."),
         QStringLiteral("Available operators: %1.")
             .arg(binaryOperators().join(QStringLiteral(" "))),
         QStringLiteral("`+` on strings concatenates, and a number joined to a string "
                        "converts itself. That is how text is built in Enforce.")});
    add(op);

    // One entry per operator, so arithmetic can be reached by searching for it.
    // The bare Operator node above needs its `op` set afterwards, and until
    // something in the UI could set it there was no way to write a subtraction
    // at all. These carry the operator with them, so "multiply" finds a
    // multiply and places one.
    for (const QString &symbol : binaryOperators()) {
        NodeDef preset = op;
        preset.key = IdOp + QLatin1Char('.') + symbol;
        preset.title = symbol;
        preset.subtitle = operatorWord(symbol);
        preset.doc = QStringLiteral("%1 (`a %2 b`). %3")
                         .arg(operatorWord(symbol), symbol, operatorNote(symbol));
        add(preset);
    }

    NodeDef notNode = makeDef(IdNot, QStringLiteral("Not"), QStringLiteral("!value"),
                              CatOperators, accents::pure(),
                              {dataPin(QStringLiteral("a"), QStringLiteral("value"),
                                       PinDir::In, prim(PinKind::Bool)),
                               dataPin(QStringLiteral("ret"), QString(), PinDir::Out,
                                       prim(PinKind::Bool))},
                              true);
    notNode.doc = help(QStringLiteral("Inverts a true/false value."),
                       {QStringLiteral("Emits `!(value)`.")});
    add(notNode);

    NodeDef select = makeDef(IdSelect, QStringLiteral("Select"),
                             QStringLiteral("cond ? a : b"), CatOperators, accents::pure(),
                             {dataPin(QStringLiteral("cond"), QStringLiteral("condition"),
                                      PinDir::In, prim(PinKind::Bool)),
                              dataPin(QStringLiteral("a"), QStringLiteral("true"),
                                      PinDir::In, prim(PinKind::Any)),
                              dataPin(QStringLiteral("b"), QStringLiteral("false"),
                                      PinDir::In, prim(PinKind::Any)),
                              dataPin(QStringLiteral("ret"), QString(), PinDir::Out,
                                      prim(PinKind::Any))},
                             true);
    select.doc = help(
        QStringLiteral("Picks one of two values based on a condition."),
        {QStringLiteral("Emits `(condition ? a : b)` inline, with no exec pins.")});
    add(select);

    // -------------------------------------------------------------- literals
    // The typed literals below are what the reference build writes; the generic
    // Literal node is the same thing with the type chosen in Details, so a
    // graph does not need six near-identical entries in the palette.
    NodeDef literal = makeDef(bi::Literal, QStringLiteral("Literal"),
                              QStringLiteral("string"), CatLiterals, accents::literal(),
                              {dataPin(QStringLiteral("v"), QString(), PinDir::In,
                                       prim(PinKind::String)),
                               dataPin(QStringLiteral("ret"), QString(), PinDir::Out,
                                       prim(PinKind::String))},
                              true);
    literal.doc = help(
        QStringLiteral("A fixed value of whatever type you choose."),
        {QStringLiteral("Set the type in Details; both pins follow it."),
         QStringLiteral("Type the value directly on the node. Strings are quoted and "
                        "vectors written as `\"x y z\"` when the script is generated.")});
    add(literal);

    const auto addLiteral = [this](const QString &key, const QString &title,
                                   const QString &subtitle, PinKind kind,
                                   const QString &doc) {
        NodeDef d = makeDef(key, title, subtitle, CatLiterals, accents::literal(),
                            {dataPin(QStringLiteral("v"), QString(), PinDir::In, prim(kind)),
                             dataPin(QStringLiteral("ret"), QString(), PinDir::Out, prim(kind))},
                            true);
        d.doc = doc;
        add(d);
    };

    addLiteral(IdLitBool, QStringLiteral("Bool"), QString(), PinKind::Bool,
               help(QStringLiteral("A fixed true/false value."),
                    {QStringLiteral("Type the value directly on the node.")}));
    addLiteral(IdLitInt, QStringLiteral("Int"), QString(), PinKind::Int,
               help(QStringLiteral("A fixed whole number."),
                    {QStringLiteral("Type the value directly on the node.")}));
    addLiteral(IdLitFloat, QStringLiteral("Float"), QString(), PinKind::Float,
               help(QStringLiteral("A fixed decimal number."),
                    {QStringLiteral("Type the value directly on the node.")}));
    addLiteral(IdLitString, QStringLiteral("String"), QString(), PinKind::String,
               help(QStringLiteral("A fixed piece of text."),
                    {QStringLiteral("Quotes are added for you when the script is "
                                    "generated.")}));
    addLiteral(IdLitVector, QStringLiteral("Vector"), QString(), PinKind::Vector,
               help(QStringLiteral("A fixed position or direction."),
                    {QStringLiteral("Written as `\"x y z\"`, because DayZ vectors are "
                                    "space-separated strings in script.")}));
    addLiteral(IdLitClass, QStringLiteral("Class Name"),
               QStringLiteral("typename literal"), PinKind::Typename,
               help(QStringLiteral("A class name as a value (a `typename`)."),
                    {QStringLiteral("What `AddAction` and similar calls expect: the "
                                    "type itself, not an instance.")}));

    // --------------------------------------------------------------- casting
    NodeDef cast = makeDef(bi::Cast, QStringLiteral("Cast To"),
                           QStringLiteral("Class.CastTo"), CatCasting, accents::cast(),
                           {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                            dataPin(QStringLiteral("obj"), QStringLiteral("object"),
                                    PinDir::In, obj(QStringLiteral("Class"))),
                            execPin(QStringLiteral("success"), QStringLiteral("success"),
                                    PinDir::Out),
                            execPin(QStringLiteral("failed"), QStringLiteral("failed"),
                                    PinDir::Out),
                            dataPin(QStringLiteral("as"), QStringLiteral("as"),
                                    PinDir::Out, obj(QStringLiteral("auto")))});
    cast.doc = help(
        QStringLiteral("Tries to treat an object as a more specific class."),
        {QStringLiteral("Emits `Class.CastTo(...)` inside an `if`, so the success pin "
                        "only runs when the cast worked."),
         QStringLiteral("The `as` pin carries the typed result into the success branch.")},
        {QStringLiteral("This is how you safely narrow an `EntityAI` to an `ItemBase`. "
                        "Never assume the type.")});
    add(cast);

    NodeDef newObj = makeDef(IdNew, QStringLiteral("New Object"),
                             QStringLiteral("new Class(), not entities"),
                             CatCasting, accents::cast(),
                             {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                              execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                              dataPin(QStringLiteral("ret"), QStringLiteral("object"),
                                      PinDir::Out, obj(QStringLiteral("auto")))});
    newObj.doc = help(
        QStringLiteral("Creates a plain script object with `new`: helpers, data "
                       "holders, Timers."),
        {QStringLiteral("Set the class in Details. The instance comes out of the "
                        "object pin.")},
        {QStringLiteral("Not for entities. Anything descending from `Object` (items, "
                        "players, vehicles, buildings) has an engine object behind it "
                        "that `new` cannot create. Use Spawn Entity."),
         QStringLiteral("A `new` object held only in a local dies when the call ends. "
                        "Store it in a class variable if it must survive, which is "
                        "what a Timer needs.")});
    add(newObj);

    NodeDef spawn = makeDef(IdSpawn, QStringLiteral("Spawn Entity"),
                            QStringLiteral("CreateObjectEx"), CatCasting, accents::cast(),
                            {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                             execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                             dataPin(QStringLiteral("type"), QStringLiteral("class name"),
                                     PinDir::In, prim(PinKind::String)),
                             dataPin(QStringLiteral("pos"), QStringLiteral("position"),
                                     PinDir::In, prim(PinKind::Vector)),
                             dataPin(QStringLiteral("ret"), QStringLiteral("entity"),
                                     PinDir::Out, obj(QStringLiteral("EntityAI")))});
    spawn.doc = help(
        QStringLiteral("Creates a real entity in the world."),
        {QStringLiteral("Emits `GetGame().CreateObjectEx(type, position, "
                        "ECE_PLACE_ON_SURFACE)` and casts the result to `EntityAI`."),
         QStringLiteral("Leaving position unwired spawns at this entity's own position.")},
        {QStringLiteral("Server-side only. Put a Server Only node ahead of it, or "
                        "clients will try to spawn their own copy."),
         QStringLiteral("The class name is the config class from `CfgVehicles`, not the "
                        "script class. The two are usually but not always the same.")});
    add(spawn);

    // --------------------------------------------------------------- utility
    NodeDef print = makeDef(bi::Print, QStringLiteral("Print"),
                            QStringLiteral("Print(value)"), CatUtility, accents::call(),
                            {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                             execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                             dataPin(QStringLiteral("value"), QStringLiteral("value"),
                                     PinDir::In, prim(PinKind::Any))});
    // The pin id is `value`, not `p0`: Print is a builtin with one named input,
    // not a catalogue call with positional parameters, and codegen, analysis and
    // the .sdzn fixture all read `value`. Renaming it silently drops the
    // argument of every Print in every saved project.
    //
    // `value` takes any type, which means no inline editor; the literal is set
    // by hand so an unwired Print still generates something that compiles.
    for (Pin &p : print.pins) {
        if (p.id != QLatin1String("value")) continue;
        p.def = QStringLiteral("\"\"");
        p.hasDef = true;
    }
    print.doc = help(
        QStringLiteral("Writes a line to the script log."),
        {QStringLiteral("Emits `Print(value);`. Anything can be wired in, and Enforce "
                        "converts it for you."),
         QStringLiteral("Output lands in the client or server script log, whichever "
                        "side ran the node.")},
        {QStringLiteral("Printing every frame floods the log and costs real "
                        "performance on a live server.")});
    add(print);

    NodeDef raw = makeDef(bi::Raw, QStringLiteral("Raw Enforce"),
                          QStringLiteral("inline code"), CatUtility, rawAccent(),
                          {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                           execPin(QStringLiteral("exec"), QString(), PinDir::Out)});
    raw.doc = help(
        QStringLiteral("Drops hand-written Enforce Script straight into the flow."),
        {QStringLiteral("The text is emitted verbatim at this point in the chain, "
                        "indented to match."),
         QStringLiteral("The code is read as Enforce, not treated as opaque text. "
                        "Unbalanced braces or parentheses, a string or block comment "
                        "left open, and names that nothing in this graph or the "
                        "catalogue declares are all reported on the node."),
         QStringLiteral("The node shows the code itself on the canvas, so a chain of "
                        "these reads as the script it generates.")},
        {QStringLiteral("Reading is not compiling. A name that exists but is the "
                        "wrong one, an argument of the wrong type, a missing "
                        "semicolon: all of those still reach the generated file.")});
    add(raw);

    NodeDef rawExpr = makeDef(IdRawExpr, QStringLiteral("Raw Expression"),
                              QStringLiteral("inline value"), CatUtility, rawAccent(),
                              {dataPin(QStringLiteral("ret"), QString(), PinDir::Out,
                                       prim(PinKind::Any))},
                              true);
    rawExpr.doc = help(
        QStringLiteral("Hand-written Enforce used as a value."),
        {QStringLiteral("Inlined verbatim wherever the output pin is wired."),
         QStringLiteral("Balance and unknown names are checked the same way as a raw "
                        "statement block, and the expression is shown on the node.")},
        {QStringLiteral("The value is still not type-checked against the pin it "
                        "feeds.")});
    add(rawExpr);

    NodeDef comment = makeDef(bi::Comment, QStringLiteral("Comment"),
                              QStringLiteral("sticky note"), CatUtility,
                              accents::comment(), {}, true);
    comment.doc = help(QStringLiteral("A note on the canvas."),
                       {QStringLiteral("Purely for humans. It generates no script, and "
                                       "nothing in it is read as code.")});
    add(comment);
}

void Builtins::add(const NodeDef &def)
{
    m_defs.insert(def.key, def);
    m_order.append(def.key);
}

void Builtins::addBeginMode(const QString &key, const LifecycleSig &sig)
{
    m_beginModes.insert(key, sig);
    m_beginOrder.append(key);
}

QVector<NodeDef> Builtins::all() const
{
    QVector<NodeDef> out;
    out.reserve(m_order.size());
    for (const QString &key : m_order) out.append(m_defs.value(key));
    return out;
}

NodeDef Builtins::def(const QString &id) const
{
    return m_defs.value(id);
}

QStringList Builtins::categories() const
{
    return {CatLifecycle, CatFlow, CatVariables, CatOperators,
            CatLiterals, CatCasting, CatUtility};
}

LifecycleSig Builtins::beginMode(const QString &key) const
{
    // End is not a Begin choice, but its signature is the same kind of record
    // and the generator needs one definition of it rather than two.
    if (key == QLatin1String("end")) return endSignature();
    const auto hit = m_beginModes.constFind(key);
    if (hit != m_beginModes.constEnd()) return hit.value();
    return m_beginModes.value(QStringLiteral("init"));
}

NodeDef Builtins::defForNode(const GraphNode &node, const Catalog &cat) const
{
    const auto isEnumFn = [&cat](const QString &n) { return cat.isEnum(n); };

    // Variable nodes are shaped by the variable, which lives in the graph.
    // Document passes the real record to variableDef(); this path only fires
    // when a node is resolved on its own, so it falls back to its options.
    if (node.kind == NodeKind::VarGet || node.kind == NodeKind::VarSet) {
        GraphVariable v;
        const int dot = node.ref.lastIndexOf(QLatin1Char('.'));
        v.id = dot >= 0 ? node.ref.mid(dot + 1) : node.ref;
        v.name = node.opts.value(QStringLiteral("name"), v.id);
        v.type = node.opts.value(QStringLiteral("type"), QStringLiteral("auto"));
        return variableDef(v, node.kind == NodeKind::VarSet, cat);
    }

    const NodeDef base = def(node.ref);
    if (!base.valid) return cat.defFor(node.ref);

    // Begin always shows which Enforce method it will become, so the graph does
    // not hide the one decision that matters about it.
    if (base.key == bi::Begin) {
        const LifecycleSig sig =
            beginMode(node.opts.value(QStringLiteral("when"), QStringLiteral("init")));
        NodeDef d = base;
        d.subtitle = sig.ctor ? QStringLiteral("constructor")
                              : QStringLiteral("%1()").arg(sig.method);
        // Event parameters are addressed as o0, o1 ... by the generator. No mode
        // takes any today, so this only matters if one is added later.
        d.pins = {execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                  dataPin(QStringLiteral("self"), QStringLiteral("self"), PinDir::Out,
                          obj(QStringLiteral("auto")))};
        for (int i = 0; i < sig.params.size(); ++i)
            d.pins.append(dataPin(QStringLiteral("o%1").arg(i), sig.params.at(i).name,
                                  PinDir::Out, pinTypeOf(sig.params.at(i).type, isEnumFn)));
        return d;
    }

    if (base.key == bi::End) {
        NodeDef d = base;
        d.subtitle = QStringLiteral("%1()").arg(endSignature().method);
        return d;
    }

    if (node.opts.isEmpty()) return base;

    if (base.key.startsWith(IdOp)) {
        // A palette entry carries its operator in the key (bi.op.*), a node
        // placed from the bare Operator entry carries it in opts. Either way
        // the operator is what reshapes the node, so resolve both here.
        QString opText = node.opts.value(QStringLiteral("op"));
        if (opText.isEmpty() && base.key.size() > IdOp.size() + 1)
            opText = base.key.mid(IdOp.size() + 1);
        if (opText.isEmpty()) return base;
        const bool logical = isLogicalOp(opText);
        const PinType operand = logical ? prim(PinKind::Bool) : prim(PinKind::Any);
        const PinType result = operatorYieldsBool(opText) ? prim(PinKind::Bool)
                                                          : prim(PinKind::Any);
        NodeDef d = base;
        d.title = opText;
        d.subtitle = QStringLiteral("operator");
        d.pins = {dataPin(QStringLiteral("a"), QStringLiteral("a"), PinDir::In, operand),
                  dataPin(QStringLiteral("b"), QStringLiteral("b"), PinDir::In, operand),
                  dataPin(QStringLiteral("ret"), QString(), PinDir::Out, result)};
        return d;
    }

    if (base.key == bi::Literal) {
        const QString type =
            node.opts.value(QStringLiteral("type"), QStringLiteral("string"));
        const PinType t = pinTypeOf(type, isEnumFn);
        NodeDef d = base;
        d.subtitle = type;
        d.pins = {dataPin(QStringLiteral("v"), QString(), PinDir::In, t),
                  dataPin(QStringLiteral("ret"), QString(), PinDir::Out, t)};
        return d;
    }

    if (base.key == bi::Cast || base.key == IdNew) {
        // Both spellings of the target class resolve through bi::castClass, so
        // the canvas cannot end up showing a different type than is generated.
        const QString cls = bi::castClass(node);
        if (cls.isEmpty()) return base;
        const PinType t = pinTypeOf(cls, isEnumFn);
        const bool isCast = base.key == bi::Cast;
        NodeDef d = base;
        d.title = isCast ? QStringLiteral("Cast To %1").arg(cls)
                         : QStringLiteral("New %1").arg(cls);
        for (Pin &p : d.pins) {
            const bool shaped = isCast ? p.id == QLatin1String("as")
                                       : p.id == QLatin1String("ret");
            if (shaped && p.dir == PinDir::Out) p.type = t;
        }
        return d;
    }

    return base;
}

NodeDef Builtins::variableDef(const GraphVariable &var, bool setter,
                              const Catalog &cat) const
{
    const auto isEnumFn = [&cat](const QString &n) { return cat.isEnum(n); };
    const QString type = var.type.isEmpty() ? QStringLiteral("auto") : var.type;
    const PinType t = pinTypeOf(type, isEnumFn);
    const QString name = var.name.isEmpty() ? QStringLiteral("variable") : var.name;

    if (!setter) {
        NodeDef d = makeDef(QStringLiteral("var.get.%1").arg(var.id),
                            QStringLiteral("Get %1").arg(name), type,
                            CatVariables, accents::variable(),
                            {dataPin(QStringLiteral("ret"), QString(), PinDir::Out, t)},
                            true);
        d.doc = help(QStringLiteral("Reads the member variable %1.").arg(name),
                     {QStringLiteral("No exec pins. It reads inline wherever its "
                                     "output is used.")});
        return d;
    }

    // The value also comes out of the setter, so a chain can keep using it
    // without a second Get node.
    NodeDef d = makeDef(QStringLiteral("var.set.%1").arg(var.id),
                        QStringLiteral("Set %1").arg(name), type,
                        CatVariables, accents::variable(),
                        {execPin(QStringLiteral("exec"), QString(), PinDir::In),
                         execPin(QStringLiteral("exec"), QString(), PinDir::Out),
                         dataPin(QStringLiteral("v"), QStringLiteral("value"),
                                 PinDir::In, t),
                         dataPin(QStringLiteral("ret"), QString(), PinDir::Out, t)});
    QStringList cautions;
    if (var.sync)
        cautions << QStringLiteral("%1 is net-synced: call SetSynchDirty() on the "
                                   "server after writing it, or clients never see "
                                   "the new value.").arg(name);
    if (var.isConst)
        cautions << QStringLiteral("%1 is const, so assigning to it will not compile.")
                        .arg(name);
    d.doc = help(QStringLiteral("Assigns to the member variable %1.").arg(name),
                 {QStringLiteral("Emits `%1 = value;`.").arg(name)}, cautions);
    return d;
}
