#include "catalog.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace accents {
QColor event()    { return QColor("#7a2318"); }
QColor call()     { return QColor("#1d3a52"); }
QColor pure()     { return QColor("#234a2e"); }
QColor flow()     { return QColor("#2f353f"); }
QColor variable() { return QColor("#4a2c5e"); }
QColor literal()  { return QColor("#3c4022"); }
QColor cast()     { return QColor("#5e4a1d"); }
QColor comment()  { return QColor("#242c36"); }
} // namespace accents

namespace {

int intAt(const QJsonArray &a, int i, int fallback = 0)
{
    return i < a.size() ? a.at(i).toInt(fallback) : fallback;
}

// A catalogue key is a prefix plus a plain decimal index into one of the packed
// tables. Anything else has to fail: QString::toInt() reports 0 for an empty
// remainder, for text, and for an overflowing number, and it silently accepts
// leading whitespace and a sign, so an unvalidated conversion turns a ref an
// older build wrote ("m_LegacyName", "gSomething") into entry 0 of the table:
// a real node with the wrong signature, and no diagnostic anywhere downstream.
// Returns -1 when the key does not name an entry of `count`.
int indexIn(const QString &key, int prefixLen, int count)
{
    const QStringView digits = QStringView(key).mid(prefixLen);
    if (digits.isEmpty()) return -1;
    for (const QChar c : digits)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return -1;
    bool ok = false;
    const int n = digits.toInt(&ok);
    return ok && n >= 0 && n < count ? n : -1;
}

} // namespace

bool Catalog::load(const QString &jsonPath)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open %1").arg(jsonPath);
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        m_error = QStringLiteral("%1: %2").arg(jsonPath, err.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    m_source = root.value("source").toString();

    const QJsonArray strings = root.value("strings").toArray();
    m_strings.reserve(strings.size());
    for (const QJsonValue &v : strings) m_strings.append(v.toString());

    const QJsonArray files = root.value("files").toArray();
    m_files.reserve(files.size());
    for (const QJsonValue &v : files) m_files.append(v.toString());

    const auto readParams = [](const QJsonArray &arr) {
        QVector<Param> out;
        out.reserve(arr.size());
        for (const QJsonValue &pv : arr) {
            const QJsonArray p = pv.toArray();
            Param param;
            param.type = intAt(p, 0);
            param.name = intAt(p, 1);
            param.dir = intAt(p, 2);
            param.def = intAt(p, 3);
            out.append(param);
        }
        return out;
    };

    // [name, extends, file, line, module, guards, doc]
    for (const QJsonValue &v : root.value("classes").toArray()) {
        const QJsonArray a = v.toArray();
        Class c;
        c.name = intAt(a, 0);
        c.extendsId = intAt(a, 1, -1);
        c.file = intAt(a, 2);
        c.line = intAt(a, 3);
        c.module = intAt(a, 4);
        c.guards = intAt(a, 5);
        c.doc = intAt(a, 6);
        m_classes.append(c);
    }

    // [owner, name, ret, params, flags, line, guards, doc]
    for (const QJsonValue &v : root.value("methods").toArray()) {
        const QJsonArray a = v.toArray();
        Method m;
        m.owner = intAt(a, 0);
        m.name = intAt(a, 1);
        m.ret = intAt(a, 2);
        m.params = readParams(a.at(3).toArray());
        m.flags = intAt(a, 4);
        m.line = intAt(a, 5);
        m.guards = intAt(a, 6);
        m.doc = intAt(a, 7);
        m_methods.append(m);
    }

    // [name, [values], file, line]
    for (const QJsonValue &v : root.value("enums").toArray()) {
        const QJsonArray a = v.toArray();
        Enum e;
        e.name = intAt(a, 0);
        for (const QJsonValue &val : a.at(1).toArray()) e.values.append(val.toInt());
        e.file = intAt(a, 2);
        e.line = intAt(a, 3);
        m_enums.append(e);
    }

    // [name, ret, params, flags, file, line, guards, doc]
    for (const QJsonValue &v : root.value("globals").toArray()) {
        const QJsonArray a = v.toArray();
        Global g;
        g.name = intAt(a, 0);
        g.ret = intAt(a, 1);
        g.params = readParams(a.at(2).toArray());
        g.flags = intAt(a, 3);
        g.file = intAt(a, 4);
        g.line = intAt(a, 5);
        g.guards = intAt(a, 6);
        g.doc = intAt(a, 7);
        m_globals.append(g);
    }

    // [name, type, value, file, line]
    for (const QJsonValue &v : root.value("consts").toArray()) {
        const QJsonArray a = v.toArray();
        Const c;
        c.name = intAt(a, 0);
        c.type = intAt(a, 1);
        c.value = intAt(a, 2);
        c.file = intAt(a, 3);
        c.line = intAt(a, 4);
        m_consts.append(c);
    }

    const QJsonObject totals = root.value("totals").toObject();
    for (auto it = totals.begin(); it != totals.end(); ++it)
        m_totals.insert(it.key(), it.value().toInt());

    for (const Enum &e : m_enums) m_enumNames.insert(s(e.name));
    for (int i = 0; i < m_classes.size(); ++i)
        m_classByName.insert(s(m_classes.at(i).name), i);
    for (int i = 0; i < m_methods.size(); ++i)
        m_methodsByClass[m_methods.at(i).owner].append(i);

    buildSearchIndex();
    m_loaded = true;
    return true;
}

