#include "events.h"

#include "catalog.h"

#include <QHash>
#include <QRegularExpression>
#include <algorithm>

namespace {

// Groups in display order. GroupOther is last and holds the alphabetical tail
// plus everything pushed down for being debug-only or deprecated.
enum Group {
    GroupLifecycle,
    GroupAttachments,
    GroupDamage,
    GroupPersistence,
    GroupInteraction,
    GroupFrame,
    GroupOther,
    GroupCount,
};

const char *const kGroupNames[GroupCount] = {
    "Lifecycle",
    "Attachments and cargo",
    "Damage and death",
    "Persistence",
    "Player interaction",
    "Frame and update",
    "Everything else",
};

// Rank is group index times kGroupSpan plus the row's position in kCurated, so
// a curated group keeps the order written below instead of falling back to
// alphabetical. The span has to exceed the table length or a late row in one
// group would outrank an early row in the next.
constexpr int kGroupSpan = 1000;

struct Curated {
    const char *name;
    Group group;
    const char *summary;
};

// Every summary here was checked against P:\scripts rather than written from
// memory, and a side (server, client, both) is only named where the vanilla
// source or its own comment says so. Picking a client-only hook for server
// logic is the classic way to lose an afternoon, and a confident guess about
// the side is worse than no claim at all.
//
// These are fallbacks. A cleaned vanilla doc comment wins whenever there is
// one, which is roughly a quarter of the time.
const Curated kCurated[] = {
    // Lifecycle
    {"EEInit", GroupLifecycle,
     "Runs once the entity has been created, on the server and on the client. "
     "Register net sync variables, read config values and start timers here."},
    {"DeferredInit", GroupLifecycle,
     "Queued by the constructor and run about 34 ms later, so the entity is "
     "finished by the time it fires. Use it when EEInit is still too early."},
    {"EEDelete", GroupLifecycle,
     "Runs immediately before the entity is deleted. Last chance to stop "
     "timers, remove effects and drop references to it."},
    {"AfterStoreLoad", GroupLifecycle,
     "Runs once persistence has restored this entity and everything in its "
     "cargo. Storage is loaded on the server, so read saved state that needs "
     "its children to exist here rather than in OnStoreLoad."},
    {"EEOnCECreate", GroupLifecycle,
     "Runs when the central economy spawns this entity as new, as opposed to "
     "restoring it from storage."},
    {"OnInitEnergy", GroupLifecycle,
     "Runs once the energy manager component on this item is ready. Anything "
     "that reads energy state has to wait for it."},
    {"OnWorkStart", GroupLifecycle,
     "Fires once when an energy managed device starts working. The device "
     "update loop runs on the server and on the client, so both sides see it."},
    {"OnWork", GroupLifecycle,
     "Fires on every device update while the item is working, carrying the "
     "energy consumed since the last one."},
    {"OnWorkStop", GroupLifecycle,
     "Fires once when the device stops being powered, whether it was switched "
     "off or ran out of energy."},
    {"OnSwitchOn", GroupLifecycle,
     "Fires when the device is switched on. The energy manager raises it on "
     "the server and separately on the client, so both sides see it."},
    {"OnSwitchOff", GroupLifecycle,
     "Fires when the device is switched off. Like OnSwitchOn it is raised on "
     "the server and separately on the client."},
    // Player graphs are the other common base class, so the same groups carry
    // the hooks a PlayerBase mod starts from.
    {"Init", GroupLifecycle,
     "Runs from the PlayerBase constructor and builds the managers a player "
     "needs. Call super first, because most of PlayerBase assumes it has run."},
    {"OnPlayerLoaded", GroupLifecycle,
     "Runs when the player connects or respawns and their stats have loaded. "
     "Both sides reach it, and vanilla branches inside it on "
     "IsControlledPlayer for the client half."},
    {"OnReconnect", GroupLifecycle,
     "Runs on the server when a player reconnects to a character that is still "
     "in the world. MissionServer raises it."},

    // Attachments and cargo
    {"EEItemAttached", GroupAttachments,
     "Runs on the parent when a child is attached to one of its slots. The "
     "attached item and the slot name arrive as pins."},
    {"EEItemDetached", GroupAttachments,
     "Runs on the parent when a child is detached from one of its slots."},
    {"OnWasAttached", GroupAttachments,
     "Runs on the child once it has been attached, with the new parent and the "
     "slot id. The parent side of the same moment is EEItemAttached."},
    {"OnWasDetached", GroupAttachments,
     "Runs on the child once it has been detached, with the parent it left and "
     "the slot id it came out of."},
    {"EECargoIn", GroupAttachments,
     "Runs on the container when an item is placed in its cargo. The container "
     "then calls OnMovedInsideCargo on the item itself."},
    {"OnMovedInsideCargo", GroupAttachments,
     "Runs on the item once it has been moved into a container's cargo, with "
     "that container as a pin."},
    {"OnMovedWithinCargo", GroupAttachments,
     "Runs on the item when it is rearranged inside the cargo it is already "
     "in, rather than moved to a different container."},
    {"OnRemovedFromCargo", GroupAttachments,
     "Runs on the item once it has been taken out of a container's cargo."},
    {"OnItemLocationChanged", GroupAttachments,
     "Runs when this entity changes owner, with the old and the new parent as "
     "pins. EEItemLocationChanged calls it, so it covers hands, cargo, "
     "attachment slots and the ground."},
    {"EEItemLocationChanged", GroupAttachments,
     "Runs when this entity's inventory location changes, carrying the full "
     "old and new InventoryLocation. It calls OnItemLocationChanged for you, "
     "so take this one when you need the slot and location type."},
    {"OnItemAttachmentSlotChanged", GroupAttachments,
     "Runs only when this entity moves straight from one attachment slot to "
     "another. A move to or from cargo or hands does not reach it."},
    {"OnInventoryEnter", GroupAttachments,
     "Runs on the item when it enters a player's inventory, with that player "
     "as a pin."},
    {"OnInventoryExit", GroupAttachments,
     "Runs on the item when it leaves a player's inventory, with the player it "
     "left as a pin."},
    {"OnChildItemRemoved", GroupAttachments,
     "Runs on the old owner when an item leaves its hierarchy. The paired "
     "event on the new owner is OnChildItemReceived."},
    {"EEParentedTo", GroupAttachments,
     "Runs when this entity gains a hierarchy parent, which covers being "
     "attached, put into cargo and taken into hands."},
    {"EEParentedFrom", GroupAttachments,
     "Runs when this entity's hierarchy parent changes or goes away."},
    {"ChangeIntoOnAttach", GroupAttachments,
     "Return the classname this item should become when it is attached to the "
     "given slot, or an empty string to leave it alone. Vanilla uses it to "
     "fold an item into its attached form."},
    {"ChangeIntoOnDetach", GroupAttachments,
     "Return the classname this item should become when it is detached, or an "
     "empty string to leave it alone."},
    {"OnAttachmentQuantityChanged", GroupAttachments,
     "Runs on the parent when the quantity of one of its attachments changes."},
    {"EEItemIntoHands", GroupAttachments,
     "Runs on the player when an item is taken into their hands, with that "
     "item as a pin."},
    {"EEItemOutOfHands", GroupAttachments,
     "Runs on the player when an item leaves their hands."},
    {"OnItemInHandsChanged", GroupAttachments,
     "Runs on the player after whatever is in their hands changed, without "
     "saying what it is. Man declares it empty."},

    // Damage and death
    {"EEHitBy", GroupDamage,
     "Runs when this entity takes damage. The pins carry the damage result, "
     "the entity that caused it, the hit component, the damage zone and the "
     "ammo type. Vanilla pairs it with EEHitByRemote for the shooter's "
     "client."},
    {"EEHitByRemote", GroupDamage,
     "Runs only on the client that caused the hit, which is what vanilla's own "
     "comment on EntityAI says. Good for local feedback, never for applying "
     "damage."},
    {"EEHealthLevelChanged", GroupDamage,
     "Runs when health crosses a level boundary, for example worn to damaged "
     "or damaged to ruined. The pins carry the old level, the new level and "
     "the damage zone; an empty zone means global health."},
    {"OnDamageDestroyed", GroupDamage,
     "Runs when a damage zone reaches ruined. EEHealthLevelChanged calls it "
     "when the zone is the entity's global health."},
    {"EEKilled", GroupDamage,
     "Runs on the server when the entity is killed, with the killer as a pin."},
    {"ConvertNonlethalDamage", GroupDamage,
     "Turns shock damage into the health damage that gets applied, and returns "
     "the amount. Override it to change how hard a nonlethal hit lands."},
    {"EEOnDamageCalculated", GroupDamage,
     "Runs after the engine has worked out the damage but before it is "
     "applied, so it can still be inspected."},
    {"OnAttachmentRuined", GroupDamage,
     "Runs on the parent when one of its attachments is ruined. Vanilla notes "
     "that it fires on the server and the client."},
    {"ReplaceOnDeath", GroupDamage,
     "Return true to have the entity swapped for the class GetDeadItemName "
     "returns when it is killed. EEKilled schedules the swap, so this is asked "
     "on the server."},
    {"KeepHealthOnReplace", GroupDamage,
     "Return true so the replacement created by ReplaceOnDeath inherits this "
     "entity's health instead of spawning at full."},
    {"Explode", GroupDamage,
     "Detonates this entity using the ammoType named in its config."},

    // Persistence
    {"OnStoreSave", GroupPersistence,
     "Runs on the server when the entity is written to storage. Write your "
     "values in the same order OnStoreLoad reads them, and call super first."},
    {"OnStoreLoad", GroupPersistence,
     "Runs on the server when the entity is read back from storage. Read in "
     "the order OnStoreSave wrote, and return false when a read fails so the "
     "engine knows the record is bad."},
    {"EEOnAfterLoad", GroupPersistence,
     "Runs after a connected system, for example a base building piece, has "
     "been restored. AfterStoreLoad is the plainer hook for one entity."},
    {"WriteVarsToCTX", GroupPersistence,
     "Writes this item's own variables into the storage context. Call super "
     "first so the inherited values still land."},
    {"ReadVarsFromCTX", GroupPersistence,
     "Reads this item's own variables back from the storage context, matching "
     "what WriteVarsToCTX put in."},

    // Player interaction
    {"SetActions", GroupInteraction,
     "Registers the user actions this item offers, one AddAction per action "
     "class. Call super first, because dropping it drops every inherited "
     "action. Vanilla caches the result per item type, so it runs once for the "
     "first instance of the type."},
    {"GetActions", GroupInteraction,
     "Returns the actions registered for one input type. Vanilla fills it from "
     "the cache SetActions built, so override SetActions unless you need the "
     "lookup itself."},
    {"OnApply", GroupInteraction,
     "Runs on the server when an apply style action finishes, with the player "
     "it was applied to. Vanilla uses it for the injectable medical items."},
    {"OnCombine", GroupInteraction,
     "Runs after this item has taken quantity from another stack of the same "
     "type, with the other item as a pin."},
    {"OnRightClick", GroupInteraction,
     "Runs on the client when the player right clicks this item in the "
     "inventory screen."},
    {"OnUseFromInventory", GroupInteraction,
     "Runs when the item is used straight from the inventory screen, and "
     "returns whether that was handled. ItemBook is vanilla's only user."},
    {"OnActivatedByItem", GroupInteraction,
     "Runs when another item activates this one, which is how remote "
     "detonators, timers and tripwires reach their charge."},
    {"Open", GroupInteraction,
     "Vanilla declares this empty for the open user action. Give it a body and "
     "pair it with IsOpen so the action knows the current state."},
    {"Close", GroupInteraction,
     "Vanilla declares this empty for the close user action, the counterpart "
     "to Open."},
    {"OnPlacementStarted", GroupInteraction,
     "Runs when the player starts positioning this item. Vanilla raises it "
     "from both the server and the local placing path."},
    {"OnHologramBeingPlaced", GroupInteraction,
     "Runs repeatedly on the projection while the player is still moving the "
     "placement hologram around."},
    {"OnPlacementComplete", GroupInteraction,
     "Runs on the server when a deploy action finishes, with the player and "
     "the final position and orientation. Vanilla fires it from "
     "ActionDeployBase before the item has been moved to that position."},
    {"OnEndPlacement", GroupInteraction,
     "Runs on the server once placing has finished and the item is no longer "
     "being placed."},
    {"OnPlacementCancelled", GroupInteraction,
     "Runs when the player cancels placing this item, with the player as a "
     "pin."},
    {"OnStanceChange", GroupInteraction,
     "Runs on the player when their stance changes, with the previous and the "
     "new stance. CanChangeStance is the guard that decides whether it may."},
    {"OnJumpStart", GroupInteraction,
     "Runs when the player's jump begins, raised by the jump and climb "
     "handler once the stamina check has passed."},
    {"OnJumpEnd", GroupInteraction,
     "Runs when the player's jump finishes, with the land type."},
    {"OnLand", GroupInteraction,
     "Runs when the player lands, with the current command id and the fall "
     "damage data. Return true once the landing has been handled."},
    {"OnRollStart", GroupInteraction,
     "Runs when the player starts a roll, with the direction it goes."},
    {"OnRollFinish", GroupInteraction,
     "Runs when the player's roll ends."},
    // Human declares one empty start and finish stub per movement command and
    // the engine calls them. They only differ by which command they track, so
    // the summaries do too.
    {"OnCommandMoveStart", GroupInteraction,
     "Runs when the player's move command starts. Human declares it empty for "
     "the engine to call."},
    {"OnCommandFallStart", GroupInteraction,
     "Runs when the player starts falling."},
    {"OnCommandFallFinish", GroupInteraction,
     "Runs when the player's fall command ends."},
    {"OnCommandClimbStart", GroupInteraction,
     "Runs when the player starts climbing or vaulting."},
    {"OnCommandClimbFinish", GroupInteraction,
     "Runs when the player's climb ends."},
    {"OnCommandSwimStart", GroupInteraction,
     "Runs when the player starts swimming."},
    {"OnCommandSwimFinish", GroupInteraction,
     "Runs when the player stops swimming."},
    {"OnCommandVehicleStart", GroupInteraction,
     "Runs when the player enters the vehicle command, which covers driving "
     "and riding."},
    {"OnCommandVehicleFinish", GroupInteraction,
     "Runs when the player leaves the vehicle command."},
    {"OnCommandDeathStart", GroupInteraction,
     "Runs when the player's death command starts, which is the animation "
     "side of dying. EEKilled is the gameplay side."},
    {"OnVehicleSeatDriverEnter", GroupInteraction,
     "Runs when the player takes the driver's seat."},
    {"OnVehicleSeatDriverLeft", GroupInteraction,
     "Runs when the player leaves the driver's seat."},

    // Frame and update
    {"EOnInit", GroupFrame,
     "EntityEvent.INIT. Runs after the world and every entity in it has been "
     "created. Needs SetEventMask(EntityEvent.INIT) to fire at all."},
    {"EOnFrame", GroupFrame,
     "EntityEvent.FRAME. Runs every rendered frame with the time step. It only "
     "fires after SetEventMask(EntityEvent.FRAME), which is the usual reason a "
     "graph hung off it does nothing."},
    {"EOnPostFrame", GroupFrame,
     "EntityEvent.POSTFRAME. Runs at the end of a frame, or whenever the "
     "entity moved during it. Needs the POSTFRAME event mask."},
    {"EOnSimulate", GroupFrame,
     "EntityEvent.SIMULATE. Runs on the physics step rather than the render "
     "frame. Needs the SIMULATE event mask."},
    {"EOnPostSimulate", GroupFrame,
     "EntityEvent.POSTSIMULATE. Runs after the physics step, once positions "
     "have settled. Needs the POSTSIMULATE event mask."},
    {"EOnContact", GroupFrame,
     "EntityEvent.CONTACT. Runs on a physics contact, with the other entity "
     "and the contact data. Needs the CONTACT event mask."},
    {"EOnTouch", GroupFrame,
     "EntityEvent.TOUCH. Runs when another entity touches this one. Needs the "
     "TOUCH event mask."},
    {"EOnEnter", GroupFrame,
     "EntityEvent.ENTER. Runs when an entity enters this trigger. Needs the "
     "ENTER event mask and the TRIGGER entity flag."},
    {"EOnLeave", GroupFrame,
     "EntityEvent.LEAVE. Runs when an entity leaves this trigger. Needs the "
     "LEAVE event mask and the TRIGGER entity flag."},
    {"OnCEUpdate", GroupFrame,
     "Runs when the central economy sweeps every entity. Call super first and "
     "use m_ElapsedSinceLastUpdate for anything time based, because the "
     "interval is not fixed."},
    {"OnTick", GroupFrame,
     "Runs on the server once per player update from MissionServer. It works "
     "out the time step and hands it to OnScheduledTick, where the modifiers "
     "and notifiers run."},

    // Not in a curated group, but worth a written summary anyway: these are the
    // networking hooks, and the seven groups above have no home for them.
    {"OnRPC", GroupOther,
     "Receives a remote procedure call sent to this entity, with the sender "
     "identity, the rpc id and the params context. Call super first and start "
     "your own ids well above the vanilla ERPCs range."},
    {"OnVariablesSynchronized", GroupOther,
     "Runs on the client after synchronised variables arrive from the server. "
     "It is the read side of RegisterNetSyncVariable and SetSynchDirty."},
    {"RPC", GroupOther,
     "Sends a remote procedure call from this entity. A shortcut for "
     "CGame.RPC; OnRPC is the receiving end."},
    {"OnAction", GroupOther,
     "Handles a selectable action picked from this entity's debug menu. Match "
     "the action id against EActions and return true once handled."},
};

static_assert(sizeof(kCurated) / sizeof(kCurated[0]) < kGroupSpan,
              "kGroupSpan must exceed the curated table length, or a late row "
              "in one group outranks an early row in the next");

// Doxygen and banner text the extractor glued onto the front or back of a doc.
// Everything from the first hit onward is cut: what follows is markup, a
// parameter list, or in one case an instruction to whoever maintains vanilla.
const char *const kDocMarkers[] = {
    "\u00b7",        // vanilla's own separator between prose and @param blocks
    "DO NOT INSERT", // the banner sitting above IEntity's EOn* declarations
    "@fn", "@param", "@ctx", "@prev", "@see", "@code", "@endcode", "@note",
    "@warning", "@brief", "@return",
    "\\name", "\\param", "\\return", "\\code", "\\note", "\\warning", "\\brief",
    "[note]", "[warning]", "[hndfsm]", "[i]",
    "TODO",
};

bool isRulerChar(QChar c)
{
    return c == QLatin1Char('-') || c == QLatin1Char('=') || c == QLatin1Char('*')
           || c == QLatin1Char('_') || c == QLatin1Char('/') || c.isSpace();
}

// True for a word vanilla would only write inside a section banner: two or more
// letters, none of them lower case. "A" and "I" are excluded so an ordinary
// sentence starting with one is left alone.
bool isBannerWord(const QString &w)
{
    int letters = 0;
    for (const QChar c : w) {
        if (c.isLower()) return false;
        if (c.isLetter()) ++letters;
    }
    return letters >= 2;
}

// Vanilla writes section headings in capitals above a run of declarations, and
// the doc extractor hands the heading to whichever declaration came next or
// last. Open's whole doc comment is "OPEN/CLOSE USER ACTIONS Implementations
// only"; OnPlacementStarted's is "End of fire distribution ^ ADVANCED PLACEMENT
// EVENTS". A heading only ever lands at one end of what got glued together, so
// capitals in the middle of a sentence are emphasis and stay: OnEnergyConsumed
// really does document itself with "ALWAYS CALL super.OnEnergyConsumed()".
bool hasBannerEdge(const QString &text)
{
    const QStringList w = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (w.size() < 2) return false;
    if (isBannerWord(w.at(0)) && isBannerWord(w.at(1))) return true;
    return isBannerWord(w.at(w.size() - 1)) && isBannerWord(w.at(w.size() - 2));
}

// A vanilla doc comment reduced to one plain sentence, or "" when nothing
// usable survives. The result reaches the UI, so the characters this project
// bans (middot, ellipsis, em dash, curly quotes) have to go: they arrive here
// from vanilla, not from anything written in this repo.
QString cleanDoc(const QString &raw)
{
    QString t = raw.trimmed();

    // The extractor also picks up commented-out statements sitting above a real
    // comment, which is how EEHitByRemote ends up documented with a Print call.
    // Quotes plus a call ending mark the tail of code rather than prose.
    if (t.contains(QLatin1Char('"'))) {
        const int endOfCode = t.lastIndexOf(QLatin1String(");"));
        if (endOfCode >= 0) t = t.mid(endOfCode + 2);
    }

    int cut = t.size();
    for (const char *marker : kDocMarkers) {
        const int at = t.indexOf(QString::fromUtf8(marker));
        if (at >= 0 && at < cut) cut = at;
    }
    t.truncate(cut);

    for (const QChar bad : {QChar(0x00b7), QChar(0x2026), QChar(0x2014),
                            QChar(0x2013), QChar(0x2018), QChar(0x2019),
                            QChar(0x201c), QChar(0x201d)})
        t.replace(bad, QLatin1Char(' '));

    // Rulers appear at the front, between a heading and its text, and at the
    // end, so they are collapsed wherever they sit rather than trimmed off one
    // side. Three is the shortest run vanilla draws, and it keeps "-1" intact.
    static const QRegularExpression ruler(QStringLiteral("[-=*_]{3,}"));
    t.replace(ruler, QStringLiteral(" "));
    t = t.simplified();

    int lead = 0;
    while (lead < t.size() && isRulerChar(t.at(lead))) ++lead;
    t = t.mid(lead);
    while (!t.isEmpty() && isRulerChar(t.back())) t.chop(1);
    while (!t.isEmpty()
           && (t.back() == QLatin1Char(',') || t.back() == QLatin1Char(';')
               || t.back() == QLatin1Char(':')))
        t.chop(1);
    t = t.simplified();

    // A doc carrying a section heading was glued together from comments that
    // belong to the neighbouring declarations too, so none of it can be trusted
    // to describe this one.
    if (hasBannerEdge(t)) return {};

    // Two words is a label, not a summary. "server-side" and "Implementations
    // only" are real doc comments in the tree, and a written fallback beats
    // either of them.
    if (t.size() < 16) return {};
    if (t.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() < 3) return {};
    return t;
}

const QHash<QString, int> &curatedIndex()
{
    static const QHash<QString, int> map = [] {
        QHash<QString, int> out;
        const int n = int(sizeof(kCurated) / sizeof(kCurated[0]));
        for (int i = 0; i < n; ++i) out.insert(QString::fromUtf8(kCurated[i].name), i);
        return out;
    }();
    return map;
}

// The catalogue prefixes an event's search title with "Event ".
QString bareName(const QString &title)
{
    static const QString prefix = QStringLiteral("Event ");
    return title.startsWith(prefix) ? title.mid(prefix.size()) : title;
}

} // namespace

