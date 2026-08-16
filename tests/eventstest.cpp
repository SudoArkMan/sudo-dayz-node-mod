// Headless check of the event ranking: the class filter is exact, the curated
// rows resolve to real catalogue entries, and the entries a modder should not
// reach for first are at the bottom rather than missing.
//
//   cmake --build . --target eventstest && ./tests/eventstest ../resources
#include "catalog.h"
#include "events.h"

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

    out << Qt::endl << (failures == 0 ? "all checks passed"
                                      : QStringLiteral("%1 failed").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
