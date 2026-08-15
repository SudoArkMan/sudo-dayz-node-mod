// Enforce Script types -> graph pin kinds.
//
// Pins carry a coarse kind (for colour and compatibility) plus, for object
// pins, the concrete class name so connections can be checked against the
// real inheritance chain. Port of the reference implementation in the
// Electron build (src/nodes/types.ts); the .sdzn format depends on it.
#pragma once

#include <QColor>
#include <QString>
#include <functional>

enum class PinKind {
    Exec, Bool, Int, Float, String, Vector, Object, Enum, Typename, Any
};

struct PinType {
    PinKind kind = PinKind::Any;
    QString cls;          // class name for Object pins, enum name for Enum pins
    bool isArray = false;
};

enum class PinDir { In, Out };

struct Pin {
    QString id;
    QString label;
    PinDir dir = PinDir::In;
    PinType type;
    QString def;          // default literal when unconnected
    bool hasDef = false;
};

// Unreal-ish pin colours; exec is the neutral UI foreground.
QColor pinColor(PinKind kind);

// Parse an Enforce type name ("array<ref ItemBase>", "TStringArray",
// "PlayerBase") into a PinType. isEnum resolves enum names via the catalog.
PinType pinTypeOf(const QString &enforceType,
                  const std::function<bool(const QString &)> &isEnum);

// Literal for an unconnected input, so generated code always compiles.
QString defaultLiteral(const PinType &t);

enum class InlineEditor { None, Text, Number, Checkbox };

// Widget used to edit an unconnected input inline.
InlineEditor inlineEditorFor(const PinType &t);

// Round-trip helpers for the .sdzn JSON, which stores kinds as strings.
QString pinKindName(PinKind kind);
PinKind pinKindFromName(const QString &name);
