#include "scriptapi.h"

#include "catalog.h"

#include <QRegularExpression>

namespace {

Pin execPin(const QString &id, PinDir dir)
{
    Pin p;
    p.id = id;
    p.dir = dir;
    p.type = {PinKind::Exec, {}, false};
    return p;
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

// An absent `returns` and an explicit "void" are the same function: graph.cpp
// reads the key with toString(), so a function the importer wrote without one
// arrives here as an empty string. Every decision that keys off the return type
// has to see "void" for both, or a helper with no declared return grows a value
// pin the generator will not emit and a header with no type in front of it.
// codegen.cpp and analysis.cpp make the same normalisation on their own copies;
// if that ever moves into one shared place, this is the definition to keep.
QString functionReturnType(const GraphFunction &f)
{
    return f.returns.isEmpty() ? QStringLiteral("void") : f.returns;
}

} // namespace

QString functionSignature(const GraphFunction &f)
{
    QStringList params;
    for (const GraphParam &p : f.params)
        params << QStringLiteral("%1 %2").arg(p.type, p.name);
    return QStringLiteral("%1 %2(%3)").arg(functionReturnType(f), f.name,
                                           params.join(QStringLiteral(", ")));
}

NodeDef functionEntryDef(const GraphFunction &f, const IsEnumFn &isEnum)
{
    NodeDef def;
    def.key = QStringLiteral("fn.entry.%1").arg(f.id);
    def.title = QStringLiteral("Function %1").arg(f.name);
    def.subtitle = functionSignature(f);
    def.category = QStringLiteral("Lifecycle");
    def.accent = accents::event();
    def.pins.append(execPin(QStringLiteral("exec"), PinDir::Out));
    def.pins.append(dataPin(QStringLiteral("self"), QStringLiteral("self"), PinDir::Out,
                            {PinKind::Object, QStringLiteral("auto"), false}));
    for (int i = 0; i < f.params.size(); ++i)
        def.pins.append(dataPin(QStringLiteral("o%1").arg(i), f.params.at(i).name,
                                PinDir::Out, pinTypeOf(f.params.at(i).type, isEnum)));
    def.valid = true;
    return def;
}

NodeDef functionCallDef(const ScriptEntry &s, const GraphFunction &f,
                        const IsEnumFn &isEnum)
{
    // A value-returning helper with no side effects reads better without exec
    // pins, the same convention the vanilla catalogue uses.
    static const QRegularExpression accessor(QStringLiteral("^(Get|Is|Has|Can|To|From)"));
    const QString returns = functionReturnType(f);
    const bool returnsValue = returns != QLatin1String("void");
    const bool pure = returnsValue
                      && (f.isStatic || accessor.match(f.name).hasMatch());

    NodeDef def;
    def.key = QStringLiteral("fn.call.%1.%2").arg(s.id, f.id);
    if (!pure) {
        def.pins.append(execPin(QStringLiteral("exec"), PinDir::In));
        def.pins.append(execPin(QStringLiteral("exec"), PinDir::Out));
    }
    if (!f.isStatic)
        def.pins.append(dataPin(QStringLiteral("target"), QStringLiteral("target"),
                                PinDir::In, {PinKind::Object, s.name, false}));
    for (int i = 0; i < f.params.size(); ++i)
        def.pins.append(dataPin(QStringLiteral("p%1").arg(i), f.params.at(i).name,
                                PinDir::In, pinTypeOf(f.params.at(i).type, isEnum)));
    if (returnsValue)
        def.pins.append(dataPin(QStringLiteral("ret"), QStringLiteral("return"),
                                PinDir::Out, pinTypeOf(returns, isEnum)));

    def.title = f.isStatic ? QStringLiteral("%1.%2").arg(s.name, f.name) : f.name;
    def.subtitle = f.isStatic ? QStringLiteral("static - %1").arg(returns) : s.name;
    def.category = QStringLiteral("Functions");
    def.accent = pure ? accents::pure() : accents::call();
    def.pure = pure;
    def.valid = true;
    return def;
}

NodeDef scriptVarGetDef(const ScriptEntry &s, const GraphVariable &v,
                        const IsEnumFn &isEnum)
{
    NodeDef def;
    def.key = QStringLiteral("sv.get.%1.%2").arg(s.id, v.id);
    def.title = v.name;
    def.subtitle = QStringLiteral("%1 - get").arg(s.name);
    def.category = QStringLiteral("Variables");
    def.accent = accents::variable();
    def.pure = true;
    def.pins.append(dataPin(QStringLiteral("target"), QStringLiteral("target"),
                            PinDir::In, {PinKind::Object, s.name, false}));
    def.pins.append(dataPin(QStringLiteral("ret"), QString(), PinDir::Out,
                            pinTypeOf(v.type, isEnum)));
    def.valid = true;
    return def;
}

NodeDef scriptVarSetDef(const ScriptEntry &s, const GraphVariable &v,
                        const IsEnumFn &isEnum)
{
    NodeDef def;
    def.key = QStringLiteral("sv.set.%1.%2").arg(s.id, v.id);
    def.title = QStringLiteral("Set %1").arg(v.name);
    def.subtitle = s.name;
    def.category = QStringLiteral("Variables");
    def.accent = accents::variable();
    def.pins.append(execPin(QStringLiteral("exec"), PinDir::In));
    def.pins.append(execPin(QStringLiteral("exec"), PinDir::Out));
    def.pins.append(dataPin(QStringLiteral("target"), QStringLiteral("target"),
                            PinDir::In, {PinKind::Object, s.name, false}));
    def.pins.append(dataPin(QStringLiteral("v"), QStringLiteral("value"), PinDir::In,
                            pinTypeOf(v.type, isEnum)));
    def.valid = true;
    return def;
}

bool isScriptNodeKey(const QString &key)
{
    return key.startsWith(QLatin1String("fn.entry."))
           || key.startsWith(QLatin1String("fn.call."))
           || key.startsWith(QLatin1String("sv."));
}

NodeDef scriptDefFor(const QString &key, const Project &project, const IsEnumFn &isEnum)
{
    if (key.isEmpty()) return {};

    if (key.startsWith(QLatin1String("fn.entry."))) {
        const QString fnId = key.mid(9);
        for (const ScriptEntry &s : project.scripts)
            for (const GraphFunction &f : s.graph.functions)
                if (f.id == fnId) return functionEntryDef(f, isEnum);
        return {};
    }

    static const QRegularExpression callRe(QStringLiteral("^fn\\.call\\.([^.]+)\\.(.+)$"));
    const auto call = callRe.match(key);
    if (call.hasMatch()) {
        for (const ScriptEntry &s : project.scripts) {
            if (s.id != call.captured(1)) continue;
            for (const GraphFunction &f : s.graph.functions)
                if (f.id == call.captured(2)) return functionCallDef(s, f, isEnum);
        }
        return {};
    }

    static const QRegularExpression memberRe(
        QStringLiteral("^sv\\.(get|set)\\.([^.]+)\\.(.+)$"));
    const auto member = memberRe.match(key);
    if (member.hasMatch()) {
        for (const ScriptEntry &s : project.scripts) {
            if (s.id != member.captured(2)) continue;
            for (const GraphVariable &v : s.graph.variables) {
                if (v.id != member.captured(3)) continue;
                return member.captured(1) == QLatin1String("get")
                           ? scriptVarGetDef(s, v, isEnum)
                           : scriptVarSetDef(s, v, isEnum);
            }
        }
        return {};
    }

    return {};
}

CallTarget resolveCall(const QString &key, const Project &project)
{
    CallTarget out;
    static const QRegularExpression callRe(QStringLiteral("^fn\\.call\\.([^.]+)\\.(.+)$"));
    const auto m = callRe.match(key);
    if (!m.hasMatch()) return out;
    for (const ScriptEntry &s : project.scripts) {
        if (s.id != m.captured(1)) continue;
        for (const GraphFunction &f : s.graph.functions) {
            if (f.id != m.captured(2)) continue;
            out.script = &s;
            out.fn = &f;
            out.valid = true;
            return out;
        }
    }
    return out;
}

EntryTarget resolveEntry(const QString &key, const Project &project)
{
    EntryTarget out;
    if (!key.startsWith(QLatin1String("fn.entry."))) return out;
    const QString id = key.mid(9);
    for (const ScriptEntry &s : project.scripts)
        for (const GraphFunction &f : s.graph.functions)
            if (f.id == id) {
                out.fn = &f;
                out.valid = true;
                return out;
            }
    return out;
}

MemberTarget resolveMember(const QString &key, const Project &project)
{
    MemberTarget out;
    static const QRegularExpression memberRe(
        QStringLiteral("^sv\\.(get|set)\\.([^.]+)\\.(.+)$"));
    const auto m = memberRe.match(key);
    if (!m.hasMatch()) return out;
    for (const ScriptEntry &s : project.scripts) {
        if (s.id != m.captured(2)) continue;
        for (const GraphVariable &v : s.graph.variables) {
            if (v.id != m.captured(3)) continue;
            out.setter = m.captured(1) == QLatin1String("set");
            out.variable = &v;
            out.script = &s;
            out.valid = true;
            return out;
        }
    }
    return out;
}

QVector<NodeDef> projectApiDefs(const Project &project, const IsEnumFn &isEnum)
{
    QVector<NodeDef> out;
    for (const ScriptEntry &s : project.scripts) {
        for (const GraphFunction &f : s.graph.functions) {
            out.append(functionEntryDef(f, isEnum));
            out.append(functionCallDef(s, f, isEnum));
        }
        for (const GraphVariable &v : s.graph.variables) {
            out.append(scriptVarGetDef(s, v, isEnum));
            out.append(scriptVarSetDef(s, v, isEnum));
        }
    }
    return out;
}
