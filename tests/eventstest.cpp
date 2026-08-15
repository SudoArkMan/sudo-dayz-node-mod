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
    check(item.size() == 227,
          QStringLiteral("ItemBase yields 227 events, got %1").arg(item.size()));
    check(item.size() == rawCount,
          QStringLiteral("nothing dropped: %1 ranked vs %2 in the catalogue")
              .arg(item.size()).arg(rawCount));

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
    check(hasName(item, QStringLiteral("DebugBBoxDraw")),
          QStringLiteral("DebugBBoxDraw is still reachable"));
    check(eeInit >= 0 && firstDebug > eeInit,
          QStringLiteral("EEInit (%1) sorts above the first debug entry (%2)")
              .arg(eeInit).arg(firstDebug));
    check(firstDeprecated > lastLive,
          QStringLiteral("deprecated entries sort last: first at %1, last live at %2")
              .arg(firstDeprecated).arg(lastLive));
    check(firstDeprecated > firstDebug,
          QStringLiteral("deprecated sorts below debug (%1 vs %2)")
              .arg(firstDeprecated).arg(firstDebug));

    // The catalogue documents this one "not really deprecated, but missing
    // context info". A case-insensitive match on the word would bury it.
    const int setTemp = indexOfName(item, QStringLiteral("SetTemperature"));
    check(setTemp >= 0 && !item.at(setTemp).deprecated,
          QStringLiteral("SetTemperature is not read as deprecated"));

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
