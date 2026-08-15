#include "pins.h"

#include <QHash>
#include <QRegularExpression>

QColor pinColor(PinKind kind)
{
    switch (kind) {
    case PinKind::Exec:     return QColor("#d5dce4");
    case PinKind::Bool:     return QColor("#d9534f");
    case PinKind::Int:      return QColor("#1eb980");
    case PinKind::Float:    return QColor("#9fdb2f");
    case PinKind::String:   return QColor("#e05fbf");
    case PinKind::Vector:   return QColor("#ffc14d");
    case PinKind::Object:   return QColor("#4da3ff");
    case PinKind::Enum:     return QColor("#b06fe0");
    case PinKind::Typename: return QColor("#7ee081");
    case PinKind::Any:      return QColor("#8b96a3");
    }
    return QColor("#8b96a3");
}

QString pinKindName(PinKind kind)
{
    switch (kind) {
    case PinKind::Exec: return QStringLiteral("exec");
    case PinKind::Bool: return QStringLiteral("bool");
    case PinKind::Int: return QStringLiteral("int");
    case PinKind::Float: return QStringLiteral("float");
    case PinKind::String: return QStringLiteral("string");
    case PinKind::Vector: return QStringLiteral("vector");
    case PinKind::Object: return QStringLiteral("object");
    case PinKind::Enum: return QStringLiteral("enum");
    case PinKind::Typename: return QStringLiteral("typename");
    case PinKind::Any: return QStringLiteral("any");
    }
    return QStringLiteral("any");
}

PinKind pinKindFromName(const QString &name)
{
    static const QHash<QString, PinKind> map = {
        {"exec", PinKind::Exec}, {"bool", PinKind::Bool}, {"int", PinKind::Int},
        {"float", PinKind::Float}, {"string", PinKind::String},
        {"vector", PinKind::Vector}, {"object", PinKind::Object},
        {"enum", PinKind::Enum}, {"typename", PinKind::Typename},
        {"any", PinKind::Any},
    };
    return map.value(name, PinKind::Any);
}

namespace {

const QHash<QString, PinKind> &primitives()
{
    static const QHash<QString, PinKind> map = {
        {"void", PinKind::Any}, {"bool", PinKind::Bool}, {"int", PinKind::Int},
        {"float", PinKind::Float}, {"string", PinKind::String},
        {"vector", PinKind::Vector}, {"typename", PinKind::Typename},
        {"auto", PinKind::Any}, {"Class", PinKind::Any},
        {"Managed", PinKind::Object},
    };
    return map;
}

const QHash<QString, QString> &arrayAliases()
{
    static const QHash<QString, QString> map = {
        {"TStringArray", "string"}, {"TIntArray", "int"},
        {"TFloatArray", "float"}, {"TVectorArray", "vector"},
        {"TClassArray", "Class"}, {"TTypenameArray", "typename"},
        {"TBoolArray", "bool"},
    };
    return map;
}

QStringList splitGenericArgs(const QString &s)
{
    QStringList out;
    int depth = 0;
    QString cur;
    for (const QChar c : s) {
        if (c == '<') depth++;
        else if (c == '>') depth--;
        if (c == ',' && depth == 0) {
            out << cur.trimmed();
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.trimmed().isEmpty()) out << cur.trimmed();
    return out;
}

} // namespace

PinType pinTypeOf(const QString &enforceType,
                  const std::function<bool(const QString &)> &isEnum)
{
    QString t = enforceType.isEmpty() ? QStringLiteral("void") : enforceType.trimmed();
    static const QRegularExpression modifiers(
        QStringLiteral("\\b(ref|autoptr|notnull|const|owned|local)\\b\\s*"));
    t.remove(modifiers);
    t = t.trimmed();

    if (arrayAliases().contains(t)) {
        PinType inner = pinTypeOf(arrayAliases().value(t), isEnum);
        inner.isArray = true;
        return inner;
    }

    static const QRegularExpression generic(
        QStringLiteral("^(array|set|multiMap|map)\\s*<(.+)>$"));
    const auto m = generic.match(t);
    if (m.hasMatch()) {
        // map<K,V> pins carry the value type; the key is not expressible
        const QStringList args = splitGenericArgs(m.captured(2));
        const bool isMap = m.captured(1) == "map" || m.captured(1) == "multiMap";
        QString elem = args.value(isMap ? 1 : 0);
        if (elem.isEmpty()) elem = args.value(0, QStringLiteral("auto"));
        PinType inner = pinTypeOf(elem, isEnum);
        inner.isArray = true;
        return inner;
    }

    if (t.endsWith(QStringLiteral("[]"))) {
        PinType inner = pinTypeOf(t.left(t.size() - 2), isEnum);
        inner.isArray = true;
        return inner;
    }

    if (primitives().contains(t)) return {primitives().value(t), {}, false};
    if (isEnum && isEnum(t)) return {PinKind::Enum, t, false};
    static const QRegularExpression ident(QStringLiteral("^[A-Za-z_]\\w*$"));
    if (ident.match(t).hasMatch()) return {PinKind::Object, t, false};
    return {PinKind::Any, {}, false};
}

QString defaultLiteral(const PinType &t)
{
    if (t.isArray) return QStringLiteral("null");
    switch (t.kind) {
    case PinKind::Bool: return QStringLiteral("false");
    case PinKind::Int: return QStringLiteral("0");
    case PinKind::Float: return QStringLiteral("0.0");
    case PinKind::String: return QStringLiteral("\"\"");
    case PinKind::Vector: return QStringLiteral("\"0 0 0\"");
    case PinKind::Typename: return QStringLiteral("string");
    case PinKind::Enum: return QStringLiteral("0");
    default: return QStringLiteral("null");
    }
}

InlineEditor inlineEditorFor(const PinType &t)
{
    if (t.isArray) return InlineEditor::None;
    switch (t.kind) {
    case PinKind::Bool: return InlineEditor::Checkbox;
    case PinKind::Int:
    case PinKind::Float: return InlineEditor::Number;
    case PinKind::String:
    case PinKind::Vector:
    case PinKind::Enum:
    case PinKind::Typename: return InlineEditor::Text;
    default: return InlineEditor::None;
    }
}
