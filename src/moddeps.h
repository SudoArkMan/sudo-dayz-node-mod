// Depending on another mod.
//
// Very little DayZ script is written against vanilla alone. Community
// Framework (CF, addon JM_CF_Scripts) is the shared library layer most mods
// build on, and Community Online Tools (COT, addon JM_COT_Scripts) is the admin
// tool built on top of it: GUI menus for teleporting players, setting health,
// changing weather and spawning vehicles, plus the permissions and roles
// framework other mods gate their admin features on. That framework is the
// reason so many mods list COT, and it is also why "depends on COT" usually
// means CF as well, or CF alone.
//
// Two halves live here. A ModDependency records what a mod IS: the addons it
// ships, the addons it needs, and the define it sets when it is loaded.
// A ModIndex records what a mod DECLARES: the classes and methods read off the
// user's own copy, in the shape the vanilla catalogue holds, so the palette and
// the canvas can treat vanilla and a dependency the same way.
//
// Keys are prefixed and named, never positional. A vanilla key is an index into
// a packed table ("m1204"), which works for a catalogue generated in one piece,
// but a dependency is indexed by walking a folder: updating the mod would slide
// every index along and rebind saved nodes to the wrong method. A key like
// "dep.JM_COT_Scripts.JMModuleManager.GetModule" survives a reindex, a mod
// update that leaves the method alone, and reordering the dependency list. The
// "dep." prefix is what keeps it clear of vanilla keys and of builtin "bi." ids.
//
// What the index does not hold: enums, constants, and functions declared
// outside a class. The importer hands that part of a file back as text rather
// than as declarations, so reading it here would mean a second Enforce parser.
// Classes and their methods are what becomes a node.
#pragma once

#include "catalog.h"

#include <QColor>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class Builtins;
struct Project;

struct ModDependency {
    QString id;            // the CfgPatches addon name, "JM_COT_Scripts"
    QString displayName;   // "Community Online Tools"
    QString shortName;     // "COT", what the node badge shows
    // Folder holding the mod's Scripts tree, when the user has a copy on disk.
    // Held absolute and written to the .sdzn relative to the project file, the
    // way modRoot is, so moving a folder does not strand it. Empty is a normal
    // state, not a broken one: the facts below are still worth carrying when
    // the source is not installed on this machine.
    QString scriptRoot;
    QStringList addons;    // every addon it ships
    // `requires` is an ordinary identifier under C++17 and a keyword under
    // C++20. This project is pinned to 17, and the name matches the config.cpp
    // key the list is read from.
    QStringList requires;  // addons it needs itself
    // The macro the mod defines when it loads. It is what makes `optional`
    // possible: a feature wrapped in #ifdef on this name compiles either way,
    // so the mod still builds for a server that does not run the dependency.
    QString loadedDefine;
    // May be invalid, which means "nobody chose one". badgeColorFor(id) is the
    // answer then, and it is left to whoever draws the badge so that the model
    // layer does not have to know about the theme.
    QColor badgeColor;
    bool optional = false;

    bool isValid() const { return !id.isEmpty(); }
};

// COT and CF, from their published config.cpp. Presets rather than a registry:
// every field stays editable, and a dependency read off disk wins over a preset
// whenever the two disagree.
QVector<ModDependency> knownDependencies();
// One preset by addon id. Comes back with an empty id when there is no preset.
ModDependency knownDependency(const QString &id);

// A stable badge colour for an addon id, taken from the theme accent's own
// saturation and value with the hue moved by a hash of the id. That gives a
// dependency the user adds a colour of its own without a hex outside theme.cpp,
// and puts the same mod in the same colour on every machine.
QColor badgeColorFor(const QString &id);

// A badge-sized name from a display name: its capitals, so "Community Online
// Tools" gives "COT". Falls back to the first letters when there are none.
//
// Inline because the .sdzn reader falls back to it for a hand-written file that
// left the short name out, and project.cpp must not have to link this module's
// object file to do that: doing so would drag the whole importer into every
// test that opens a project.
inline QString shortNameFor(const QString &displayName)
{
    QString caps;
    for (const QChar c : displayName)
        if (c.isUpper() || c.isDigit()) caps.append(c);
    if (caps.size() >= 2) return caps.left(4);
    const QString trimmed = displayName.trimmed();
    if (trimmed.isEmpty()) return QString();
    return trimmed.left(3).toUpper();
}

// ------------------------------------------------------------ reading a folder

// What a config.cpp says about the addons it ships and the addons it needs.
struct AddonFacts {
    QStringList addons;    // every class under CfgPatches, in file order
    QStringList requires;  // every requiredAddons entry, once each
    QString modClass;      // the CfgMods class name, when there is one
    QString modName;       // its `name` property, when it is set
    QString modDir;        // its `dir` property
    bool found = false;    // there was a CfgPatches to read
};

// Reads one config.cpp. A file that is not there, or that carries no CfgPatches,
// comes back with found = false and the reason in `error`.
AddonFacts readAddonFacts(const QString &configPath, QString *error = nullptr);

