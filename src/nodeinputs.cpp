#include "nodeinputs.h"

#include "document.h"

QString pinTypeName(const PinType &type)
{
    QString base;
    switch (type.kind) {
    case PinKind::Exec:     base = QStringLiteral("exec"); break;
    case PinKind::Bool:     base = QStringLiteral("bool"); break;
    case PinKind::Int:      base = QStringLiteral("int"); break;
    case PinKind::Float:    base = QStringLiteral("float"); break;
    case PinKind::String:   base = QStringLiteral("string"); break;
    case PinKind::Vector:   base = QStringLiteral("vector"); break;
    case PinKind::Typename: base = QStringLiteral("typename"); break;
    case PinKind::Object:
    case PinKind::Enum:     base = type.cls; break;
    case PinKind::Any:      base = QStringLiteral("auto"); break;
    }
    if (base.isEmpty()) base = QStringLiteral("auto");
    return type.isArray ? QStringLiteral("array<%1>").arg(base) : base;
}

QString declaredTypeName(const Catalog &cat, const QString &nodeRef, const Pin &pin)
{
    // Catalog::paramPins names an input "p<N>" for parameter N and keeps the
    // numbering even where an out parameter contributes no input pin, so the
    // index is a direct lookup rather than a count of the pins before it.
    if (pin.id.size() > 1 && pin.id.startsWith(QLatin1Char('p'))) {
        bool ok = false;
        const int index = pin.id.mid(1).toInt(&ok);
        if (ok && index >= 0) {
            MethodSig sig = cat.method(nodeRef);
            if (!sig.valid) sig = cat.globalFn(nodeRef);
            if (sig.valid && index < sig.params.size()) {
                const QString declared = sig.params.at(index).type;
                if (!declared.isEmpty()) return declared;
            }
        }
    }
    return pinTypeName(pin.type);
}

QVector<NodeInput> nodeInputsOf(const Document &doc, const QString &nodeId)
{
    QVector<NodeInput> out;
    const Graph *graph = doc.activeGraph();
    const GraphNode *node = graph ? graph->node(nodeId) : nullptr;
    if (!node) return out;

    const NodeDef def = doc.defForNode(*node);
    for (const Pin &pin : def.pins) {
        if (pin.dir != PinDir::In || pin.type.kind == PinKind::Exec) continue;

        NodeInput in;
        in.pinId = pin.id;
        in.label = pin.label.isEmpty() ? pin.id : pin.label;
        in.type = pin.type;
        in.typeName = declaredTypeName(doc.catalog(), node->ref, pin);
        in.connected = edgeInto(*graph, nodeId, pin.id) != nullptr;
        const auto stored = node->inputs.constFind(pin.id);
        in.overridden = stored != node->inputs.constEnd();
        in.value = in.overridden ? stored.value() : pin.def;
        in.editor = inlineEditorFor(pin.type);
        // Enum members come from the catalogue rather than the pin, because the
        // pin carries only the enum's name.
        if (in.type.kind == PinKind::Enum && !in.type.isArray)
            in.choices = doc.catalog().enumValues(in.type.cls);
        out.append(in);
    }
    return out;
}

NodeInput nodeInputOf(const Document &doc, const QString &nodeId,
                      const QString &pinId, bool *found)
{
    if (found) *found = false;
    for (const NodeInput &in : nodeInputsOf(doc, nodeId)) {
        if (in.pinId != pinId) continue;
        if (found) *found = true;
        return in;
    }
    return {};
}

void setNodeInput(Document *doc, const QString &nodeId, const QString &pinId,
                  const QString &value)
{
    Graph *graph = doc ? doc->activeGraph() : nullptr;
    const GraphNode *node = graph ? graph->node(nodeId) : nullptr;
    if (!node) return;
    const auto stored = node->inputs.constFind(pinId);
    if (stored != node->inputs.constEnd() && stored.value() == value) return;

    doc->beginEdit(QStringLiteral("Edit value"));
    // Re-resolved after the snapshot: taking a copy of the graph can reallocate
    // the node vector, and the pointer above would then be into freed memory.
    graph = doc->activeGraph();
    if (GraphNode *live = graph ? graph->node(nodeId) : nullptr)
        live->inputs.insert(pinId, value);
    doc->commitEdit();
}

bool isTrueLiteral(const QString &value)
{
    const QString v = value.trimmed();
    return v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
           || v == QLatin1String("1");
}

QString toggledBool(const QString &value)
{
    return isTrueLiteral(value) ? QStringLiteral("false") : QStringLiteral("true");
}
