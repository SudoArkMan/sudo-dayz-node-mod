#include "nodeindex.h"

#include "builtins.h"
#include "catalog.h"

#include <QRegularExpression>
#include <QSet>

namespace nodeindex {
const QLatin1String BrowseEventsKey("add.event");
} // namespace nodeindex

namespace {

// A curated reference, written the way a reader would say it rather than as a
// catalogue key. Keys are indices into a file that is regenerated whenever the
// DayZ scripts move, so a table of them would go quietly wrong; a name plus its
// owner either resolves or does not, and the test says which.
//
// `label` overrides the title where the declaration's own name does not say
// what the row is for. `detail` overrides the dim right-hand text, which is
// otherwise the class that declares it or the builtin's own subtitle. Both
// empty keep what the def carries.
//
// The right-hand column is the narrowest thing in the palette dock, so a
// subtitle that only differs from its neighbour's past the elision is worse
// than no subtitle: "run something after N seconds" and "run something after N
// ms" both draw as "run something after N ..." in a dock 300 pixels wide, which
// is the one distinction between those two nodes.
//
// `alias` is search only and is never drawn. A row is otherwise reachable only
// by the words already on it, and the words a modder types are not always
// those: "new timer" is how you ask for the node called Set Timer. Rows that
// need none leave it out, which C++ fills in as a null pointer.
struct Curated {
    const char *ref;
    const char *label;
    const char *detail;
    const char *alias;
};

struct Group {
    const char *title;
    const char *doc;
    const Curated *rows;
    int count;
};

// ---------------------------------------------------------------- the table
//
// Counts in the group docs are from EXPANSION.md, recounted there against the
// unpacked tree rather than inherited. They are in the text because they are
// the reason the group sits where it does, and a modder deciding whether this
// is the normal way to do something is asking exactly that question.

const Curated kHooks[] = {
    // Short because the palette dock is narrow and this is its first row. The
    // class it browses is in the second column beside it, so the label does not
    // have to carry it as well.
    {"@events", "Browse events...", ""},
    {"bi.begin", "", ""},
    {"bi.end", "", ""},
};

const Curated kLater[] = {
    // The unit is the whole difference between these two, and the subtitles
    // they carry only differ in their last word.
    //
    // The aliases are the words the five second timer was searched for and not
    // found by. A Timer is made with this node and with nothing else now, so
    // "new timer" has to land here rather than on New Object, which is the
    // route that could not set its class.
    {"bi.setTimer", "", "after N seconds", "new create wait delay schedule repeat"},
    {"bi.callLater", "", "after N milliseconds", "new defer wait delay next frame queue"},
    {"bi.stopTimer", "", "", "cancel end"},
    {"bi.cancelCallLater", "", "", "remove stop"},
};

const Curated kFlow[] = {
    {"bi.branch", "", ""},
    {"bi.forEach", "", ""},
    {"bi.forLoop", "", ""},
    {"bi.while", "", ""},
    {"bi.sequence", "", ""},
    {"bi.super", "", ""},
    {"bi.return", "", ""},
};

const Curated kSides[] = {
    {"bi.serverOnly", "", ""},
    {"CGame::IsServer", "", ""},
    {"CGame::IsClient", "", ""},
    {"CGame::IsMultiplayer", "", ""},
    {"CGame::IsDedicatedServer", "", ""},
};

const Curated kTypes[] = {
    {"bi.cast", "", ""},
    {"bi.self", "", ""},
    {"bi.new", "", ""},
    {"bi.spawn", "", ""},
};

const Curated kConfig[] = {
    {"Object::ConfigGetString", "", ""},
    {"Object::ConfigGetInt", "", ""},
    {"Object::ConfigGetFloat", "", ""},
    {"Object::ConfigGetBool", "", ""},
    {"Object::ConfigGetTextArray", "", ""},
    {"Object::ConfigIsExisting", "", ""},
    {"CGame::ConfigGetText", "", ""},
    {"CGame::ConfigIsExisting", "", ""},
    {"CGame::ConfigGetChildrenCount", "", ""},
    {"CGame::ConfigGetChildName", "", ""},
};

const Curated kNetwork[] = {
    {"EntityAI::RegisterNetSyncVariableBool", "", ""},
    {"EntityAI::RegisterNetSyncVariableInt", "", ""},
    {"EntityAI::RegisterNetSyncVariableFloat", "", ""},
    {"EntityAI::SetSynchDirty", "", ""},
    {"Object::RPCSingleParam", "", ""},
    {"CGame::RPCSingleParam", "", ""},
    {"CGame::RPC", "", ""},
};

const Curated kValues[] = {
    {"bi.op", "", ""},
    {"bi.not", "", ""},
    {"bi.select", "", ""},
    {"bi.literal", "", ""},
    {"bi.litClass", "", ""},
};

// Expanded from Builtins::binaryOperators() rather than written out, so an
// operator added there turns up here instead of in the tail.
const Curated kMaths[] = {
    {"@operators", "", ""},
};

const Curated kMembers[] = {
    {"bi.setMember", "", ""},
    {"bi.setElement", "", ""},
};

const Curated kLogging[] = {
    {"bi.print", "", ""},
    {"global::PrintToRPT", "", ""},
    {"global::Error", "", ""},
};

const Curated kEscape[] = {
    {"bi.raw", "", ""},
    {"bi.rawExpr", "", ""},
    {"bi.comment", "", ""},
};

#define GROUP(title, doc, rows) \
    { title, doc, rows, int(sizeof(rows) / sizeof(rows[0])) }

const Group kGroups[] = {
    GROUP("Run this when something happens",
          "A mod is a set of hooks. Pick the moment first, then say what "
          "happens. Expansion carries 6,101 overrides against 731 reopened "
          "classes, so this is where nearly every graph starts.",
          kHooks),
    GROUP("Do something later",
          "529 sites in Expansion, and it is what you reach for when a value "
          "is not ready yet. A Timer stops itself when the item is deleted; a "
          "deferred call does not, and forgetting to cancel one is a shipped "
          "vanilla bug.",
          kLater),
    GROUP("Decide what runs",
          "Wire the condition, not the text. Note Call Super: inside a modded "
          "class Expansion calls super in about 80 percent of its overrides, "
          "against roughly 29 percent elsewhere, and leaving it out is how a "
          "mod breaks every other mod on the same class.",
          kFlow),
    GROUP("Server, client, or both",
          "412 runtime checks against 269 compile-time guards, and they are "
          "not the same thing: one branches, the other decides whether the "
          "code exists at all. There is no CLIENT define in DayZ; client-only "
          "code is written as the absence of SERVER.",
          kSides),
    GROUP("Get the type you need",
          "1,154 `Class.CastTo` calls, the single most repeated line in the "
          "corpus. Cast To has a success pin and a failed pin because the "
          "cast really can fail, and a graph that ignores that is the null "
          "pointer you get at 3AM.",
          kTypes),
    GROUP("Read a config value",
          "398 sites in 89 files. This is how a mod parameterises behaviour "
          "per class without writing a script subclass, and how CfgMods "
          "becomes a registry other mods can publish into. Nested arrays in "
          "config.cpp cannot be read from script at all.",
          kConfig),
    GROUP("Tell the other side",
          "314 RPC registrations and 185 synced variables. A synced variable "
          "only reaches clients after SetSynchDirty, and an RPC id that "
          "collides with another mod fails silently rather than loudly.",
          kNetwork),
    GROUP("Work out a value",
          "Pure nodes: no exec pins, they evaluate where they are used. The "
          "Enforce compiler mishandles `bool x = a && b;`, so the generator "
          "spills every intermediate into its own named local, which a wire "
          "already implies.",
          kValues),
    GROUP("Maths and comparisons",
          "One node per operator, so placing a subtraction does not mean "
          "placing an Operator and then finding where its symbol is set. Each "
          "one names the value it yields, and a comparison always yields a "
          "bool whatever it is given.",
          kMaths),
    GROUP("Members and arrays",
          "Writing into something that already exists. Declare the member in "
          "the Variable Manager first; its Get and Set nodes come from there, "
          "because they only exist in this graph.",
          kMembers),
    GROUP("See what it is doing",
          "Print truncates at 1026 characters on the server and 240 on the "
          "client, which are different undocumented limits. Long output "
          "belongs in PrintToRPT.",
          kLogging),
    GROUP("When a node is the wrong shape",
          "The way out. Raw Enforce keeps a statement as text and still "
          "generates, and the importer refuses rather than half-recognising, "
          "so text here is a decision and not a failure.",
          kEscape),
};

#undef GROUP

constexpr int kGroupCount = int(sizeof(kGroups) / sizeof(kGroups[0]));

// The heading the leftovers go under. Named the same way as the events list's
// tail, because it is the same promise: ordering something badly is recoverable,
// dropping it is not.
const char *const kRestTitle = "Everything else";
const char *const kRestDoc =
    "Builtin nodes the groups above do not name. Nothing is left out of the "
    "palette, so a node added to the tool turns up here until it earns a place "
    "of its own.";

// ------------------------------------------------------------- resolution

// A builtin's doc is a summary, then its effects, then any cautions, joined by
// blank lines. A footer has room for the summary; the cautions are pulled out
// separately so they land in the tooltip instead of being truncated away.
QString builtinSummary(const NodeDef &def)
{
    const int para = def.doc.indexOf(QLatin1String("\n\n"));
    return (para > 0 ? def.doc.left(para) : def.doc).simplified();
}

QStringList builtinCautions(const NodeDef &def)
{
    QStringList out;
    static const QLatin1String prefix("Caution: ");
    for (const QString &part : def.doc.split(QLatin1String("\n\n"), Qt::SkipEmptyParts))
        if (part.startsWith(prefix)) out << part.mid(prefix.size()).simplified();
    return out;
}

// A doc comment reduced to one line for a footer, or nothing when what survives
// is not a sentence. The inspector shows the whole thing; this is a trim for a
// single row of chrome, not a second cleaner. Returning nothing matters: the
// caller then writes the declaration out instead, and a footer reading
// "- - - - - - - -" is what the alternative looked like on SceneObject.
QString oneLine(const QString &text)
{
    QString t = text.simplified();
    // Doxygen tails: the extractor glues parameter blocks onto the prose, and a
    // footer has room for the prose only.
    for (const QLatin1String marker :
         {QLatin1String("@param"), QLatin1String("@return"), QLatin1String("@note"),
          QLatin1String("@warning"), QLatin1String("\\param"), QLatin1String("\\return"),
          QLatin1String("\\note")}) {
        const int at = t.indexOf(marker);
        if (at > 0) t.truncate(at);
    }
    // Vanilla draws rulers above a run of declarations and the extractor hands
    // them to whichever declaration came next. Three is the shortest run it
    // draws, and it leaves "-1" alone.
    static const QRegularExpression ruler(QStringLiteral("[-=*_]{3,}"));
    t.replace(ruler, QStringLiteral(" "));
    t = t.simplified();

    // Two words is a label, not a summary, and a line with no letter in it is
    // whatever was left of a banner.
    bool hasLetter = false;
    for (const QChar c : t)
        if (c.isLetter()) { hasLetter = true; break; }
    if (!hasLetter) return {};
    if (t.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() < 3) return {};

    if (t.size() <= 150) return t;

    // Cut on a sentence when there is one inside the budget, and on a word when
    // there is not, so the line never ends mid-token.
    const int stop = t.lastIndexOf(QLatin1String(". "), 150);
    if (stop > 40) return t.left(stop + 1);
    const int space = t.lastIndexOf(QLatin1Char(' '), 150);
    return t.left(space > 40 ? space : 150) + QStringLiteral("...");
}

// The compact "(int, string) : bool" the search rows use, rebuilt from a
// signature so a row with no doc still says what it takes.
QString signatureOf(const MethodSig &sig)
{
    QStringList args;
    for (const MethodSig::Param &p : sig.params) {
        const QString dir = p.dir == 1 ? QStringLiteral("out ")
                                       : p.dir == 2 ? QStringLiteral("inout ")
                                                    : QString();
        args << dir + p.type;
    }
    QString out = QStringLiteral("(%1)").arg(args.join(QStringLiteral(", ")));
    if (!sig.ret.isEmpty() && sig.ret != QLatin1String("void"))
        out += QStringLiteral(" : %1").arg(sig.ret);
    return out;
}

MethodSig sigFor(const Catalog &cat, const QString &key)
{
    const MethodSig m = cat.method(key);
    if (m.valid) return m;
    return cat.globalFn(key);
}

// The catalogue key for one declaration, named by the class that declares it.
// Search over the whole catalogue rather than with an ofClass filter: the owner
// is being asserted here, and filtering by the calling class would answer with
// whichever ancestor happened to rank first.
//
// An overloaded name resolves to the first the ranking returns. Nothing in the
// table is overloaded on its declaring class today, and a curated row is a
// starting point rather than the only way to reach a signature: search still
// lists all of them, which is what the signature column is there to tell apart.
QString keyFor(const Catalog &cat, const QString &owner, const QString &name)
{
    SearchOptions opts;
    // Well past what a single name can rank up; search sorts before it
    // truncates, so an exact match is never the entry that falls off the end.
    opts.limit = 256;
    for (const SearchHit &hit : cat.search(name, opts)) {
        if (hit.subtitle != owner) continue;
        const MethodSig sig = sigFor(cat, hit.key);
        if (!sig.valid || sig.name != name) continue;
        // An event has no target pin and no input pins at all, so a row that
        // became one would place an entry point where a call was meant. Dropped
        // rather than shown wrong; curatedRefs() plus the test is what stops it
        // happening quietly.
        if (sig.flags & flag::Event) continue;
        return hit.key;
    }
    return {};
}

// Whether a graph compiling into `selfClass` could call this declaration at
// all. The same three answers callTarget() gives in the generator, in the same
// order, so the index cannot offer a row the generator would then refuse.
bool reachableFrom(const Catalog &cat, const MethodSig &sig, const QString &selfClass)
{
    if (!cat.accessAllowed(sig.owner, sig.flags, selfClass)) return false;
    if (sig.flags & flag::Static) return true;
    // The game singleton is reachable from anywhere, through GetGame().
    if (sig.owner == QLatin1String("CGame")) return true;
    // A global function has no receiver to find.
    if (sig.owner.isEmpty()) return true;
    // Nothing can be proved when the base class is another script in this
    // project or the class is its own root, so the row stands.
    if (selfClass.isEmpty() || cat.classId(selfClass) < 0) return true;
    return cat.isA(selfClass, sig.owner);
}

bool isBuiltinRef(const QString &ref)
{
    return ref.startsWith(QLatin1String("bi."));
}

// One builtin as a row, or an invalid row when the id is not registered.
IndexRow builtinRow(const Builtins &builtins, const QString &key, const QString &label,
                    const QString &detail, const QString &alias = QString())
{
    IndexRow out;
    const NodeDef def = builtins.def(key);
    if (!def.valid) return out;
    out.key = key;
    out.title = label.isEmpty() ? def.title : label;
    out.detail = detail.isEmpty() ? def.subtitle : detail;
    out.doc = oneLine(builtinSummary(def));
    out.alias = alias;
    return out;
}

// Fills in one row, or leaves `key` empty when the reference does not resolve.
IndexRow resolve(const Catalog &cat, const Builtins &builtins, const Curated &row,
                 const QString &selfClass)
{
    IndexRow out;
    const QString ref = QString::fromUtf8(row.ref);
    const QString label = QString::fromUtf8(row.label);
    const QString detail = QString::fromUtf8(row.detail);
    // Rows that need no alias leave the field off the initialiser entirely.
    const QString alias = row.alias ? QString::fromUtf8(row.alias) : QString();

    if (ref == QLatin1String("@events")) {
        out.key = nodeindex::BrowseEventsKey;
        out.title = label;
        out.alias = alias;
        out.detail = selfClass.isEmpty() ? QStringLiteral("this script")
                                         : selfClass;
        out.doc = selfClass.isEmpty()
                      ? QStringLiteral("The hooks a class can override, ranked, with "
                                       "the method each one becomes. Give this script "
                                       "a base class to fill the list.")
                      : QStringLiteral("The hooks %1 can override, ranked, with the "
                                       "method each one becomes.").arg(selfClass);
        return out;
    }

    if (isBuiltinRef(ref)) return builtinRow(builtins, ref, label, detail, alias);

    // "Owner::Name", with "global" naming the bucket the catalogue files free
    // functions under.
    const int split = ref.indexOf(QLatin1String("::"));
    if (split <= 0) return out;
    const QString owner = ref.left(split);
    const QString name = ref.mid(split + 2);

    const QString key = keyFor(cat, owner, name);
    if (key.isEmpty()) return out;
    const MethodSig sig = sigFor(cat, key);
    if (!reachableFrom(cat, sig, selfClass)) return out;

    out.key = key;
    out.title = label.isEmpty() ? name : label;
    // The owner by default: it is what tells `ConfigGetString` on this item from
    // `ConfigGetText` on any config path, and the two sit next to each other.
    out.detail = detail.isEmpty() ? owner : detail;
    out.doc = nodeSummary(cat, builtins, key);
    out.alias = alias;
    return out;
}

} // namespace