QStringList eventGroupOrder()
{
    QStringList out;
    out.reserve(GroupCount);
    for (int i = 0; i < GroupCount; ++i) out << QString::fromUtf8(kGroupNames[i]);
    return out;
}

QVector<EventInfo> eventsForClass(const Catalog &cat, const QString &className)
{
    // Catalog::search only filters by owner when the class resolves to an
    // ancestor chain. An unknown name leaves that filter empty and the empty
    // query then matches every event in the catalogue, so the class has to be
    // checked here rather than relying on the search to come back empty.
    if (!cat.isLoaded() || className.isEmpty() || cat.classId(className) < 0)
        return {};

    SearchOptions opts;
    opts.category = QStringLiteral("Events");
    opts.ofClass = className;
    // Well past the 289 PlayerBase declares, because search truncates to the
    // limit and a truncated list would drop entries without saying so.
    opts.limit = 8192;
    const QVector<SearchHit> hits = cat.search(QString(), opts);

    const QHash<QString, int> &curated = curatedIndex();

    QVector<EventInfo> out;
    out.reserve(hits.size());
    for (const SearchHit &hit : hits) {
        EventInfo info;
        info.key = hit.key;
        info.name = bareName(hit.title);
        info.owner = hit.subtitle;
        info.signature = hit.sig;

        const QString raw = cat.doc(hit.key);
        // Matched in capitals on purpose. EntityAI::SetTemperature is
        // documented "not really deprecated, but missing context info", and a
        // case-insensitive match would bury a live event on the strength of a
        // sentence saying the opposite.
        info.deprecated = raw.contains(QStringLiteral("DEPRECATED"));
        // A DIAG_DEVELOPER or DEVELOPER guard means the declaration is not
        // compiled into a retail server, which puts it in the same bucket as
        // the Debug* entries.
        info.debugOnly = info.name.contains(QStringLiteral("Debug"))
                         || hit.guards.contains(QStringLiteral("DEVELOPER"));

        const int row = curated.value(info.name, -1);
        Group group = row >= 0 ? kCurated[row].group : GroupOther;
        info.summary = cleanDoc(raw);
        if (info.summary.isEmpty() && row >= 0)
            info.summary = QString::fromUtf8(kCurated[row].summary);

        if (info.deprecated || info.debugOnly) {
            // Held back from the curated groups even when the name is in the
            // table, so "at the bottom" is true of the whole list and not just
            // of one group. The deprecated ConvertNonlethalDamage overload
            // lands here while the live one keeps its place.
            group = GroupOther;
            info.rank = GroupOther * kGroupSpan + (info.deprecated ? 200 : 100);
        } else if (row >= 0 && group != GroupOther) {
            info.rank = group * kGroupSpan + row;
        } else {
            info.rank = GroupOther * kGroupSpan;
        }
        info.group = QString::fromUtf8(kGroupNames[group]);
        out.append(info);
    }

    std::sort(out.begin(), out.end(), [](const EventInfo &a, const EventInfo &b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        const int byName = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        if (byName != 0) return byName < 0;
        // Overloads share a name, so the signature and then the key keep the
        // order stable across runs.
        if (a.signature != b.signature) return a.signature < b.signature;
        return a.key < b.key;
    });
    return out;
}