ClassInfo Catalog::classInfo(int id) const
{
    ClassInfo info;
    if (id < 0 || id >= m_classes.size()) return info;
    const Class &c = m_classes.at(id);
    info.id = id;
    info.name = s(c.name);
    info.extendsId = c.extendsId;
    info.file = fileAt(c.file);
    info.line = c.line;
    info.module = s(c.module);
    info.guards = s(c.guards);
    info.doc = s(c.doc);
    info.valid = true;
    return info;
}

int Catalog::classId(const QString &name) const
{
    return m_classByName.value(name, -1);
}

QStringList Catalog::classNames() const
{
    QStringList out = m_classByName.keys();
    out.sort(Qt::CaseInsensitive);
    return out;
}

QVector<ClassInfo> Catalog::ancestors(const QString &name) const
{
    QVector<ClassInfo> out;
    QSet<int> seen;
    int id = m_classByName.value(name, -1);
    while (id >= 0 && !seen.contains(id)) {
        seen.insert(id);
        const ClassInfo info = classInfo(id);
        if (!info.valid) break;
        out.append(info);
        id = info.extendsId;
    }
    return out;
}

bool Catalog::isA(const QString &child, const QString &base) const
{
    if (child == base) return true;
    for (const ClassInfo &c : ancestors(child))
        if (c.name == base) return true;
    return false;
}

Pin Catalog::makePin(const QString &id, const QString &label, PinDir dir,
                     const PinType &type) const
{
    Pin p;
    p.id = id;
    p.label = label;
    p.dir = dir;
    p.type = type;
    if (dir == PinDir::In && type.kind != PinKind::Exec
        && inlineEditorFor(type) != InlineEditor::None) {
        p.def = defaultLiteral(type);
        p.hasDef = true;
    }
    return p;
}

QVector<Pin> Catalog::paramPins(const QVector<Param> &params) const
{
    QVector<Pin> pins;
    const auto isEnumFn = [this](const QString &n) { return isEnum(n); };
    for (int i = 0; i < params.size(); ++i) {
        const Param &p = params.at(i);
        const PinType type = pinTypeOf(s(p.type), isEnumFn);
        const QString declaredDefault = s(p.def);
        // Optional parameters are labelled with their default so the pin does
        // not read as required.
        QString base = s(p.name);
        if (base.isEmpty()) base = QStringLiteral("arg%1").arg(i);
        const QString label = declaredDefault.isEmpty()
                                  ? base
                                  : QStringLiteral("%1 = %2").arg(base, declaredDefault);
        if (p.dir == 0 || p.dir == 2) {
            Pin in = makePin(QStringLiteral("p%1").arg(i), label, PinDir::In, type);
            if (!declaredDefault.isEmpty()) { // let codegen drop optionals
                in.def.clear();
                in.hasDef = false;
            }
            pins.append(in);
        }
        if (p.dir == 1 || p.dir == 2)
            pins.append(makePin(QStringLiteral("o%1").arg(i), base, PinDir::Out, type));
    }
    return pins;
}

NodeDef Catalog::defFor(const QString &key) const
{
    if (key.isEmpty()) return {};
    const auto hit = m_defCache.constFind(key);
    if (hit != m_defCache.constEnd()) return hit.value();
    NodeDef built = build(key);
    if (built.valid) m_defCache.insert(key, built);
    return built;
}