QVector<IndexGroup> nodeIndex(const Catalog &cat, const Builtins &builtins,
                              const QString &selfClass)
{
    QVector<IndexGroup> out;
    out.reserve(kGroupCount + 1);
    QSet<QString> placed;

    for (int i = 0; i < kGroupCount; ++i) {
        const Group &g = kGroups[i];
        IndexGroup group;
        group.title = QString::fromUtf8(g.title);
        group.doc = QString::fromUtf8(g.doc);
        for (int r = 0; r < g.count; ++r) {
            // One reference stands for a family: the operator presets are keyed
            // off the list Builtins publishes, so adding an operator there puts
            // it here rather than in the tail.
            if (QLatin1String(g.rows[r].ref) == QLatin1String("@operators")) {
                for (const QString &symbol : Builtins::binaryOperators()) {
                    const IndexRow row = builtinRow(
                        builtins, QStringLiteral("bi.op.") + symbol, QString(), QString());
                    if (row.key.isEmpty()) continue;
                    group.rows.append(row);
                    placed.insert(row.key);
                }
                continue;
            }
            const IndexRow row = resolve(cat, builtins, g.rows[r], selfClass);
            if (row.key.isEmpty()) continue;
            group.rows.append(row);
            placed.insert(row.key);
        }
        // A group that resolved to nothing is a group with nothing to say. It
        // can happen legitimately: every row in Read a config value is declared
        // on Object, and a graph that is not an Object cannot call one.
        if (!group.rows.isEmpty()) out.append(group);
    }

    // Whatever the table did not name. Built from Builtins::all() rather than a
    // second list, so this cannot fall behind builtins.cpp.
    IndexGroup rest;
    rest.title = QString::fromUtf8(kRestTitle);
    rest.doc = QString::fromUtf8(kRestDoc);
    for (const NodeDef &def : builtins.all()) {
        if (placed.contains(def.key)) continue;
        IndexRow row;
        row.key = def.key;
        row.title = def.title;
        row.detail = def.subtitle.isEmpty() ? def.category : def.subtitle;
        row.doc = oneLine(builtinSummary(def));
        rest.rows.append(row);
    }
    if (!rest.rows.isEmpty()) out.append(rest);

    return out;
}

