// Headless check of the two ranked lists the UI browses by: the event list for
// one class, and the task index the Node Palette and the canvas menu both show.
// Both are hand-curated tables of names over a catalogue that is regenerated
// whenever the DayZ scripts move, so both need proving against the catalogue
// that ships rather than against the one they were written on. A name that
// stops resolving is a row that quietly disappears from the palette, which is
// the failure this file exists to make loud.
//
//   cmake --build . --target eventstest && ./tests/eventstest ../resources
#include "builtins.h"
#include "catalog.h"
#include "events.h"
#include "nodeindex.h"

#include <QCoreApplication>
#include <QSet>
#include <QTextStream>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) failures++;
}

static int indexOfName(const QVector<EventInfo> &events, const QString &name)
{
    for (int i = 0; i < events.size(); ++i)
        if (events.at(i).name == name) return i;
    return -1;
}

static bool hasName(const QVector<EventInfo> &events, const QString &name)
{
    return indexOfName(events, name) >= 0;
}

// The catalogue key for one declaration, named by the class that declares it.
// Search alone is not enough: it ranks over the whole ancestor chain, so asking
// Timer for "Run" can answer with a Run declared further up.
static QString keyOf(const Catalog &cat, const QString &owner, const QString &name)
{
    SearchOptions opts;
    opts.limit = 200;
    opts.ofClass = owner;
    for (const SearchHit &h : cat.search(name, opts)) {
        const MethodSig sig = cat.method(h.key);
        if (sig.valid && sig.owner == owner && sig.name == name) return h.key;
    }
    return {};
}