NodeDef Catalog::build(const QString &key) const
{
    const auto isEnumFn = [this](const QString &n) { return isEnum(n); };

    if (key.startsWith('m')) {
        const int n = indexIn(key, 1, m_methods.size());
        if (n < 0) return {};
        const Method &m = m_methods.at(n);
        const ClassInfo owner = classInfo(m.owner);
        const QString name = s(m.name);
        const QString ret = s(m.ret);
        const bool isStatic = m.flags & flag::Static;
        const bool isCtor = m.flags & flag::Ctor;
        const bool isEvent = m.flags & flag::Event;
        const bool isPure = m.flags & flag::Pure;

        NodeDef def;
        def.key = key;
        if (isEvent) {
            // Event nodes start a flow and expose parameters as outputs.
            def.pins.append(makePin("exec", "", PinDir::Out, {PinKind::Exec}));
            if (owner.valid)
                def.pins.append(makePin("self", "self", PinDir::Out,
                                        {PinKind::Object, owner.name, false}));
            for (int i = 0; i < m.params.size(); ++i) {
                const Param &p = m.params.at(i);
                QString label = s(p.name);
                if (label.isEmpty()) label = QStringLiteral("arg%1").arg(i);
                def.pins.append(makePin(QStringLiteral("o%1").arg(i), label,
                                        PinDir::Out, pinTypeOf(s(p.type), isEnumFn)));
            }
        } else {
            if (!isPure) {
                def.pins.append(makePin("exec", "", PinDir::In, {PinKind::Exec}));
                def.pins.append(makePin("exec", "", PinDir::Out, {PinKind::Exec}));
            }
            if (!isStatic && !isCtor && owner.valid)
                def.pins.append(makePin("target", "target", PinDir::In,
                                        {PinKind::Object, owner.name, false}));
            def.pins.append(paramPins(m.params));
            if (ret != QLatin1String("void"))
                def.pins.append(makePin("ret", isCtor ? "object" : "return",
                                        PinDir::Out, pinTypeOf(ret, isEnumFn)));
        }

        def.title = isCtor ? QStringLiteral("Construct %1").arg(name)
                           : isEvent ? QStringLiteral("Event %1").arg(name)
                                     : name;
        def.subtitle = owner.valid ? owner.name : QString();
        def.category = isEvent ? QStringLiteral("Events")
                               : isPure ? QStringLiteral("Pure")
                                        : QStringLiteral("Functions");
        def.accent = isEvent ? accents::event() : isPure ? accents::pure() : accents::call();
        def.pure = isPure;
        def.native = m.flags & flag::Native;
        def.loc = owner.valid ? QStringLiteral("%1:%2").arg(owner.file).arg(m.line)
                              : QString();
        def.valid = true;
        return def;
    }

    if (key.startsWith('g')) {
        const int n = indexIn(key, 1, m_globals.size());
        if (n < 0) return {};
        const Global &g = m_globals.at(n);
        const bool isPure = g.flags & flag::Pure;
        NodeDef def;
        def.key = key;
        if (!isPure) {
            def.pins.append(makePin("exec", "", PinDir::In, {PinKind::Exec}));
            def.pins.append(makePin("exec", "", PinDir::Out, {PinKind::Exec}));
        }
        def.pins.append(paramPins(g.params));
        const QString ret = s(g.ret);
        if (ret != QLatin1String("void"))
            def.pins.append(makePin("ret", "return", PinDir::Out,
                                    pinTypeOf(ret, isEnumFn)));
        def.title = s(g.name);
        def.subtitle = QStringLiteral("global");
        def.category = QStringLiteral("Globals");
        def.accent = isPure ? accents::pure() : accents::call();
        def.pure = isPure;
        def.native = g.flags & flag::Native;
        def.loc = QStringLiteral("%1:%2").arg(fileAt(g.file)).arg(g.line);
        def.valid = true;
        return def;
    }

    if (key.startsWith(QLatin1String("en"))) {
        const int n = indexIn(key, 2, m_enums.size());
        if (n < 0) return {};
        const Enum &e = m_enums.at(n);
        const QString name = s(e.name);
        NodeDef def;
        def.key = key;
        def.title = name;
        def.subtitle = QStringLiteral("enum - %1 values").arg(e.values.size());
        def.category = QStringLiteral("Enums");
        def.accent = accents::literal();
        def.pins.append(makePin("ret", "value", PinDir::Out,
                                {PinKind::Enum, name, false}));
        def.pure = true;
        def.loc = QStringLiteral("%1:%2").arg(fileAt(e.file)).arg(e.line);
        def.valid = true;
        return def;
    }

    if (key.startsWith(QLatin1String("co"))) {
        const int n = indexIn(key, 2, m_consts.size());
        if (n < 0) return {};
        const Const &c = m_consts.at(n);
        NodeDef def;
        def.key = key;
        def.title = s(c.name);
        def.subtitle = QStringLiteral("const %1").arg(s(c.type));
        def.category = QStringLiteral("Constants");
        def.accent = accents::literal();
        def.pins.append(makePin("ret", "value", PinDir::Out,
                                pinTypeOf(s(c.type), isEnumFn)));
        def.pure = true;
        def.loc = QStringLiteral("%1:%2").arg(fileAt(c.file)).arg(c.line);
        def.valid = true;
        return def;
    }

    return {};
}