bool matchesQuery(const QString &query, const QStringList &fields)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) return true;

    static const QRegularExpression gap(QStringLiteral("\\s+"));
    const QStringList terms = q.split(gap, Qt::SkipEmptyParts);
    if (terms.isEmpty()) return true;

    // "config get" against ConfigGetString. Checked first, because a row that
    // spells the whole thing is a better answer than one that happens to carry
    // both words apart.
    const QString joined = terms.join(QString());
    for (const QString &field : fields)
        if (field.contains(joined, Qt::CaseInsensitive)) return true;

    for (const QString &term : terms) {
        bool landed = false;
        for (const QString &field : fields) {
            if (!field.contains(term, Qt::CaseInsensitive)) continue;
            landed = true;
            break;
        }
        if (!landed) return false;
    }
    return true;
}

bool rowMatches(const QString &query, const IndexRow &row, const QString &groupTitle)
{
    return matchesQuery(query, {row.title, row.detail, groupTitle, row.alias});
}

bool eventFitsClass(const Catalog &cat, const QString &category, const QString &owner,
                    const QString &selfClass)
{
    if (category != QLatin1String("Events")) return true;
    if (selfClass.isEmpty() || owner.isEmpty()) return true;
    // A base class that is another script in this project, or a class from a
    // mod the catalogue was not built against. Nothing can be shown either way.
    if (cat.classId(selfClass) < 0 || cat.classId(owner) < 0) return true;
    return cat.isA(selfClass, owner);
}

