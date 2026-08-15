// Overridable events for one class, ranked and grouped so the list is usable.
//
// The catalogue answers "which events does ItemBase have" with 227 entries in
// alphabetical order, which puts AddArrow, AddWet and a run of DebugBBox* above
// EEInit. That ordering is why nobody can find an event without already knowing
// its name. This module keeps every entry the catalogue returns and reorders
// them: a hand-checked table lifts the hooks a mod actually starts from into
// named groups, the long tail stays alphabetical underneath, and debug-only or
// deprecated entries sink to the bottom carrying a flag rather than being
// dropped. Silently hiding an entry is worse than ordering it badly, because a
// modder who wants DebugBBoxDraw has no other way to reach it.
//
// Only entries the catalogue files under "Events" appear here. A guard like
// CanPutInCargo reads like an event and is overridden like one, but the
// catalogue classes it as Pure, and NodeScene derives an Event node from the
// category alone, so listing it here would place a call node with a target pin
// instead of an entry point.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

class Catalog;

struct EventInfo {
    QString key;         // catalogue key, for Catalog::defFor and addNodeAt
    QString name;        // EEItemAttached
    QString owner;       // EntityAI
    QString signature;   // (EntityAI, string)
    QString summary;     // vanilla doc, cleaned, or a written fallback
    QString group;       // one of eventGroupOrder()
    int rank = 0;        // lower sorts first; already applied to the returned order
    bool deprecated = false;
    bool debugOnly = false;
};

// Group headings in display order. The panel reads this rather than deriving
// its own order, so a group added here shows up without a second edit.
QStringList eventGroupOrder();

// Every event `className` and its ancestors declare, sorted by rank then name.
// Empty when the catalogue does not know the class, so a graph naming a class
// from a mod the catalogue was not built against degrades to an empty list.
QVector<EventInfo> eventsForClass(const Catalog &cat, const QString &className);
