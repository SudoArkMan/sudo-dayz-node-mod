// Loads the generated node catalogue (catalog.json, produced from the
// dayz-script-api index) and turns packed entries into NodeDefs on demand.
// Every vanilla class, method, enum, global function and constant is
// reachable as a node; defs are built lazily and memoised.
//
// Catalogue keys: "m<idx>" method, "g<idx>" global, "en<idx>" enum,
// "co<idx>" constant. Builtin ids ("bi.*") are resolved elsewhere.
//
// <idx> is a plain in-range decimal and nothing else: a key carrying a name, a
// sign, whitespace or an overflowing number names no entry at all, so every
// lookup here reports "unknown" for it rather than falling back to entry 0.
#pragma once

#include "graph.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace accents {
// Node header accents, by role.
QColor event();
QColor call();
QColor pure();
QColor flow();
QColor variable();
QColor literal();
QColor cast();
QColor comment();
} // namespace accents

// Method flags, packed into one int per entry in catalog.json.
//
// Access lives here too, and it decides whether a call compiles at all.
// `Protected` is carried by every protected declaration the catalogue holds,
// 2,204 of them, checked entry for entry against the dayz-script-api index.
// There is no `Private` bit because a private method never reaches the
// catalogue: all 344 callable ones are dropped where it is built, since a
// `modded class` inherits rather than reopens and cannot see them either. So
// everything in here is public or protected, and nothing else.
// tools/catalog_access.mjs proves both after a rebuild.
namespace flag {
constexpr int Static = 1;
constexpr int Proto = 2;
constexpr int Native = 4;
constexpr int Override = 8;
constexpr int Pure = 16;
constexpr int Event = 32;
constexpr int Protected = 64;
constexpr int Ctor = 128;
} // namespace flag

struct ClassInfo {
    int id = -1;
    QString name;
    int extendsId = -1;
    QString file;
    int line = 0;
    QString module;
    QString guards;
    QString doc;
    bool valid = false;
};

struct SearchHit {
    QString key;
    QString title;
    QString subtitle;
    QString sig;      // compact "(int, string) : bool", tells overloads apart
    QString category;
    int score = 0;
    QString guards;   // non-empty when the declaration sits behind #ifdefs
};

struct SearchOptions {
    int limit = 60;
    QString category; // filter to one category when set
    QString ofClass;  // restrict to members of this class and its ancestors
    // The class the graph doing the calling compiles into, from selfClassOf().
    QString selfClass;
    // Withhold whatever `selfClass` could not legally call. Off by default on
    // purpose: the importer and the generator resolve names through this same
    // search while working inside the declaring class, where protected is
    // legal, and hiding it from them would turn working code back into text.
    // The palette is the one caller that wants it, because the palette is
    // where a node gets picked up and dropped onto somebody else's object.
    bool respectAccess = false;
};

// What a node does, for the inspector. `effects` is derived structurally from
// the signature and flags, so it exists for every node even though only a
// quarter of vanilla methods carry a doc comment.
struct NodeHelp {
    QString summary;
    QString kind;
    QStringList effects;
    QStringList cautions;
    QString source;
    bool documented = false;
    bool valid = false;
};

struct MethodSig {
    QString owner;
    QString name;
    QString ret;
    int flags = 0;
    struct Param { QString type; QString name; int dir = 0; QString def; };
    QVector<Param> params;
    bool valid = false;
};

class Catalog {
public:
    // Loads the packed catalogue; returns false (and sets error) on failure.
    bool load(const QString &jsonPath);
    QString error() const { return m_error; }
    bool isLoaded() const { return m_loaded; }

    QString source() const { return m_source; }
    QHash<QString, int> totals() const { return m_totals; }

    bool isEnum(const QString &name) const { return m_enumNames.contains(name); }

    int classCount() const { return m_classes.size(); }
    ClassInfo classInfo(int id) const;
    int classId(const QString &name) const; // -1 when unknown
    QStringList classNames() const;

    // Class plus every ancestor, nearest first.
    QVector<ClassInfo> ancestors(const QString &name) const;
    // True when `child` is `base` or descends from it.
    bool isA(const QString &child, const QString &base) const;

    // Whether code written inside `fromClass` may call a member declared on
    // `owner` with `flags`. Public members answer true for anybody. A
    // protected one answers true only from inside `owner` or a class that
    // inherits it, which is what makes `m_Timer.SetRunning(false)` from a
    // MissionServer a compile error and TimerBase's own use of it correct.
    //
    // An empty or unknown `fromClass` cannot be shown to inherit anything, so
    // it answers false for protected. Callers that would rather stay quiet
    // when nothing can be proved have to check the class themselves; the
    // palette prefers the strict reading, because a node it never offered is
    // a compile error that never happened.
    bool accessAllowed(const QString &owner, int flags, const QString &fromClass) const;

    // Lazily built NodeDef for a catalogue key. Invalid def when unknown.
    NodeDef defFor(const QString &key) const;

    // Raw signature records, used by the code generator.
    MethodSig method(const QString &key) const;   // "m<idx>"
    MethodSig globalFn(const QString &key) const; // "g<idx>"
    QString constName(const QString &key) const;
    QString enumName(const QString &key) const;
    QStringList enumValues(const QString &enumName) const;

    // Cleaned doc comment, or "" when undocumented.
    QString doc(const QString &key) const;
    // Full inspector explanation.
    NodeHelp explain(const QString &key) const;

    // Ranked substring search over all 29k+ entries.
    QVector<SearchHit> search(const QString &query,
                              const SearchOptions &opts = {}) const;

private:
    struct Param { int type = 0; int name = 0; int dir = 0; int def = 0; };
    struct Method {
        int owner = 0, name = 0, ret = 0, flags = 0, line = 0, guards = 0, doc = 0;
        QVector<Param> params;
    };
    struct Class { int name = 0, extendsId = -1, file = 0, line = 0, module = 0,
                       guards = 0, doc = 0; };
    struct Enum { int name = 0, file = 0, line = 0; QVector<int> values; };
    struct Global {
        int name = 0, ret = 0, flags = 0, file = 0, line = 0, guards = 0, doc = 0;
        QVector<Param> params;
    };
    struct Const { int name = 0, type = 0, value = 0, file = 0, line = 0; };
    struct SearchRow {
        QString key, name, hay, title, sub, sig, cat, guards;
        int flags = 0; // as packed in the catalogue, for the access check
    };

    QString s(int id) const { return id >= 0 && id < m_strings.size()
                                  ? m_strings.at(id) : QString(); }
    QString fileAt(int idx) const { return idx >= 0 && idx < m_files.size()
                                        ? m_files.at(idx) : QString(); }
    Pin makePin(const QString &id, const QString &label, PinDir dir,
                const PinType &type) const;
    QVector<Pin> paramPins(const QVector<Param> &params) const;
    NodeDef build(const QString &key) const;
    void buildSearchIndex();

    bool m_loaded = false;
    QString m_error;
    QString m_source;
    QHash<QString, int> m_totals;

    QVector<QString> m_strings;
    QVector<QString> m_files;
    QVector<Class> m_classes;
    QVector<Method> m_methods;
    QVector<Enum> m_enums;
    QVector<Global> m_globals;
    QVector<Const> m_consts;

    QSet<QString> m_enumNames;
    QHash<QString, int> m_classByName;
    QHash<int, QVector<int>> m_methodsByClass;
    QVector<SearchRow> m_search;
    mutable QHash<QString, NodeDef> m_defCache;
};