QString nodeSummary(const Catalog &cat, const Builtins &builtins, const QString &key)
{
    if (key.isEmpty()) return {};
    if (key == nodeindex::BrowseEventsKey)
        return QStringLiteral("The hooks this class can override, ranked, with the "
                              "method each one becomes.");
    if (isBuiltinRef(key)) {
        const NodeDef def = builtins.def(key);
        return def.valid ? oneLine(builtinSummary(def)) : QString();
    }

    const NodeHelp help = cat.explain(key);
    const QString written = help.valid ? oneLine(help.summary) : QString();
    if (!written.isEmpty()) return written;

    // Three quarters of vanilla carries no doc comment. The declaration is
    // still an answer to "what does this take and what does it give me", and it
    // is a true one, which a written guess would not be.
    const MethodSig sig = sigFor(cat, key);
    if (sig.valid) {
        const QString where = sig.owner.isEmpty()
                                  ? QStringLiteral("A global function.")
                                  : QStringLiteral("Declared on %1.").arg(sig.owner);
        return QStringLiteral("%1 %2%3").arg(where, sig.name, signatureOf(sig));
    }
    if (help.valid && !help.kind.isEmpty()) return help.kind;
    return {};
}

QStringList nodeCautions(const Catalog &cat, const Builtins &builtins,
                         const QString &key)
{
    if (key.isEmpty() || key == nodeindex::BrowseEventsKey) return {};
    if (isBuiltinRef(key)) return builtinCautions(builtins.def(key));
    const NodeHelp help = cat.explain(key);
    return help.valid ? help.cautions : QStringList();
}

QStringList curatedRefs()
{
    QStringList out;
    for (int i = 0; i < kGroupCount; ++i)
        for (int r = 0; r < kGroups[i].count; ++r)
            out << QString::fromUtf8(kGroups[i].rows[r].ref);
    return out;
}