MethodSig Catalog::method(const QString &key) const
{
    MethodSig sig;
    if (!key.startsWith('m')) return sig;
    const int n = indexIn(key, 1, m_methods.size());
    if (n < 0) return sig;
    const Method &m = m_methods.at(n);
    sig.owner = classInfo(m.owner).name;
    sig.name = s(m.name);
    sig.ret = s(m.ret);
    sig.flags = m.flags;
    for (const Param &p : m.params)
        sig.params.append({s(p.type), s(p.name), p.dir, s(p.def)});
    sig.valid = true;
    return sig;
}

MethodSig Catalog::globalFn(const QString &key) const
{
    MethodSig sig;
    if (!key.startsWith('g')) return sig;
    const int n = indexIn(key, 1, m_globals.size());
    if (n < 0) return sig;
    const Global &g = m_globals.at(n);
    sig.name = s(g.name);
    sig.ret = s(g.ret);
    sig.flags = g.flags;
    for (const Param &p : g.params)
        sig.params.append({s(p.type), s(p.name), p.dir, s(p.def)});
    sig.valid = true;
    return sig;
}

QString Catalog::constName(const QString &key) const
{
    if (!key.startsWith(QLatin1String("co"))) return {};
    const int n = indexIn(key, 2, m_consts.size());
    return n >= 0 ? s(m_consts.at(n).name) : QString();
}

QString Catalog::enumName(const QString &key) const
{
    if (!key.startsWith(QLatin1String("en"))) return {};
    const int n = indexIn(key, 2, m_enums.size());
    return n >= 0 ? s(m_enums.at(n).name) : QString();
}

QStringList Catalog::enumValues(const QString &name) const
{
    for (const Enum &e : m_enums) {
        if (s(e.name) != name) continue;
        QStringList out;
        for (int v : e.values) out << s(v);
        return out;
    }
    return {};
}

QString Catalog::doc(const QString &key) const
{
    if (key.startsWith(QLatin1String("en")) || key.startsWith(QLatin1String("co")))
        return {};
    if (key.startsWith('m')) {
        const int n = indexIn(key, 1, m_methods.size());
        return n >= 0 ? s(m_methods.at(n).doc) : QString();
    }
    if (key.startsWith('g')) {
        const int n = indexIn(key, 1, m_globals.size());
        return n >= 0 ? s(m_globals.at(n).doc) : QString();
    }
    return {};
}