// The Scripts folder inside a mod folder: <folder>/Scripts, or <folder> itself
// when that is already the Scripts folder, or the Scripts folder one level down,
// which is the layout COT ships (@CommunityOnlineTools/JM/COT/Scripts). Empty
// when there is none.
QString scriptsDirFor(const QString &folder);

// The config.cpp belonging to a mod folder: beside its scripts first, then in
// the folder itself. Empty when neither is there.
QString configPathFor(const QString &folder);

// A dependency built from a folder the user pointed at, so nobody has to type an
// addon name. The id is the first CfgPatches class, which is the addon the rest
// of the mod is named after; requiredAddons becomes `requires`, minus the
// addons this mod ships itself. A preset for the same id fills in what a
// config.cpp cannot say (the short name, the loaded define). Comes back with an
// empty id when the folder holds no config this can read, with `error` saying
// which part was missing.
ModDependency dependencyFromFolder(const QString &folder, QString *error = nullptr);

// The .sdzn shape of a dependency lives in project.cpp, beside the shape of
// everything else the file holds.

// ------------------------------------------------------------------- the index

// A class one dependency declares.
struct ModClass {
    QString name;
    QString base;
    bool modded = false;   // `modded class X`, so it extends the class it names
    QString depId;
    QString file;          // path relative to the dependency's scriptRoot
    QString module;        // 3_Game / 4_World / 5_Mission, when the path says so
    bool valid = false;
};

// One callable a dependency declares. Param is the catalogue's own param record,
// so a dependency method and a vanilla method reach the generator identically.
struct ModMethod {
    QString key;
    QString depId;
    QString owner;         // declaring class
    QString name;
    QString ret;
    QVector<MethodSig::Param> params;
    int flags = 0;         // flag::Static, flag::Override, flag::Protected, flag::Ctor
    QString file;
    // Which declaration of this name on this class it is. Only overloads carry
    // a non-zero value, and it is what puts the "#2" on the second key.
    int overload = 0;
    bool valid = false;
};

class ModIndex {
public:
    // Walks the dependency's Scripts tree and reads every .c under it.
    //
    // A scriptRoot that is not on disk adds the dependency and nothing else:
    // the preset facts stay usable, so a project opened on a machine without
    // the mod installed still knows what it depends on and still draws its
    // badges. That case returns false with the reason in `error`, which means
    // "there was nothing to read", not "the project is broken".
    //
    // Reading a script tree costs the same as opening each of those files in
    // the editor, so this is on demand rather than at startup, and what it
    // produces is kept until clear().
    bool add(const ModDependency &dep, const Catalog &cat, const Builtins &builtins,
             QString *error = nullptr);
    void clear();

    QVector<ModDependency> dependencies() const { return m_deps; }
    const ModDependency *dependency(const QString &id) const;
    bool isIndexed(const QString &id) const { return m_indexed.contains(id); }

    int classCount() const { return m_classes.size(); }
    int methodCount() const { return m_methods.size(); }
    QVector<ModClass> classes() const { return m_classes; }
    ModClass classInfo(const QString &name) const;  // invalid when unknown
    // The classes `name` descends through inside this index, nearest first,
    // stopping at the first base the index does not know. That base is a
    // vanilla class, and Catalog::ancestors carries on from there.
    QStringList modAncestors(const QString &name) const;

    // True for a key this module owns. Cheap enough to call once per node.
    static bool isDependencyKey(const QString &key);
    // The dependency id carried inside a key. Read from the key itself, so a
    // node badge resolves before anything has been indexed and on a machine
    // that does not have the mod. Empty when the key is not one of ours.
    static QString dependencyIdOf(const QString &key);

    ModMethod methodInfo(const QString &key) const;
    // Catalogue-shaped lookups. `method` feeds the generator, `defFor` feeds the
    // canvas. The catalogue is passed to defFor because pin types are resolved
    // against it: a parameter typed with a vanilla enum has to come out an enum
    // pin, and only the catalogue knows which names those are.
    MethodSig method(const QString &key) const;
    NodeDef defFor(const QString &key, const Catalog &cat) const;

    // Ranked search over everything indexed, same options as Catalog::search.
    // `ofClass` is matched against this index's own inheritance chain.
    QVector<SearchHit> search(const QString &query, const SearchOptions &opts = {}) const;

    // Names the walk refused and files it could not read. Not failures: a mod
    // tree holds generated files, third-party copies and half-written ones, and
    // the palette has to stay clean whatever is in there. Capped, with a count
    // of what did not fit on the end.
    QStringList notes() const;

private:
    struct SearchRow { QString key, name, hay, title, sub, sig, cat; };

    void note(const QString &text);
    void dropDependency(const QString &id);
    // Every lookup is rebuilt from m_classes and m_methods rather than patched,
    // because add() may be replacing a dependency that is already in here and a
    // half-updated hash would answer with a key that no longer exists.
    void rebuildLookups();

    QVector<ModDependency> m_deps;
    QSet<QString> m_indexed;
    QVector<ModClass> m_classes;
    QHash<QString, int> m_classByName;
    QVector<ModMethod> m_methods;
    QHash<QString, int> m_methodByKey;
    QVector<SearchRow> m_search;
    QStringList m_notes;
    int m_noteCount = 0;
    mutable QHash<QString, NodeDef> m_defCache;
};