static bool hasPin(const NodeDef &def, const QString &id, PinDir dir)
{
    for (const Pin &p : def.pins)
        if (p.id == id && p.dir == dir) return true;
    return false;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("resources");

    Catalog cat;
    const bool loaded = cat.load(root + "/catalog.json");
    check(loaded, QStringLiteral("loads catalog.json (%1)").arg(cat.error()));
    if (!loaded) return 1;

    out << "coverage" << Qt::endl;
    const QVector<EventInfo> item = eventsForClass(cat, QStringLiteral("ItemBase"));
    // Every event ItemBase and its ancestors declare. Nothing is filtered out,
    // so this has to match what the catalogue itself returns for the category.
    SearchOptions opts;
    opts.category = QStringLiteral("Events");
    opts.ofClass = QStringLiteral("ItemBase");
    opts.limit = 8192;
    const int rawCount = cat.search(QString(), opts).size();
    // 227 before event-ness was decided per class rather than per method name.
    // The number moved in both directions: `SetTemperature` and `DebugBBoxDraw`
    // left, because their only overriders are on Hud and Component and neither
    // is an ancestor of anything here, and `OnInventoryInit`, `EEInventoryIn`
    // and 36 more arrived, because they are hooks nothing in vanilla happens to
    // override and the old rule could not see them.
    check(item.size() == 263,
          QStringLiteral("ItemBase yields 263 events, got %1").arg(item.size()));
    check(item.size() == rawCount,
          QStringLiteral("nothing dropped: %1 ranked vs %2 in the catalogue")
              .arg(item.size()).arg(rawCount));

    // Access filtering must not touch this list. Every row here is declared on
    // ItemBase or one of its ancestors, so a protected one among them is a hook
    // ItemBase inherits and is legal to override; withholding it would take
    // real work away from the user rather than prevent a compile error.
    SearchOptions gated = opts;
    gated.selfClass = QStringLiteral("ItemBase");
    gated.respectAccess = true;
    check(cat.search(QString(), gated).size() == rawCount,
          QStringLiteral("an ItemBase graph is offered all %1 of its events")
              .arg(rawCount));

    // Point the same rows at a class that inherits none of them and the
    // protected ones drop out, which is the half of the rule that stops
    // `m_Timer.SetRunning(false)` being buildable.
    SearchOptions foreign = gated;
    foreign.selfClass = QStringLiteral("Mission");
    const int foreignCount = cat.search(QString(), foreign).size();
    int protectedRows = 0;
    for (const SearchHit &h : cat.search(QString(), opts))
        if (cat.method(h.key).flags & flag::Protected) protectedRows++;
    out << "       ItemBase events: " << rawCount << ", protected among them "
        << protectedRows << Qt::endl;
    check(protectedRows > 0,
          QStringLiteral("%1 of ItemBase's events are protected").arg(protectedRows));
    check(foreignCount == rawCount - protectedRows,
          QStringLiteral("a class that inherits none of them loses exactly those "
                         "%1 (%2 left of %3)")
              .arg(protectedRows).arg(foreignCount).arg(rawCount));

    // The names a mod actually starts from all have to be present.
    for (const QString &name : {QStringLiteral("EEInit"),
                                QStringLiteral("EEDelete"),
                                QStringLiteral("EEItemAttached"),
                                QStringLiteral("EEItemDetached"),
                                QStringLiteral("EEHitBy"),
                                QStringLiteral("EEKilled"),
                                QStringLiteral("EEHealthLevelChanged"),
                                QStringLiteral("OnStoreSave"),
                                QStringLiteral("OnStoreLoad"),
                                QStringLiteral("OnWorkStart"),
                                QStringLiteral("OnItemLocationChanged"),
                                QStringLiteral("OnPlacementComplete"),
                                QStringLiteral("EOnFrame")})
        check(hasName(item, name), QStringLiteral("ItemBase has %1").arg(name));

    out << "what counts as an event" << Qt::endl;
    // Event-ness belongs to one declaration on one class. Deciding it by method
    // name made every `Run` in the tree an event because `WorkbenchPlugin`
    // declares `event void Run()`, and an event node has no input pins at all,
    // so `Timer::Run` could not be reached from a Timer pin and a five second
    // timer needed a raw text node to write.
    const QString timerRun = keyOf(cat, QStringLiteral("Timer"), QStringLiteral("Run"));
    check(!timerRun.isEmpty(), QStringLiteral("Timer::Run is in the catalogue"));
    if (!timerRun.isEmpty()) {
        check(!(cat.method(timerRun).flags & flag::Event),
              QStringLiteral("Timer::Run is a call, not an event"));
        const NodeDef def = cat.defFor(timerRun);
        check(hasPin(def, QStringLiteral("exec"), PinDir::In),
              QStringLiteral("Timer::Run takes an exec input"));
        check(hasPin(def, QStringLiteral("target"), PinDir::In),
              QStringLiteral("Timer::Run can be reached from a Timer pin"));
    }

    struct Case { const char *owner; const char *name; bool event; const char *why; };
    static const Case cases[] = {
        {"WorkbenchPlugin", "Run", true,
         "declared with the `event` keyword, which the language settles"},
        {"PlayerBase", "OnReconnect", true,
         "a hook on a leaf class: nothing in vanilla extends PlayerBase, so no "
         "override exists to point at it"},
        {"EntityAI", "OnInventoryInit", true,
         "a hook the old name-global rule missed outright"},
        {"EntityAI", "SetTemperature", false,
         "its only overrider is IngameHud, which descends from Hud, not EntityAI"},
        {"EntityAI", "DebugBBoxDraw", false,
         "its only overrider descends from Component"},
        {"IEntity", "SetName", false, "a setter, overridden only on unrelated classes"},
        {"IEntity", "Update", false, "proto native: an override of it does not compile"},
        {"ItemBase", "SplitItem", true,
         "Magazine overrides it through the InventoryItemSuper typedef"},
    };
    for (const Case &c : cases) {
        const QString key = keyOf(cat, QLatin1String(c.owner), QLatin1String(c.name));
        if (key.isEmpty()) {
            check(false, QStringLiteral("%1::%2 is in the catalogue")
                             .arg(QLatin1String(c.owner), QLatin1String(c.name)));
            continue;
        }
        const bool isEvent = cat.method(key).flags & flag::Event;
        check(isEvent == c.event,
              QStringLiteral("%1::%2 is %3an event: %4")
                  .arg(QLatin1String(c.owner), QLatin1String(c.name),
                       c.event ? QString() : QStringLiteral("not "),
                       QLatin1String(c.why)));
    }

    out << "catalogue keys" << Qt::endl;
    // A curated row that names an event the catalogue spells differently would
    // show a summary with no node behind it, so every key has to build a def.
    int badKeys = 0;
    int emptyPins = 0;
    for (const EventInfo &e : item) {
        const NodeDef def = cat.defFor(e.key);
        if (!def.valid) {
            badKeys++;
            out << "    unresolved key " << e.key << " for " << e.name << Qt::endl;
            continue;
        }
        // An event node starts a flow, so it always has an outgoing exec pin,
        // and every pin it carries needs an id.
        bool exec = false;
        bool named = true;
        for (const Pin &p : def.pins) {
            if (p.id.isEmpty()) named = false;
            if (p.id == QLatin1String("exec") && p.dir == PinDir::Out) exec = true;
        }
        if (!exec || !named) {
            emptyPins++;
            out << "    bad pins on " << e.name << " (" << e.key << ")" << Qt::endl;
        }
    }
    check(badKeys == 0, QStringLiteral("every key resolves (%1 bad)").arg(badKeys));
    check(emptyPins == 0,
          QStringLiteral("every event def has valid pins (%1 bad)").arg(emptyPins));

    out << "grouping" << Qt::endl;
    const QStringList groups = eventGroupOrder();
    check(!groups.isEmpty(), QStringLiteral("group order is published"));
    check(groups.first() == QStringLiteral("Lifecycle"),
          QStringLiteral("Lifecycle leads, got '%1'").arg(groups.value(0)));
    check(groups.last() == QStringLiteral("Everything else"),
          QStringLiteral("the tail group is last, got '%1'").arg(groups.last()));

    QSet<QString> known(groups.begin(), groups.end());
    int strayGroup = 0;
    for (const EventInfo &e : item)
        if (!known.contains(e.group)) strayGroup++;
    check(strayGroup == 0,
          QStringLiteral("every event lands in a published group (%1 stray)")
              .arg(strayGroup));

    // The order the panel renders is the order it gets back, so the groups have
    // to arrive in blocks rather than interleaved.
    int lastGroup = -1;
    bool blocked = true;
    QSet<int> closed;
    for (const EventInfo &e : item) {
        const int g = groups.indexOf(e.group);
        if (g == lastGroup) continue;
        if (closed.contains(g)) blocked = false;
        closed.insert(lastGroup);
        lastGroup = g;
    }
    check(blocked, QStringLiteral("groups come back contiguous, not interleaved"));

    check(indexOfName(item, QStringLiteral("EEInit")) == 0,
          QStringLiteral("EEInit is the first entry"));

    out << "ranking" << Qt::endl;
    const int eeInit = indexOfName(item, QStringLiteral("EEInit"));
    int firstDebug = -1;
    int lastLive = -1;
    int firstDeprecated = -1;
    int debugCount = 0;
    int deprecatedCount = 0;
    for (int i = 0; i < item.size(); ++i) {
        const EventInfo &e = item.at(i);
        if (e.debugOnly) {
            debugCount++;
            if (firstDebug < 0) firstDebug = i;
        }
        if (e.deprecated) {
            deprecatedCount++;
            if (firstDeprecated < 0) firstDeprecated = i;
        }
        if (!e.debugOnly && !e.deprecated) lastLive = i;
    }
    check(debugCount > 0, QStringLiteral("%1 debug entries kept, not hidden").arg(debugCount));
    check(deprecatedCount > 0,
          QStringLiteral("%1 deprecated entries kept, not hidden").arg(deprecatedCount));
    check(hasName(item, QStringLiteral("OnDebugSpawn")),
          QStringLiteral("OnDebugSpawn is still reachable"));
    check(eeInit >= 0 && firstDebug > eeInit,
          QStringLiteral("EEInit (%1) sorts above the first debug entry (%2)")
              .arg(eeInit).arg(firstDebug));
    check(firstDeprecated > lastLive,
          QStringLiteral("deprecated entries sort last: first at %1, last live at %2")
              .arg(firstDeprecated).arg(lastLive));
    check(firstDeprecated > firstDebug,
          QStringLiteral("deprecated sorts below debug (%1 vs %2)")
              .arg(firstDeprecated).arg(firstDebug));

    // Only a capitalised DEPRECATED marks an entry. This one's doc ends "timeout
    // paramter is deprecated", which is about an argument, not the hook, and a
    // case-insensitive match would bury a live event on the strength of it.
    // EntityAI::SetTemperature used to be the witness here and is a plain call
    // now, so the check moved to the class that still carries one.
    const QVector<EventInfo> trigger = eventsForClass(cat, QStringLiteral("Trigger"));
    const int insiders = indexOfName(trigger, QStringLiteral("UpdateInsiders"));
    check(insiders >= 0 && !trigger.at(insiders).deprecated,
          QStringLiteral("UpdateInsiders is not read as deprecated"));

    // Both ConvertNonlethalDamage overloads survive; only the documented one
    // is marked, and it goes to the bottom while the live one keeps its group.
    int convertLive = -1;
    int convertDead = -1;
    for (int i = 0; i < item.size(); ++i) {
        if (item.at(i).name != QLatin1String("ConvertNonlethalDamage")) continue;
        if (item.at(i).deprecated) convertDead = i;
        else convertLive = i;
    }
    check(convertLive >= 0 && convertDead > convertLive,
          QStringLiteral("both ConvertNonlethalDamage overloads kept, deprecated last"));
    check(convertLive >= 0
              && item.at(convertLive).group == QStringLiteral("Damage and death"),
          QStringLiteral("the live ConvertNonlethalDamage stays in its group"));

    out << "summaries" << Qt::endl;
    int summarised = 0;
    for (const EventInfo &e : item)
        if (!e.summary.isEmpty()) summarised++;
    check(summarised > 120,
          QStringLiteral("%1 of %2 events carry a summary").arg(summarised).arg(item.size()));

    // Every curated entry needs one, since the whole point of the group is that
    // the name alone tells a newcomer nothing.
    int missing = 0;
    for (const EventInfo &e : item) {
        if (e.group == QStringLiteral("Everything else")) continue;
        if (!e.summary.isEmpty()) continue;
        missing++;
        out << "    no summary for " << e.name << Qt::endl;
    }
    check(missing == 0, QStringLiteral("every curated event has a summary (%1 missing)")
                            .arg(missing));

    // Summaries reach the UI, and vanilla's docs carry characters this project
    // does not ship. cleanDoc has to strip them.
    int banned = 0;
    for (const EventInfo &e : item) {
        for (const QChar c : e.summary) {
            if (c == QChar(0x00b7) || c == QChar(0x2026) || c == QChar(0x2014)
                || c == QChar(0x2013) || c == QChar(0x2018) || c == QChar(0x2019)
                || c == QChar(0x201c) || c == QChar(0x201d)) {
                banned++;
                out << "    banned character in " << e.name << ": " << e.summary
                    << Qt::endl;
                break;
            }
        }
        if (e.summary.contains(QLatin1String("@"))
            || e.summary.contains(QLatin1String("\\brief"))) {
            banned++;
            out << "    doxygen markup left in " << e.name << ": " << e.summary
                << Qt::endl;
        }
    }
    check(banned == 0, QStringLiteral("no doxygen markup or banned characters (%1)")
                           .arg(banned));

    out << "inheritance" << Qt::endl;
    const QVector<EventInfo> player = eventsForClass(cat, QStringLiteral("PlayerBase"));
    check(player.size() > item.size(),
          QStringLiteral("PlayerBase yields more than ItemBase (%1 vs %2)")
              .arg(player.size()).arg(item.size()));
    // Player events that live on Man, Human and DayZPlayerImplement, none of
    // which are ancestors of ItemBase.
    for (const QString &name : {QStringLiteral("EEItemIntoHands"),
                                QStringLiteral("EEItemOutOfHands"),
                                QStringLiteral("OnReconnect"),
                                QStringLiteral("OnJumpStart"),
                                QStringLiteral("OnCommandMoveStart")}) {
        check(hasName(player, name), QStringLiteral("PlayerBase has %1").arg(name));
        check(!hasName(item, name), QStringLiteral("ItemBase does not have %1").arg(name));
    }
    // The shared EntityAI hooks are on both.
    check(hasName(player, QStringLiteral("EEInit")),
          QStringLiteral("PlayerBase still inherits EEInit"));

    // The two a real mod reopens PlayerBase for most: 97 overrides of
    // SetActions and 92 of Init across DayZ Expansion, against 6 of EEKilled.
    // Neither rule the catalogue generator applies can see them, because
    // nothing in vanilla extends PlayerBase to override them and neither name
    // follows the On* convention, so build-catalog.mjs names them. They are
    // pinned here because losing them is invisible: the palette would still
    // offer both as calls, and a call node cannot override anything.
    for (const QString &name : {QStringLiteral("Init"), QStringLiteral("SetActions")}) {
        check(hasName(player, name),
              QStringLiteral("PlayerBase can still override %1").arg(name));
        const int at = indexOfName(player, name);
        check(at >= 0 && !player.at(at).summary.isEmpty(),
              QStringLiteral("%1 keeps the summary the curated table wrote for it")
                  .arg(name));
    }

    // A player graph gets the same treatment: the hooks a PlayerBase mod starts
    // from are curated too, not left in the alphabetical tail of 289.
    int playerMissing = 0;
    for (const QString &name : {QStringLiteral("OnPlayerLoaded"),
                                QStringLiteral("OnReconnect"),
                                QStringLiteral("EEItemIntoHands"),
                                QStringLiteral("OnTick"),
                                QStringLiteral("OnStanceChange"),
                                QStringLiteral("OnJumpStart")}) {
        const int at = indexOfName(player, name);
        if (at < 0 || player.at(at).group == QStringLiteral("Everything else")
            || player.at(at).summary.isEmpty())
            playerMissing++;
    }
    check(playerMissing == 0,
          QStringLiteral("player hooks are grouped and summarised (%1 are not)")
              .arg(playerMissing));

    int playerBad = 0;
    for (const EventInfo &e : player) {
        if (e.group == QStringLiteral("Everything else")) continue;
        if (e.summary.isEmpty()) {
            playerBad++;
            out << "    no summary for " << e.name << Qt::endl;
        }
    }
    check(playerBad == 0,
          QStringLiteral("every curated PlayerBase event has a summary (%1 missing)")
              .arg(playerBad));

    out << "unknown classes" << Qt::endl;
    // An unknown name leaves Catalog::search with no owner filter, so this used
    // to be the whole catalogue rather than nothing.
    for (const QString &name : {QStringLiteral("NotARealClass"),
                                QStringLiteral(""),
                                QStringLiteral("itembase"),
                                QStringLiteral("ItemBase "),
                                QStringLiteral("m12")}) {
        const QVector<EventInfo> none = eventsForClass(cat, name);
        check(none.isEmpty(), QStringLiteral("'%1' yields nothing, got %2")
                                  .arg(name).arg(none.size()));
    }

    Catalog empty;
    check(eventsForClass(empty, QStringLiteral("ItemBase")).isEmpty(),
          QStringLiteral("an unloaded catalogue yields nothing"));

    // ------------------------------------------------------------ task index
    out << "task index" << Qt::endl;
    const Builtins builtins;
    const QVector<IndexGroup> index =
        nodeIndex(cat, builtins, QStringLiteral("ItemBase"));
    check(index.size() >= 10,
          QStringLiteral("the index publishes %1 groups").arg(index.size()));

    int indexRows = 0;
    for (const IndexGroup &g : index) indexRows += g.rows.size();
    out << "       " << index.size() << " groups, " << indexRows << " rows" << Qt::endl;

    // Every curated reference has to land. A dropped one is a row that vanishes
    // from the palette with nothing said, which is exactly how a method that
    // moved between DayZ releases would rot the list.
    QSet<QString> titles;
    for (const IndexGroup &g : index)
        for (const IndexRow &r : g.rows) titles.insert(r.title);
    int unresolved = 0;
    for (const QString &ref : curatedRefs()) {
        // A reference beginning with @ stands for something the table does not
        // spell out: the events row, which is an action rather than a node, and
        // the operator presets, which come from Builtins. Both are checked on
        // their own below.
        if (ref.startsWith(QLatin1Char('@'))) continue;
        const int split = ref.indexOf(QStringLiteral("::"));
        const QString name = split > 0 ? ref.mid(split + 2) : ref;
        const QString title = split > 0 ? name : builtins.def(ref).title;
        if (titles.contains(title)) continue;
        unresolved++;
        out << "    unresolved curated row " << ref << Qt::endl;
    }
    check(unresolved == 0,
          QStringLiteral("every curated row resolves (%1 did not)").arg(unresolved));

    // Nothing is hidden. A builtin the table does not name still has to be in
    // the index, once, under the tail group.
    QStringList indexKeys;
    for (const IndexGroup &g : index)
        for (const IndexRow &r : g.rows) indexKeys << r.key;
    const QSet<QString> keySet(indexKeys.begin(), indexKeys.end());
    check(indexKeys.size() == keySet.size(),
          QStringLiteral("no row appears twice (%1 rows, %2 distinct)")
              .arg(indexKeys.size()).arg(keySet.size()));
    int missingBuiltins = 0;
    for (const NodeDef &def : builtins.all()) {
        if (keySet.contains(def.key)) continue;
        missingBuiltins++;
        out << "    builtin not in the index: " << def.key << Qt::endl;
    }
    check(missingBuiltins == 0,
          QStringLiteral("every builtin is reachable by browsing (%1 are not)")
              .arg(missingBuiltins));
    check(index.last().title == QStringLiteral("Everything else"),
          QStringLiteral("the tail group is last, got '%1'").arg(index.last().title));

    // The way into the events list is the first row of the first group, because
    // a hook is found by the moment it fires and no name search reaches it.
    check(!index.isEmpty() && !index.first().rows.isEmpty()
              && index.first().rows.first().key == nodeindex::BrowseEventsKey,
          QStringLiteral("the events list is the first row offered"));

    // The five topics the corpus counts put at the top. Named by the method a
    // modder would reach for, so a group renamed without its rows still fails.
    for (const QString &want : {QStringLiteral("Set Timer"),
                                QStringLiteral("Call Later"),
                                QStringLiteral("Cast To"),
                                QStringLiteral("IsServer"),
                                QStringLiteral("ConfigGetString"),
                                QStringLiteral("ConfigGetChildName"),
                                QStringLiteral("RegisterNetSyncVariableBool"),
                                QStringLiteral("SetSynchDirty"),
                                QStringLiteral("Call Super")})
        check(titles.contains(want), QStringLiteral("the index offers %1").arg(want));

    // Arithmetic is browsable rather than only searchable. Expanded from the
    // list Builtins publishes, so an operator added there cannot end up in the
    // tail without this failing.
    int operatorRows = 0;
    for (const IndexGroup &g : index)
        if (g.title == QStringLiteral("Maths and comparisons")) operatorRows = g.rows.size();
    check(operatorRows == Builtins::binaryOperators().size(),
          QStringLiteral("every operator has a row of its own (%1 of %2)")
              .arg(operatorRows).arg(Builtins::binaryOperators().size()));

    // Every row says what it does before it is placed, which is the whole point
    // of curating one, and a caution-free vanilla method still has to say
    // something true rather than nothing.
    int docless = 0;
    for (const IndexGroup &g : index) {
        if (g.doc.isEmpty()) {
            docless++;
            out << "    no doc for group " << g.title << Qt::endl;
        }
        for (const IndexRow &r : g.rows) {
            if (!r.doc.isEmpty()) continue;
            docless++;
            out << "    no doc for row " << r.title << Qt::endl;
        }
    }
    check(docless == 0, QStringLiteral("every group and row carries a line (%1 do not)")
                            .arg(docless));

    // Those lines are drawn by Qt, so the house rules apply to them as much as
    // to anything written by hand here.
    int indexBanned = 0;
    for (const IndexGroup &g : index) {
        QStringList text{g.title, g.doc};
        for (const IndexRow &r : g.rows) text << r.title << r.detail << r.doc;
        for (const QString &s : text) {
            for (const QChar c : s) {
                if (c.unicode() < 128) continue;
                indexBanned++;
                out << "    non-ascii in the index: " << s << Qt::endl;
                break;
            }
        }
    }
    check(indexBanned == 0,
          QStringLiteral("the index is plain ascii (%1 are not)").arg(indexBanned));

    // A curated call is a call. An event def has no target pin and no inputs at
    // all, so a row that turned into one would place an entry point where the
    // user asked for a call. That is the shape of the Timer::Run bug.
    int wrongShape = 0;
    for (const IndexGroup &g : index) {
        for (const IndexRow &r : g.rows) {
            if (r.key.startsWith(QStringLiteral("bi."))
                || r.key == nodeindex::BrowseEventsKey)
                continue;
            const MethodSig sig = cat.method(r.key);
            if (!sig.valid) continue; // a global, which has no owner to target
            if (!(sig.flags & flag::Event)) continue;
            wrongShape++;
            out << "    event offered as a call: " << r.title << Qt::endl;
        }
    }
    check(wrongShape == 0,
          QStringLiteral("no curated row is an event (%1 are)").arg(wrongShape));

    // The generator refuses a call whose owner the graph does not descend from,
    // so the index has to refuse it first. Mission is not an Object, and every
    // per-item config read is declared on Object.
    const QVector<IndexGroup> onMission =
        nodeIndex(cat, builtins, QStringLiteral("Mission"));
    int reachable = 0;
    int gameRows = 0;
    for (const IndexGroup &g : onMission)
        for (const IndexRow &r : g.rows) {
            if (r.detail == QStringLiteral("Object")) reachable++;
            if (r.detail == QStringLiteral("CGame")) gameRows++;
        }
    check(reachable == 0,
          QStringLiteral("a Mission graph is offered no Object member (%1 were)")
              .arg(reachable));
    check(gameRows > 0,
          QStringLiteral("a Mission graph keeps the %1 rows reached through GetGame()")
              .arg(gameRows));
    int itemObjectRows = 0;
    for (const IndexGroup &g : index)
        for (const IndexRow &r : g.rows)
            if (r.detail == QStringLiteral("Object")) itemObjectRows++;
    check(itemObjectRows > 0,
          QStringLiteral("an ItemBase graph keeps its %1 Object members")
              .arg(itemObjectRows));

    // A graph with no base class yet proves nothing about what it can call, so
    // the rows stand rather than being withheld on a guess. Same reading the
    // generator takes when it cannot resolve `this`.
    int unknownObjectRows = 0;
    for (const IndexGroup &g : nodeIndex(cat, builtins, QString()))
        for (const IndexRow &r : g.rows)
            if (r.detail == QStringLiteral("Object")) unknownObjectRows++;
    check(unknownObjectRows == itemObjectRows,
          QStringLiteral("a graph with no base class keeps them too (%1 vs %2)")
              .arg(unknownObjectRows).arg(itemObjectRows));

    // Without a catalogue the curated calls cannot resolve, and the index has to
    // degrade to the builtins rather than come back empty or take the app down.
    Catalog unloaded;
    const QVector<IndexGroup> bare =
        nodeIndex(unloaded, builtins, QStringLiteral("ItemBase"));
    int bareRows = 0;
    for (const IndexGroup &g : bare) bareRows += g.rows.size();
    check(bareRows >= builtins.all().size(),
          QStringLiteral("an unloaded catalogue still offers every builtin (%1 rows)")
              .arg(bareRows));

    // The index is filtered by the same reading the catalogue search uses. A
    // plain contains() answered nothing for "config get" while the catalogue
    // answered eleven rows, which reads as half the palette being broken.
    const QStringList configRow{QStringLiteral("ConfigGetString"),
                                QStringLiteral("Object"),
                                QStringLiteral("Read a config value")};
    check(matchesQuery(QStringLiteral("config get"), configRow),
          QStringLiteral("two words find the name that spells them joined"));
    check(matchesQuery(QStringLiteral("configget"), configRow),
          QStringLiteral("the joined spelling still matches"));
    check(matchesQuery(QStringLiteral("config object"), configRow),
          QStringLiteral("a word from the owning class counts"));
    check(matchesQuery(QString(), configRow),
          QStringLiteral("an empty query matches everything"));
    check(!matchesQuery(QStringLiteral("config colour"), configRow),
          QStringLiteral("a term that lands nowhere refuses the row"));
    check(matchesQuery(QStringLiteral("  set   timer  "),
                       {QStringLiteral("Set Timer"), QStringLiteral("after N seconds"),
                        QStringLiteral("Do something later")}),
          QStringLiteral("runs of whitespace split the same as one space"));

    // The queries a five second timer is looked for by. "new timer" is the one
    // that used to answer nothing at all: the node is called Set Timer, and no
    // vanilla declaration carries the word new, so neither half of the palette
    // had anything to say. Asked through rowMatches rather than by hand,
    // because that is what both surfaces call.
    {
        const QVector<IndexGroup> index =
            nodeIndex(cat, builtins, QStringLiteral("ItemBase"));
        const auto answers = [&index](const QString &query, const QString &key) {
            for (const IndexGroup &g : index)
                for (const IndexRow &r : g.rows)
                    if (r.key == key && rowMatches(query, r, g.title)) return true;
            return false;
        };
        for (const QString &q : {QStringLiteral("new timer"), QStringLiteral("set timer"),
                                 QStringLiteral("wait"), QStringLiteral("delay")})
            check(answers(q, QStringLiteral("bi.setTimer")),
                  QStringLiteral("\"%1\" finds Set Timer").arg(q));
        check(answers(QStringLiteral("defer"), QStringLiteral("bi.callLater")),
              QStringLiteral("\"defer\" finds Call Later"));
        // An alias is a way in, not a way to be shown something else: it must
        // not widen a query that already names another node.
        check(!answers(QStringLiteral("branch"), QStringLiteral("bi.setTimer")),
              QStringLiteral("an alias does not catch an unrelated query"));

        // Nothing an alias carries is ever drawn, so it cannot be checked by
        // eye. Both halves are asserted instead: the words are plain ASCII, and
        // the row still draws what the def gave it.
        int badAlias = 0;
        for (const IndexGroup &g : index)
            for (const IndexRow &r : g.rows) {
                if (r.alias.isEmpty()) continue;
                if (r.title.contains(r.alias) || r.detail.contains(r.alias)) badAlias++;
                for (const QChar c : r.alias)
                    if (c.unicode() > 126 || c.isUpper()) badAlias++;
            }
        check(badAlias == 0,
              QStringLiteral("every alias is lower case ascii and stays out of the "
                             "drawn text (%1)").arg(badAlias));
    }

    // An event is an override, so it is only worth offering to a class that
    // inherits the declaration. Typing "oninit" into a MissionServer graph used
    // to answer with seven classes, six of which generate a method the engine
    // never calls. Calls are untouched: a target pin is how a graph reaches
    // somebody else's object on purpose.
    check(eventFitsClass(cat, QStringLiteral("Events"), QStringLiteral("Mission"),
                         QStringLiteral("MissionServer")),
          QStringLiteral("a hook the class inherits is offered"));
    check(!eventFitsClass(cat, QStringLiteral("Events"), QStringLiteral("Backlit"),
                          QStringLiteral("MissionServer")),
          QStringLiteral("a hook from an unrelated class is not"));
    check(eventFitsClass(cat, QStringLiteral("Functions"), QStringLiteral("Backlit"),
                         QStringLiteral("MissionServer")),
          QStringLiteral("a call from an unrelated class still is"));
    check(eventFitsClass(cat, QStringLiteral("Events"), QStringLiteral("Backlit"),
                         QString()),
          QStringLiteral("nothing is withheld when the class is not known yet"));
    check(eventFitsClass(cat, QStringLiteral("Events"), QStringLiteral("Backlit"),
                         QStringLiteral("SomeMod_ThingBase")),
          QStringLiteral("or when the catalogue has never heard of it"));
    check(eventFitsClass(cat, QStringLiteral("Events"), QStringLiteral("EntityAI"),
                         QStringLiteral("ItemBase")),
          QStringLiteral("an inherited hook two classes up is still offered"));

    check(!nodeSummary(cat, builtins, QStringLiteral("bi.branch")).isEmpty(),
          QStringLiteral("a builtin summary reads before the node is placed"));
    check(nodeSummary(cat, builtins, QStringLiteral("nothing")).isEmpty(),
          QStringLiteral("an unknown key summarises to nothing rather than guessing"));

    out << Qt::endl << (failures == 0 ? "all checks passed"
                                      : QStringLiteral("%1 failed").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