NodeHelp Catalog::explain(const QString &key) const
{
    NodeHelp help;

    if (key.startsWith(QLatin1String("en"))) {
        const QString name = enumName(key);
        const QStringList vals = enumValues(name);
        const QStringList head = vals.mid(0, 8);
        help.summary = QStringLiteral("Enum with %1 values: %2%3")
                           .arg(vals.size()).arg(head.join(QStringLiteral(", ")),
                                vals.size() > 8 ? QStringLiteral(", ...") : QString());
        help.kind = QStringLiteral("Enum - constant value");
        help.effects << QStringLiteral(
            "Outputs the enum type. Pick a member on the pin that consumes it.");
        help.source = defFor(key).loc;
        help.documented = true;
        help.valid = true;
        return help;
    }

    if (key.startsWith(QLatin1String("co"))) {
        help.kind = QStringLiteral("Constant");
        help.effects << QStringLiteral(
            "Outputs a fixed engine constant. It has no exec pins and evaluates where it "
            "is used.");
        help.source = defFor(key).loc;
        help.valid = true;
        return help;
    }

    const MethodSig m = method(key);
    const MethodSig g = globalFn(key);
    const MethodSig &sig = m.valid ? m : g;
    if (!sig.valid) return help;

    const bool isEvent = sig.flags & flag::Event;
    const bool isPure = sig.flags & flag::Pure;
    const bool isStatic = sig.flags & flag::Static;
    const bool isCtor = sig.flags & flag::Ctor;
    const QString owner = m.valid ? m.owner : QStringLiteral("global");

    QVector<MethodSig::Param> outs;
    QStringList paramNames;
    for (const auto &p : sig.params) {
        paramNames << p.name;
        if (p.dir != 0) outs.append(p);
    }

    if (isEvent) {
        help.kind = QStringLiteral("Event - overridable hook on %1").arg(owner);
        help.effects << QStringLiteral(
            "Runs when the engine calls %1::%2. Chain nodes from its exec pin.")
                            .arg(owner, sig.name);
        help.effects << QStringLiteral(
            "`super` is called automatically. Turn it off in the Inspector only to "
            "replace the base behaviour.");
        if (!sig.params.isEmpty())
            help.effects << QStringLiteral(
                "The event's parameters (%1) are available as output pins.")
                                .arg(paramNames.join(QStringLiteral(", ")));
    } else if (isCtor) {
        help.kind = QStringLiteral("Constructor - %1").arg(owner);
        help.effects << QStringLiteral(
            "Creates a new %1 with `new`. The result comes out of the object pin.")
                            .arg(owner);
    } else if (isPure) {
        help.kind = QStringLiteral("Pure - reads from %1").arg(owner);
        help.effects << QStringLiteral(
            "It has no exec pins and evaluates inline wherever its output is used.");
        help.effects << QStringLiteral("Returns %1.").arg(sig.ret);
    } else {
        help.kind = m.valid ? QStringLiteral("Function - called on %1").arg(owner)
                            : QStringLiteral("Global function");
        if (!m.valid)
            help.effects << QStringLiteral("Calls the global %1().").arg(sig.name);
        else if (isStatic)
            help.effects << QStringLiteral("Calls the static %1.%2().").arg(owner, sig.name);
        else
            help.effects << QStringLiteral(
                "Calls %1() on whatever is wired to `target`, which defaults to `this`.")
                                .arg(sig.name);
        if (sig.ret != QLatin1String("void"))
            help.effects << QStringLiteral(
                "Returns %1; wiring the return pin stores it in a local first.").arg(sig.ret);
    }

    if (!outs.isEmpty()) {
        QStringList names;
        for (const auto &p : outs) names << QStringLiteral("`%1`").arg(p.name);
        help.effects << QStringLiteral(
            "Writes to %1 output pin%2 through %3, declared as locals before the call.")
                            .arg(outs.size()).arg(outs.size() > 1 ? "s" : "",
                                 names.join(QStringLiteral(", ")));
    }

    if (sig.flags & flag::Native)
        help.cautions << QStringLiteral(
            "Engine-implemented (`proto native`). You can call it, but a `modded class` "
            "override will not compile.");
    if (sig.flags & flag::Protected)
        help.cautions << QStringLiteral(
            "Declared `protected`, so it is only reachable from inside the class or a "
            "subclass.");

    for (const SearchRow &row : m_search) {
        if (row.key != key) continue;
        if (!row.guards.isEmpty())
            help.cautions << QStringLiteral(
                "Only compiled when %1 is defined, so it will not exist in every build.")
                                .arg(row.guards);
        break;
    }

    help.summary = doc(key);
    help.source = defFor(key).loc;
    help.documented = !help.summary.isEmpty();
    help.valid = true;
    return help;
}

// The index the catalogue is built from mis-reads a member declaration that
// carries an initialiser as a method, so 542 of the 29,024 method entries are
// named things like `m_Data = new AutotestConfigJson`. They are not callable
// and they cannot be a node; offering them turns a search for "<" into a list
// of half-declarations. Nothing else in the catalogue is filtered, so a real
// entry can never be lost this way.
bool isCallableName(const QString &name)
{
    if (name.isEmpty()) return false;
    if (!name.at(0).isLetter() && name.at(0) != QLatin1Char('_')) return false;
    for (const QChar c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) return false;
    return true;
}

