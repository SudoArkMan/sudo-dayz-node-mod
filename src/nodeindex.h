// The node list, arranged by what you are trying to do.
//
// The catalogue answers "find the node called X". That is no use to somebody
// who does not already know that EEItemAttached, ConfigGetString or
// RegisterNetSyncVariableBool exist, and it is the whole reason a five second
// timer took four wrong turns to build. This module answers the other question:
// given a job, which nodes do it.
//
// Ordering is not taste. Every group carries the count that put it where it is,
// measured over DayZ Expansion in EXPANSION.md: 1,154 `Class.CastTo`, 529
// deferral sites, 412 runtime `IsServer` against 269 compile-time guards, 398
// config reads, 314 RPC registrations. A method used twice in vanilla does not
// sit at the same depth as one of those.
//
// Two rules the groups answer to. Names say what the thing does, not what the
// engine calls it: the reader is a DayZ modder, not a Blueprint user, and a
// heading named after an engine subsystem is no better than the flat list it
// replaced. And nothing is hidden: every builtin the index does not name is
// collected into a final group, so a builtin added to builtins.cpp shows up
// here without a second edit rather than silently disappearing.
//
// This lives beside the model rather than in the panel because two surfaces
// show it (the Node Palette dock and the canvas add-node menu, which is also
// the drag-out menu), and because a test has to be able to prove every curated
// row still resolves after the catalogue is rebuilt.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

class Builtins;
class Catalog;

// One row. `key` is what gets placed: a builtin id ("bi.branch"), a catalogue
// key ("m4256"), or one of the action keys below, which are not node keys at
// all and are acted on by whoever draws the row.
struct IndexRow {
    QString key;
    QString title;
    QString detail;  // dim right-hand text: the class that declares it, or the
                     // shape a builtin emits
    QString doc;     // one line, so a node can be read before it is placed
    // Words somebody types looking for this row that are nowhere in its title.
    // Never drawn. "new timer" is the whole reason it exists: the node that
    // answers it is called Set Timer, and no declaration in the catalogue
    // carries the word new, so the search used to come back with nothing at all
    // for the most obvious thing a modder can ask for.
    QString alias;
};

struct IndexGroup {
    QString title;
    QString doc;  // what the group is for, and the count behind it
    QVector<IndexRow> rows;
};

namespace nodeindex {

// The row that hands over to the events list instead of placing a node. Not a
// node key, so it can never collide with one. The same string the canvas menu
// has used for that row since it gained one.
extern const QLatin1String BrowseEventsKey;

} // namespace nodeindex

// The whole index for a graph that compiles into `selfClass` (from
// selfClassOf(); empty when the graph has no base class yet).
//
// A curated catalogue row is dropped when the generated script could not call
// it: protected and out of reach, or declared on a class this one does not
// descend from. That is the same rule the generator applies in callTarget(), so
// the index cannot offer a row that turns into a compile error. A name the
// catalogue no longer has is dropped too, which is what keeps this from
// claiming methods a DayZ update removed.
QVector<IndexGroup> nodeIndex(const Catalog &cat, const Builtins &builtins,
                              const QString &selfClass);

// True when `query` matches a row described by `fields` (its title, the class
// beside it, the group it sits in). The same reading Catalog::search takes:
// whitespace splits terms, every term has to land on one of the fields, and the
// terms joined up are how a declaration spells what the reader spelled apart.
// Without it "config get" answered nothing from the index while the catalogue
// answered eleven rows, which reads as the index being broken.
bool matchesQuery(const QString &query, const QStringList &fields);

// The same question asked of a whole row, so the palette dock and the canvas
// menu read one the same way. Both used to spell the field list out for
// themselves, which is how one of them would have kept its own idea of what a
// row is once aliases existed.
bool rowMatches(const QString &query, const IndexRow &row, const QString &groupTitle);

// False for a search hit a graph compiling into `selfClass` should not be
// offered: an event declared on a class it does not descend from.
//
// An Event node is an override and nothing else. One taken from an unrelated
// class generates a method with the right name on the wrong class, which
// compiles and is never called, and finding that out costs an afternoon.
// Typing "oninit" into a MissionServer graph offered Backlit, DebugPrint,
// SymptomBase and four more beside the one that works.
//
// Only events are judged: a call is legitimately made on somebody else's
// object through its target pin, which is the whole point of that pin. Nothing
// is withheld when the class is empty or the catalogue does not know it,
// because then nothing can be proved. `category` and `owner` are SearchHit's
// own fields, passed rather than the hit itself so this header does not have
// to pull in the catalogue.
bool eventFitsClass(const Catalog &cat, const QString &category, const QString &owner,
                    const QString &selfClass);

// One line saying what `key` does, for a footer that reads before a node is
// placed. The vanilla doc when there is one, the declaration when there is not,
// and the builtin's own text for a builtin. Empty for a key nothing knows.
QString nodeSummary(const Catalog &cat, const Builtins &builtins, const QString &key);

// The cautions attached to `key`, for a tooltip: the same warnings the
// inspector shows once a node has been placed, available before it is.
QStringList nodeCautions(const Catalog &cat, const Builtins &builtins,
                         const QString &key);

// Every curated reference the table names, as written ("bi.branch",
// "CGame::IsServer"). For the test that proves they all still resolve.
QStringList curatedRefs();
