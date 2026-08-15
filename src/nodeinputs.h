// The literal inputs of one node, as plain data.
//
// A value that is not wired has to be typed somewhere. The canvas draws these
// on the node itself and the inspector lists them in a dock, and neither
// surface may reach into the other: NodeItem's pin layout is private and the
// panel has only a Document. So both read nodeInputsOf and both write through
// setNodeInput, which is the only place a literal is committed. Two writers
// would mean two undo entries for one gesture and two versions of the truth.
//
// Nothing here includes a widget or a graphics header, so a panel can take it
// without taking the canvas with it.
#pragma once

#include "pins.h"

#include <QString>
#include <QStringList>
#include <QVector>

class Catalog;
class Document;

struct NodeInput {
    QString pinId;
    QString label;           // the pin's label, falling back to its id
    QString typeName;        // Enforce type as the declaration writes it
    PinType type;            // parsed form, for pinColor and the editor choice
    QString value;           // literal in force: the override, or the default
    bool overridden = false; // the node carries its own value for this pin
    bool connected = false;  // a wire drives it, so the literal is unused
    InlineEditor editor = InlineEditor::None;
    QStringList choices;     // enum members; empty for every other kind
};

// Every data input of `nodeId`, in the order the node draws them. Exec pins and
// outputs are left out; pins nothing can be typed into (objects, arrays) are
// kept with editor == None, because a row saying "wire this one" is more use
// than a gap where a parameter should be.
QVector<NodeInput> nodeInputsOf(const Document &doc, const QString &nodeId);

// One input by pin id. `found` is set false when the node or the pin is gone.
NodeInput nodeInputOf(const Document &doc, const QString &nodeId,
                      const QString &pinId, bool *found = nullptr);

// A type name from a parsed pin. Pins keep the kind and, for objects and enums,
// the class name, so this is a reconstruction: array<T> for the array flag and
// the primitive spelling otherwise.
QString pinTypeName(const PinType &type);

// The type the vanilla declaration actually wrote ("ref ItemBase",
// "TStringArray"). Method and global nodes number their input pins after their
// parameters, so the signature can be asked; anything else falls back to
// pinTypeName.
QString declaredTypeName(const Catalog &cat, const QString &nodeRef, const Pin &pin);

// The one writer. Commits `value` as the literal for `pinId` inside a single
// beginEdit/commitEdit, so the change lands on the undo stack exactly once
// whichever surface asked for it. Writing the value that is already there does
// nothing: commitEdit pushes an entry unconditionally, and a panel echoing a
// field back on every refresh would otherwise fill the history with steps that
// undo to the same graph.
void setNodeInput(Document *doc, const QString &nodeId, const QString &pinId,
                  const QString &value);

// The value a bool input takes when it is clicked. Kept here so the checkbox on
// the node and the checkbox in the panel agree on what "not true" is written as.
QString toggledBool(const QString &value);
bool isTrueLiteral(const QString &value);
