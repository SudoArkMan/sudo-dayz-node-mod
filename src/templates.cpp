#include "templates.h"

#include "catalog.h"

#include <QRegularExpression>

const QString TEMPLATE_PREFIX = QStringLiteral("tpl.");

namespace {

TemplatePin in(const QString &id, const QString &label, const QString &type)
{
    return {id, label, type, PinDir::In};
}

TemplatePin out(const QString &id, const QString &label, const QString &type)
{
    return {id, label, type, PinDir::Out};
}

// The DayZ Essentials set, mirrored from nodes/builtin-sets.ts. These exist
// because of what importing a real mod showed: 84% of it fell back to raw
// Enforce. The same few reasons repeat: string building, member access, array
// iteration, file IO. Each is one template away from being a proper node.
QVector<NodeTemplate> makeBuiltins()
{
    QVector<NodeTemplate> t;
    const auto add = [&t](NodeTemplate n) {
        n.valid = true;
        t.append(n);
    };

    // ----------------------------------------------------------- strings
    add({QStringLiteral("str.concat2"), QStringLiteral("Join Text"),
         QStringLiteral("Text"), QStringLiteral("a + b"),
         QStringLiteral("Joins two pieces of text. Enforce concatenates with `+`, and a number "
                        "joined to a string is converted automatically."),
         {},
         true,
         {in(QStringLiteral("a"), QStringLiteral("a"), QStringLiteral("string")),
          in(QStringLiteral("b"), QStringLiteral("b"), QStringLiteral("string")),
          out(QStringLiteral("ret"), QString(), QStringLiteral("string"))},
         QStringLiteral("{a} + {b}")});

    add({QStringLiteral("str.append"), QStringLiteral("Append Text"),
         QStringLiteral("Text"), QStringLiteral("target += value"),
         QStringLiteral("Appends to an existing string variable. The usual way a payload or "
                        "message is built up across several statements."),
         {},
         false,
         {in(QStringLiteral("target"), QStringLiteral("variable"), QStringLiteral("string")),
          in(QStringLiteral("value"), QStringLiteral("append"), QStringLiteral("string"))},
         QStringLiteral("{target} += {value};")});

    add({QStringLiteral("str.format"), QStringLiteral("Format Text"),
         QStringLiteral("Text"), QStringLiteral("string.Format"),
         QStringLiteral("Builds a string from a pattern. Placeholders are %1 to %9, "
                        "one-indexed. There is no %0, and a tenth argument will not compile."),
         {},
         true,
         {in(QStringLiteral("fmt"), QStringLiteral("pattern"), QStringLiteral("string")),
          in(QStringLiteral("a"), QStringLiteral("%1"), QStringLiteral("string")),
          in(QStringLiteral("b"), QStringLiteral("%2"), QStringLiteral("string")),
          out(QStringLiteral("ret"), QString(), QStringLiteral("string"))},
         QStringLiteral("string.Format({fmt}, {a}, {b})")});

    // ------------------------------------------------------------ access
    add({QStringLiteral("obj.member"), QStringLiteral("Read Field"),
         QStringLiteral("Objects"), QStringLiteral("object.field"),
         QStringLiteral("Reads a public field off an object. Type the field name; the pin type "
                        "is whatever you declare it as."),
         {},
         true,
         {in(QStringLiteral("obj"), QStringLiteral("object"), QStringLiteral("Class")),
          in(QStringLiteral("field"), QStringLiteral("field"), QStringLiteral("string")),
          out(QStringLiteral("ret"), QString(), QStringLiteral("string"))},
         QStringLiteral("{obj}.{field}")});

    add({QStringLiteral("obj.setMember"), QStringLiteral("Write Field"),
         QStringLiteral("Objects"), QStringLiteral("object.field = value"),
         QStringLiteral("Assigns to a public field on another object."),
         {},
         false,
         {in(QStringLiteral("obj"), QStringLiteral("object"), QStringLiteral("Class")),
          in(QStringLiteral("field"), QStringLiteral("field"), QStringLiteral("string")),
          in(QStringLiteral("value"), QStringLiteral("value"), QStringLiteral("string"))},
         QStringLiteral("{obj}.{field} = {value};")});

    // ------------------------------------------------------------ arrays
    add({QStringLiteral("arr.new"), QStringLiteral("New Array"),
         QStringLiteral("Arrays"), QStringLiteral("new array<ref T>"),
         QStringLiteral("Creates an owned array. Element type is written as you would in "
                        "script, e.g. `ref SUDO_Event`."),
         {},
         false,
         {in(QStringLiteral("elem"), QStringLiteral("element type"), QStringLiteral("string")),
          out(QStringLiteral("ret"), QStringLiteral("array"), QStringLiteral("string"))},
         QStringLiteral("new array<{elem}>()")});

    add({QStringLiteral("arr.count"), QStringLiteral("Array Count"),
         QStringLiteral("Arrays"), QStringLiteral("array.Count()"),
         QStringLiteral("Number of elements. Note this is Count(), not Length(). Length() is a "
                        "string method, and using it on an array will not compile."),
         {},
         true,
         {in(QStringLiteral("arr"), QStringLiteral("array"), QStringLiteral("Class")),
          out(QStringLiteral("ret"), QStringLiteral("count"), QStringLiteral("int"))},
         QStringLiteral("{arr}.Count()")});

    add({QStringLiteral("arr.get"), QStringLiteral("Array Get"),
         QStringLiteral("Arrays"), QStringLiteral("array.Get(i)"),
         QStringLiteral("Element at an index. Out-of-range access throws at runtime, so pair it "
                        "with Array Count."),
         {},
         true,
         {in(QStringLiteral("arr"), QStringLiteral("array"), QStringLiteral("Class")),
          in(QStringLiteral("i"), QStringLiteral("index"), QStringLiteral("int")),
          out(QStringLiteral("ret"), QStringLiteral("item"), QStringLiteral("Class"))},
         QStringLiteral("{arr}.Get({i})")});

    add({QStringLiteral("arr.insert"), QStringLiteral("Array Insert"),
         QStringLiteral("Arrays"), QStringLiteral("array.Insert(item)"),
         QStringLiteral("Appends an item to the end of an array."),
         {},
         false,
         {in(QStringLiteral("arr"), QStringLiteral("array"), QStringLiteral("Class")),
          in(QStringLiteral("item"), QStringLiteral("item"), QStringLiteral("Class"))},
         QStringLiteral("{arr}.Insert({item});")});

    add({QStringLiteral("arr.clear"), QStringLiteral("Array Clear"),
         QStringLiteral("Arrays"), QStringLiteral("array.Clear()"),
         QStringLiteral("Removes every element."),
         {},
         false,
         {in(QStringLiteral("arr"), QStringLiteral("array"), QStringLiteral("Class"))},
         QStringLiteral("{arr}.Clear();")});

    // -------------------------------------------------------------- json
    add({QStringLiteral("json.load"), QStringLiteral("Load JSON File"),
         QStringLiteral("Files"), QStringLiteral("JsonFileLoader<T>.JsonLoadFile"),
         QStringLiteral("Reads a JSON file into an existing object."),
         {QStringLiteral("JsonLoadFile returns void and fills the object you pass it, so "
                         "assigning its result does not compile."),
          QStringLiteral("The type must derive from Managed, or the call fails with "
                         "\"Bad type 'JsonFileLoader'\".")},
         false,
         {in(QStringLiteral("type"), QStringLiteral("type"), QStringLiteral("string")),
          in(QStringLiteral("path"), QStringLiteral("path"), QStringLiteral("string")),
          in(QStringLiteral("obj"), QStringLiteral("into"), QStringLiteral("Class"))},
         QStringLiteral("JsonFileLoader<{type}>.JsonLoadFile({path}, {obj});")});

    add({QStringLiteral("json.save"), QStringLiteral("Save JSON File"),
         QStringLiteral("Files"), QStringLiteral("JsonFileLoader<T>.JsonSaveFile"),
         QStringLiteral("Writes an object to a JSON file."),
         {QStringLiteral("Paths use forward slashes. A backslash in an Enforce string breaks "
                         "the parser.")},
         false,
         {in(QStringLiteral("type"), QStringLiteral("type"), QStringLiteral("string")),
          in(QStringLiteral("path"), QStringLiteral("path"), QStringLiteral("string")),
          in(QStringLiteral("obj"), QStringLiteral("from"), QStringLiteral("Class"))},
         QStringLiteral("JsonFileLoader<{type}>.JsonSaveFile({path}, {obj});")});

    add({QStringLiteral("file.exists"), QStringLiteral("File Exists"),
         QStringLiteral("Files"), QStringLiteral("FileExist"),
         QStringLiteral("True when a file is present. `$profile:` resolves to the server "
                        "profile folder."),
         {},
         true,
         {in(QStringLiteral("path"), QStringLiteral("path"), QStringLiteral("string")),
          out(QStringLiteral("ret"), QString(), QStringLiteral("bool"))},
         QStringLiteral("FileExist({path})")});

    // ------------------------------------------------------------ timing
    add({QStringLiteral("timer.callLater"), QStringLiteral("Call Later"),
         QStringLiteral("Timing"), QStringLiteral("CallQueue.CallLater"),
         QStringLiteral("Schedules a method to run after a delay, optionally repeating."),
         {QStringLiteral("CallLater takes a function reference, `this.MethodName`, with no "
                         "quotes. The quoted form is CallLaterByName, and passing a name here "
                         "fails to convert."),
          QStringLiteral("Remove it in your shutdown path or it keeps firing against a dead "
                         "object.")},
         false,
         {in(QStringLiteral("fn"), QStringLiteral("this.Method"), QStringLiteral("string")),
          in(QStringLiteral("ms"), QStringLiteral("delay ms"), QStringLiteral("int")),
          in(QStringLiteral("repeat"), QStringLiteral("repeat"), QStringLiteral("bool"))},
         QStringLiteral("GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater({fn}, {ms}, "
                        "{repeat});")});

    add({QStringLiteral("timer.remove"), QStringLiteral("Cancel Call Later"),
         QStringLiteral("Timing"), QStringLiteral("CallQueue.Remove"),
         QStringLiteral("Cancels a scheduled call. Pass the same function reference you "
                        "scheduled."),
         {},
         false,
         {in(QStringLiteral("fn"), QStringLiteral("this.Method"), QStringLiteral("string"))},
         QStringLiteral("GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).Remove({fn});")});

    // ---------------------------------------------------------- advanced
    add({QStringLiteral("misc.methodRef"), QStringLiteral("Method Reference"),
         QStringLiteral("Advanced"), QStringLiteral("this.Method"),
         QStringLiteral("A reference to one of this class's methods, for APIs that take a "
                        "function rather than a name."),
         {},
         true,
         {in(QStringLiteral("name"), QStringLiteral("method"), QStringLiteral("string")),
          out(QStringLiteral("ret"), QString(), QStringLiteral("string"))},
         QStringLiteral("this.{name}")});

    return t;
}

Pin toPin(const TemplatePin &p, const std::function<bool(const QString &)> &isEnum)
{
    Pin pin;
    pin.id = p.id;
    pin.label = p.label;
    pin.dir = p.dir;
    pin.type = p.type == QLatin1String("exec") ? PinType{PinKind::Exec, QString(), false}
                                               : pinTypeOf(p.type, isEnum);
    if (p.dir == PinDir::In && pin.type.kind != PinKind::Exec
        && inlineEditorFor(pin.type) != InlineEditor::None) {
        pin.def = defaultLiteral(pin.type);
        pin.hasDef = true;
    }
    return pin;
}

} // namespace