void Catalog::buildSearchIndex()
{
    const auto sigOf = [this](const QVector<Param> &params, int ret) {
        QStringList args;
        for (const Param &p : params) {
            const QString prefix = p.dir == 1 ? QStringLiteral("out ")
                                              : p.dir == 2 ? QStringLiteral("inout ")
                                                           : QString();
            args << prefix + s(p.type);
        }
        const QString r = s(ret);
        return QStringLiteral("(%1)%2").arg(args.join(QStringLiteral(", ")),
                                            (!r.isEmpty() && r != QLatin1String("void"))
                                                ? QStringLiteral(" : %1").arg(r)
                                                : QString());
    };

    const auto push = [this](const QString &key, const QString &name,
                             const QString &title, const QString &sub,
                             const QString &sig, const QString &cat,
                             const QString &guards) {
        SearchRow row;
        row.key = key;
        row.name = name;
        row.hay = QStringLiteral("%1::%2").arg(sub, name).toLower();
        row.title = title;
        row.sub = sub;
        row.sig = sig;
        row.cat = cat;
        row.guards = guards;
        m_search.append(row);
    };

    // `override void EEInit()` is declared on 100+ classes, all the same hook,
    // so the palette shows only the base declaration.
    QSet<QString> baseDeclared;
    for (const Method &m : m_methods)
        if (!(m.flags & flag::Override)) baseDeclared.insert(s(m.name));

    m_search.reserve(m_methods.size() + m_globals.size() + m_enums.size() + m_consts.size());
    for (int i = 0; i < m_methods.size(); ++i) {
        const Method &m = m_methods.at(i);
        const QString name = s(m.name);
        if (!isCallableName(name)) continue;
        if ((m.flags & flag::Override) && baseDeclared.contains(name)) continue;
        const QString owner = classInfo(m.owner).name;
        const bool isEvent = m.flags & flag::Event;
        push(QStringLiteral("m%1").arg(i), name,
             isEvent ? QStringLiteral("Event %1").arg(name) : name,
             owner, sigOf(m.params, m.ret),
             isEvent ? QStringLiteral("Events")
                     : (m.flags & flag::Pure) ? QStringLiteral("Pure")
                                              : QStringLiteral("Functions"),
             s(m.guards));
    }
    for (int i = 0; i < m_globals.size(); ++i) {
        const Global &g = m_globals.at(i);
        push(QStringLiteral("g%1").arg(i), s(g.name), s(g.name),
             QStringLiteral("global"), sigOf(g.params, g.ret),
             QStringLiteral("Globals"), s(g.guards));
    }
    for (int i = 0; i < m_enums.size(); ++i) {
        const Enum &e = m_enums.at(i);
        push(QStringLiteral("en%1").arg(i), s(e.name), s(e.name),
             QStringLiteral("enum"),
             QStringLiteral("%1 values").arg(e.values.size()),
             QStringLiteral("Enums"), QString());
    }
    for (int i = 0; i < m_consts.size(); ++i) {
        const Const &c = m_consts.at(i);
        push(QStringLiteral("co%1").arg(i), s(c.name), s(c.name),
             QStringLiteral("const"), s(c.type),
             QStringLiteral("Constants"), QString());
    }
}

QVector<SearchHit> Catalog::search(const QString &query, const SearchOptions &opts) const
{
    const QString q = query.trimmed().toLower();
    const int limit = opts.limit > 0 ? opts.limit : 60;
    if (q.isEmpty() && opts.ofClass.isEmpty()) return {};

    QSet<QString> allowed;
    if (!opts.ofClass.isEmpty())
        for (const ClassInfo &c : ancestors(opts.ofClass))
            allowed.insert(c.name.toLower());

    QVector<SearchHit> out;
    for (const SearchRow &e : m_search) {
        if (!allowed.isEmpty() && !allowed.contains(e.sub.toLower())) continue;
        if (!opts.category.isEmpty() && e.cat != opts.category) continue;

        if (q.isEmpty()) {
            out.append({e.key, e.title, e.sub, e.sig, e.cat, 0, e.guards});
            if (out.size() >= limit * 4) break;
            continue;
        }

        const QString nameLower = e.name.toLower();
        int score = -1;
        if (nameLower == q) score = 1000;
        else if (nameLower.startsWith(q)) score = 800 - nameLower.size();
        else if (nameLower.contains(q)) score = 500 - nameLower.indexOf(q);
        else if (e.hay.contains(q)) score = 300;
        if (score < 0) continue;

        // prefer shallower / more commonly used owners
        if (e.sub == QLatin1String("EntityAI") || e.sub == QLatin1String("ItemBase")
            || e.sub == QLatin1String("PlayerBase")) score += 60;
        if (e.cat == QLatin1String("Events")) score += 25;
        // A guarded declaration may not exist in the build you ship against.
        // GAME_TEMPLATE's GetGame() is the classic decoy.
        if (!e.guards.isEmpty()) score -= 150;
        out.append({e.key, e.title, e.sub, e.sig, e.cat, score, e.guards});
    }

    std::sort(out.begin(), out.end(), [](const SearchHit &a, const SearchHit &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.title.compare(b.title) < 0;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}