bool isTemplateKey(const QString &key)
{
    return key.startsWith(TEMPLATE_PREFIX);
}

const QVector<NodeTemplate> &builtinTemplates()
{
    static const QVector<NodeTemplate> table = makeBuiltins();
    return table;
}

NodeTemplate findTemplate(const QString &key)
{
    if (!isTemplateKey(key)) return {};
    const QString id = key.mid(TEMPLATE_PREFIX.size());
    for (const NodeTemplate &t : builtinTemplates())
        if (t.id == id) return t;
    return {};
}

NodeDef templateDef(const NodeTemplate &t,
                    const std::function<bool(const QString &)> &isEnum)
{
    NodeDef def;
    if (!t.valid) return def;
    def.key = TEMPLATE_PREFIX + t.id;
    def.title = t.title;
    def.subtitle = t.subtitle.isEmpty() ? t.category : t.subtitle;
    def.category = t.category;
    def.accent = t.pure ? accents::pure() : accents::call();
    def.doc = t.summary;
    def.pure = t.pure;
    if (!t.pure) {
        Pin execIn;
        execIn.id = QStringLiteral("exec");
        execIn.dir = PinDir::In;
        execIn.type = PinType{PinKind::Exec, QString(), false};
        Pin execOut = execIn;
        execOut.dir = PinDir::Out;
        def.pins.append(execIn);
        def.pins.append(execOut);
    }
    for (const TemplatePin &p : t.pins) def.pins.append(toPin(p, isEnum));
    def.valid = true;
    return def;
}

QString renderTemplate(const NodeTemplate &t,
                       const std::function<QString(const QString &)> &resolve)
{
    static const QRegularExpression placeholder(QStringLiteral("\\{(\\w+)\\}"));
    QString out;
    int last = 0;
    // Substitutions run strictly left to right: resolving a pin can allocate a
    // temporary and push a warning, so the order is observable.
    auto it = placeholder.globalMatch(t.code);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString pinId = m.captured(1);
        bool known = false;
        for (const TemplatePin &p : t.pins)
            if (p.id == pinId && p.dir == PinDir::In) known = true;
        out += t.code.mid(last, m.capturedStart() - last);
        out += known ? resolve(pinId) : m.captured(0);
        last = m.capturedEnd();
    }
    out += t.code.mid(last);
    return out;
}
