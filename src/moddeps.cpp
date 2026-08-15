#include "moddeps.h"

#include "builtins.h"
#include "config/configtree.h"
#include "enforce/import.h"
#include "project.h"
#include "theme.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <functional>

// The catalogue's own guard, defined in catalog.cpp. The index builder behind
// catalog.json mis-reads 542 member declarations as methods, and this is what
// keeps names like `m_Data = new AutotestConfigJson` out of the palette. A mod
// folder is no more trustworthy than that index, so the same rule applies to
// everything read out of one, and reusing the function keeps the two from
// drifting apart.
bool isCallableName(const QString &name);

namespace {

// An addon name, a class name and a method name are all Enforce identifiers, so
// a key can join them with dots and still be taken apart again. Anything that
// is not one would break that, which is reason enough to refuse it.
bool isIdentifier(const QString &text)
{
    return isCallableName(text);
}

const int kMaxNotes = 40;

QString relativeTo(const QString &root, const QString &path)
{
    if (root.isEmpty()) return QFileInfo(path).fileName();
    return QDir(root).relativeFilePath(path);
}

// Mirrors Catalog::makePin and Catalog::paramPins. Those are private members
// working on the catalogue's interned strings, so they cannot be called with a
// dependency's params; the shape they produce is what the canvas and the
// generator expect, so it is reproduced rather than reinvented.
Pin makePin(const QString &id, const QString &label, PinDir dir, const PinType &type)
{
    Pin p;
    p.id = id;
    p.label = label;
    p.dir = dir;
    p.type = type;
    if (dir == PinDir::In && type.kind != PinKind::Exec
        && inlineEditorFor(type) != InlineEditor::None) {
        p.def = defaultLiteral(type);
        p.hasDef = true;
    }
    return p;
}

QVector<Pin> paramPins(const QVector<MethodSig::Param> &params,
                       const std::function<bool(const QString &)> &isEnumFn)
{
    QVector<Pin> pins;
    for (int i = 0; i < params.size(); ++i) {
        const MethodSig::Param &p = params.at(i);
        const PinType type = pinTypeOf(p.type, isEnumFn);
        QString base = p.name;
        if (base.isEmpty()) base = QStringLiteral("arg%1").arg(i);
        const QString label = p.def.isEmpty()
                                  ? base
                                  : QStringLiteral("%1 = %2").arg(base, p.def);
        if (p.dir == 0 || p.dir == 2) {
            Pin in = makePin(QStringLiteral("p%1").arg(i), label, PinDir::In, type);
            if (!p.def.isEmpty()) { // let codegen drop optionals
                in.def.clear();
                in.hasDef = false;
            }
            pins.append(in);
        }
        if (p.dir == 1 || p.dir == 2)
            pins.append(makePin(QStringLiteral("o%1").arg(i), base, PinDir::Out, type));
    }
    return pins;
}

// The importer keeps a parameter as the author wrote it: `out` and `inout` stay
// on the type because they change what the call does, and a default value rides
// on the name because there was nowhere else to put it. The catalogue splits
// all three apart, and pin building depends on that split, so it is undone here.
MethodSig::Param paramFromGraph(const GraphParam &in)
{
    MethodSig::Param out;
    QString type = in.type.trimmed();
    for (;;) {
        const int sp = type.indexOf(QLatin1Char(' '));
        if (sp <= 0) break;
        const QString word = type.left(sp);
        if (word == QLatin1String("out")) out.dir = 1;
        else if (word == QLatin1String("inout")) out.dir = 2;
        else if (word != QLatin1String("notnull") && word != QLatin1String("ref")
                 && word != QLatin1String("autoptr"))
            break;
        type = type.mid(sp + 1).trimmed();
    }
    out.type = type;

    QString name = in.name.trimmed();
    const int eq = name.indexOf(QLatin1Char('='));
    if (eq >= 0) {
        out.def = name.mid(eq + 1).trimmed();
        name = name.left(eq).trimmed();
    }
    out.name = name;
    return out;
}

} // namespace

// ------------------------------------------------------------------- presets

QColor badgeColorFor(const QString &id)
{
    int h = 0, s = 0, v = 0;
    theme::accent().getHsv(&h, &s, &v);
    if (id.isEmpty()) return theme::accent();
    // qHash is seeded per process, so the same addon would get a different
    // colour on every launch. This walks the bytes itself (FNV-1a) to keep a
    // mod the same colour on every machine and in every session.
    quint32 acc = 2166136261u;
    for (const QChar c : id) {
        acc ^= quint32(c.unicode());
        acc *= 16777619u;
    }
    return QColor::fromHsv(int(acc % 360u), s, v);
}

QVector<ModDependency> knownDependencies()
{
    QVector<ModDependency> out;

    // Community Framework. Everything here comes from COT's own requiredAddons:
    // JM_CF_Scripts is the addon mods list. CF ships more than that one addon
    // and defines macros of its own, but none of that has been read from the
    // source, so those fields stay empty rather than being filled with a guess.
    ModDependency cf;
    cf.id = QStringLiteral("JM_CF_Scripts");
    cf.displayName = QStringLiteral("Community Framework");
    cf.shortName = QStringLiteral("CF");
    cf.addons = {QStringLiteral("JM_CF_Scripts")};
    cf.badgeColor = badgeColorFor(cf.id);
    out.append(cf);

    // Community Online Tools, from JM/COT/Scripts/config.cpp: CfgPatches
    // declares JM_COT_Scripts requiring JM_CF_Scripts and DZ_Data, the mod also
    // ships JM_COT_GUI, and the file defines JM_COT_LOADED. That define is what
    // an optional integration is guarded on, so a mod that offers COT menus
    // still builds for a server that does not run COT.
    ModDependency cot;
    cot.id = QStringLiteral("JM_COT_Scripts");
    cot.displayName = QStringLiteral("Community Online Tools");
    cot.shortName = QStringLiteral("COT");
    cot.addons = {QStringLiteral("JM_COT_Scripts"), QStringLiteral("JM_COT_GUI")};
    cot.requires = {QStringLiteral("JM_CF_Scripts"), QStringLiteral("DZ_Data")};
    cot.loadedDefine = QStringLiteral("JM_COT_LOADED");
    cot.badgeColor = badgeColorFor(cot.id);
    out.append(cot);

    return out;
}

ModDependency knownDependency(const QString &id)
{
    for (const ModDependency &d : knownDependencies())
        if (d.id == id) return d;
    return {};
}

// ------------------------------------------------------------ reading a folder

QString scriptsDirFor(const QString &folder)
{
    if (folder.isEmpty()) return QString();
    const QFileInfo info(folder);
    if (!info.isDir()) return QString();
    const QDir dir(info.absoluteFilePath());

    // Mods ship both spellings of the folder name, and Qt's file engine is case
    // insensitive on Windows and case sensitive everywhere else, so entries are
    // compared by name rather than tested with exists().
    const auto scriptsIn = [](const QDir &d) -> QString {
        const QStringList names = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &name : names)
            if (name.compare(QLatin1String("Scripts"), Qt::CaseInsensitive) == 0)
                return d.absoluteFilePath(name);
        return QString();
    };

    if (dir.dirName().compare(QLatin1String("Scripts"), Qt::CaseInsensitive) == 0)
        return dir.absolutePath();
    const QString here = scriptsIn(dir);
    if (!here.isEmpty()) return here;

    // COT ships JM/COT/Scripts, so a folder pointing at the PBO prefix has its
    // scripts one level further down. One level only: any deeper and this would
    // start picking up somebody else's mod that happens to sit in the same tree.
    const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : subs) {
        const QString found = scriptsIn(QDir(dir.absoluteFilePath(name)));
        if (!found.isEmpty()) return found;
    }
    return QString();
}

QString configPathFor(const QString &folder)
{
    if (folder.isEmpty()) return QString();
    QStringList candidates;
    const QString scripts = scriptsDirFor(folder);
    if (!scripts.isEmpty()) {
        candidates << QDir(scripts).absoluteFilePath(QStringLiteral("config.cpp"));
        candidates << QDir(QFileInfo(scripts).absolutePath())
                          .absoluteFilePath(QStringLiteral("config.cpp"));
    }
    const QFileInfo info(folder);
    if (info.isDir())
        candidates << QDir(info.absoluteFilePath()).absoluteFilePath(QStringLiteral("config.cpp"));
    for (const QString &path : candidates)
        if (QFileInfo::exists(path)) return path;
    return QString();
}

AddonFacts readAddonFacts(const QString &configPath, QString *error)
{
    AddonFacts facts;
    if (configPath.isEmpty()) {
        if (error) *error = QStringLiteral("no config.cpp to read");
        return facts;
    }
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("could not read %1: %2")
                                .arg(QDir::toNativeSeparators(configPath), file.errorString());
        return facts;
    }
    // A config.cpp is not always valid UTF-8, and a decoder that refuses takes
    // the whole file with it, so the bytes are read the way the engine reads
    // them.
    const QString text = QString::fromLatin1(file.readAll());
    file.close();

    ConfigFile cfg = parseConfig(text);
    ConfigClass *patches = findClass(cfg, QStringLiteral("CfgPatches"));
    if (!patches) {
        if (error) *error = QStringLiteral("%1 has no CfgPatches, so it names no addon")
                                .arg(QFileInfo(configPath).fileName());
        return facts;
    }
    facts.found = true;

    QSet<QString> seenRequired;
    for (ConfigClass &addon : patches->classes) {
        if (addon.name.isEmpty()) continue;
        if (!facts.addons.contains(addon.name)) facts.addons.append(addon.name);
        const ConfigValue *required = findValue(addon, QStringLiteral("requiredAddons"));
        if (!required) continue;
        for (const QString &item : required->items) {
            const QString name = configUnquote(item).trimmed();
            if (name.isEmpty() || seenRequired.contains(name)) continue;
            seenRequired.insert(name);
            facts.requires.append(name);
        }
    }

    if (ConfigClass *mods = findClass(cfg, QStringLiteral("CfgMods"))) {
        for (ConfigClass &mod : mods->classes) {
            if (mod.name.isEmpty()) continue;
            facts.modClass = mod.name;
            if (const ConfigValue *v = findValue(mod, QStringLiteral("name")))
                facts.modName = configUnquote(v->scalar).trimmed();
            if (const ConfigValue *v = findValue(mod, QStringLiteral("dir")))
                facts.modDir = configUnquote(v->scalar).trimmed();
            break;
        }
    }

    if (facts.addons.isEmpty() && error)
        *error = QStringLiteral("%1 has a CfgPatches with no addon class in it")
                     .arg(QFileInfo(configPath).fileName());
    return facts;
}

ModDependency dependencyFromFolder(const QString &folder, QString *error)
{
    ModDependency dep;
    const QString config = configPathFor(folder);
    if (config.isEmpty()) {
        if (error) *error = QStringLiteral("%1 holds no config.cpp, so there is no addon name "
                                           "to read")
                                .arg(QDir::toNativeSeparators(folder));
        return dep;
    }
    QString why;
    const AddonFacts facts = readAddonFacts(config, &why);
    if (facts.addons.isEmpty()) {
        if (error) *error = why;
        return dep;
    }

    // The first CfgPatches class is the addon the rest of the mod is named
    // after, and it is what other mods put in their own requiredAddons.
    dep.id = facts.addons.first();
    dep.addons = facts.addons;
    // A mod listing its own addons under requiredAddons is common and says
    // nothing about what it depends on, so its own names come out.
    const QSet<QString> ships(facts.addons.begin(), facts.addons.end());
    for (const QString &name : facts.requires)
        if (!ships.contains(name)) dep.requires.append(name);

    const QString scripts = scriptsDirFor(folder);
    dep.scriptRoot = scripts.isEmpty() ? QFileInfo(folder).absoluteFilePath()
                                       : QFileInfo(scripts).absolutePath();

    dep.displayName = facts.modName;
    if (dep.displayName.isEmpty()) dep.displayName = facts.modClass;

    // A preset fills in what a config.cpp cannot say: the badge name and the
    // define an optional integration is guarded on. What the file does say
    // stays as the file said it.
    const ModDependency preset = knownDependency(dep.id);
    if (preset.isValid()) {
        dep.shortName = preset.shortName;
        dep.loadedDefine = preset.loadedDefine;
        if (dep.displayName.isEmpty()) dep.displayName = preset.displayName;
    }
    if (dep.displayName.isEmpty()) dep.displayName = dep.id;
    if (dep.shortName.isEmpty()) dep.shortName = shortNameFor(dep.displayName);
    dep.badgeColor = badgeColorFor(dep.id);
    return dep;
}

// ------------------------------------------------------------------- the index

bool ModIndex::isDependencyKey(const QString &key)
{
    return key.startsWith(QLatin1String("dep."));
}

QString ModIndex::dependencyIdOf(const QString &key)
{
    if (!isDependencyKey(key)) return QString();
    const int start = 4;
    const int end = key.indexOf(QLatin1Char('.'), start);
    if (end <= start) return QString();
    const QString id = key.mid(start, end - start);
    return isIdentifier(id) ? id : QString();
}

namespace {

QString methodKey(const QString &depId, const QString &owner, const QString &name, int overload)
{
    QString key = QStringLiteral("dep.%1.%2.%3").arg(depId, owner, name);
    // Overloads are the one place a name is not enough. '#' is not an
    // identifier character, so a suffix cannot be mistaken for part of a name.
    if (overload > 0) key += QStringLiteral("#%1").arg(overload + 1);
    return key;
}

} // namespace

void ModIndex::clear()
{
    m_deps.clear();
    m_indexed.clear();
    m_classes.clear();
    m_classByName.clear();
    m_methods.clear();
    m_methodByKey.clear();
    m_search.clear();
    m_notes.clear();
    m_noteCount = 0;
    m_defCache.clear();
}

const ModDependency *ModIndex::dependency(const QString &id) const
{
    for (const ModDependency &d : m_deps)
        if (d.id == id) return &d;
    return nullptr;
}

ModClass ModIndex::classInfo(const QString &name) const
{
    const int at = m_classByName.value(name, -1);
    if (at < 0 || at >= m_classes.size()) return {};
    return m_classes.at(at);
}

QStringList ModIndex::modAncestors(const QString &name) const
{
    QStringList out;
    QSet<QString> seen;
    QString at = name;
    while (!at.isEmpty() && !seen.contains(at)) {
        const ModClass info = classInfo(at);
        if (!info.valid) break;
        seen.insert(at);
        out.append(info.name);
        at = info.base;
    }
    return out;
}

ModMethod ModIndex::methodInfo(const QString &key) const
{
    const int at = m_methodByKey.value(key, -1);
    if (at < 0 || at >= m_methods.size()) return {};
    return m_methods.at(at);
}

MethodSig ModIndex::method(const QString &key) const
{
    const ModMethod m = methodInfo(key);
    if (!m.valid) return {};
    MethodSig sig;
    sig.owner = m.owner;
    sig.name = m.name;
    sig.ret = m.ret;
    sig.flags = m.flags;
    sig.params = m.params;
    sig.valid = true;
    return sig;
}

NodeDef ModIndex::defFor(const QString &key, const Catalog &cat) const
{
    if (key.isEmpty()) return {};
    const auto hit = m_defCache.constFind(key);
    if (hit != m_defCache.constEnd()) return hit.value();

    const ModMethod m = methodInfo(key);
    if (!m.valid) return {};
    const auto isEnumFn = [&cat](const QString &n) { return cat.isEnum(n); };

    const bool isStatic = m.flags & flag::Static;
    const bool isCtor = m.flags & flag::Ctor;

    NodeDef def;
    def.key = key;
    // A dependency method is always drawn as a call. Purity is a flag the
    // catalogue's index builder works out from the whole vanilla tree, and
    // guessing it per method here would put an exec pin on some nodes and not
    // others for no reason the user could see.
    def.pins.append(makePin(QStringLiteral("exec"), QString(), PinDir::In, {PinKind::Exec}));
    def.pins.append(makePin(QStringLiteral("exec"), QString(), PinDir::Out, {PinKind::Exec}));
    if (!isStatic && !isCtor && !m.owner.isEmpty())
        def.pins.append(makePin(QStringLiteral("target"), QStringLiteral("target"), PinDir::In,
                                {PinKind::Object, m.owner, false}));
    def.pins.append(paramPins(m.params, isEnumFn));
    if (isCtor) {
        def.pins.append(makePin(QStringLiteral("ret"), QStringLiteral("object"), PinDir::Out,
                                {PinKind::Object, m.owner, false}));
    } else if (!m.ret.isEmpty() && m.ret != QLatin1String("void")) {
        def.pins.append(makePin(QStringLiteral("ret"), QStringLiteral("return"), PinDir::Out,
                                pinTypeOf(m.ret, isEnumFn)));
    }

    def.title = isCtor ? QStringLiteral("Construct %1").arg(m.name) : m.name;
    def.subtitle = m.owner;
    def.category = QStringLiteral("Functions");
    def.accent = accents::call();
    def.loc = m.file;
    if (const ModDependency *dep = dependency(m.depId))
        def.doc = QStringLiteral("Declared by %1.").arg(dep->displayName.isEmpty()
                                                            ? dep->id
                                                            : dep->displayName);
    def.valid = true;
    m_defCache.insert(key, def);
    return def;
}

QVector<SearchHit> ModIndex::search(const QString &query, const SearchOptions &opts) const
{
    const QString q = query.trimmed().toLower();
    const int limit = opts.limit > 0 ? opts.limit : 60;
    if (q.isEmpty() && opts.ofClass.isEmpty()) return {};

    QSet<QString> allowed;
    if (!opts.ofClass.isEmpty()) {
        const QStringList chain = modAncestors(opts.ofClass);
        // A class this index does not know still restricts the result to
        // nothing rather than to everything, which is what the caller asked for.
        allowed.insert(opts.ofClass.toLower());
        for (const QString &name : chain) allowed.insert(name.toLower());
    }

    QVector<SearchHit> out;
    for (const SearchRow &e : m_search) {
        if (!allowed.isEmpty() && !allowed.contains(e.sub.toLower())) continue;
        if (!opts.category.isEmpty() && e.cat != opts.category) continue;

        if (q.isEmpty()) {
            out.append({e.key, e.title, e.sub, e.sig, e.cat, 0, QString()});
            if (out.size() >= limit * 4) break;
            continue;
        }
        const QString nameLower = e.name.toLower();
        int score = -1;
        if (nameLower == q) score = 1000;
        else if (nameLower.startsWith(q)) score = 800 - nameLower.size();
        else if (nameLower.contains(q)) score = 500 - nameLower.indexOf(q);
        else if (e.hay.contains(q)) score = 300;
        if (score < 0) continue;
        out.append({e.key, e.title, e.sub, e.sig, e.cat, score, QString()});
    }

    std::sort(out.begin(), out.end(), [](const SearchHit &a, const SearchHit &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.title.compare(b.title) < 0;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

void ModIndex::rebuildLookups()
{
    m_classByName.clear();
    m_methodByKey.clear();
    m_search.clear();
    m_defCache.clear();

    for (int i = 0; i < m_classes.size(); ++i)
        if (!m_classByName.contains(m_classes.at(i).name))
            m_classByName.insert(m_classes.at(i).name, i);
    for (int i = 0; i < m_methods.size(); ++i)
        m_methodByKey.insert(m_methods.at(i).key, i);

    m_search.reserve(m_methods.size());
    for (const ModMethod &m : m_methods) {
        // An override is the same call the class it came from already offers, so
        // showing it again would list one method under every class that touches
        // it. Catalog::buildSearchIndex drops them from the palette for the same
        // reason, and defFor still resolves the key either way.
        if (m.flags & flag::Override) continue;
        QStringList args;
        for (const MethodSig::Param &p : m.params) {
            const QString prefix = p.dir == 1 ? QStringLiteral("out ")
                                              : p.dir == 2 ? QStringLiteral("inout ")
                                                           : QString();
            args << prefix + p.type;
        }
        SearchRow row;
        row.key = m.key;
        row.name = m.name;
        row.sub = m.owner;
        row.hay = QStringLiteral("%1::%2").arg(row.sub, row.name).toLower();
        row.title = (m.flags & flag::Ctor) ? QStringLiteral("Construct %1").arg(m.name) : m.name;
        row.sig = QStringLiteral("(%1)%2")
                      .arg(args.join(QStringLiteral(", ")),
                           (!m.ret.isEmpty() && m.ret != QLatin1String("void"))
                               ? QStringLiteral(" : %1").arg(m.ret)
                               : QString());
        row.cat = QStringLiteral("Functions");
        m_search.append(row);
    }
}

void ModIndex::note(const QString &text)
{
    // A mod tree can hold thousands of files, and one note per file would bury
    // the few worth reading. The count still says how many there were.
    m_noteCount++;
    if (m_notes.size() < kMaxNotes) m_notes.append(text);
}

QStringList ModIndex::notes() const
{
    QStringList out = m_notes;
    if (m_noteCount > out.size())
        out.append(QStringLiteral("%1 more not listed").arg(m_noteCount - out.size()));
    return out;
}

void ModIndex::dropDependency(const QString &id)
{
    for (int i = m_deps.size() - 1; i >= 0; --i)
        if (m_deps.at(i).id == id) m_deps.removeAt(i);
    for (int i = m_classes.size() - 1; i >= 0; --i)
        if (m_classes.at(i).depId == id) m_classes.removeAt(i);
    for (int i = m_methods.size() - 1; i >= 0; --i)
        if (m_methods.at(i).depId == id) m_methods.removeAt(i);
    m_indexed.remove(id);
}

bool ModIndex::add(const ModDependency &dep, const Catalog &cat, const Builtins &builtins,
                   QString *error)
{
    if (!dep.isValid()) {
        if (error) *error = QStringLiteral("a dependency with no addon name cannot be indexed");
        return false;
    }
    if (!isIdentifier(dep.id)) {
        if (error) *error = QStringLiteral("\"%1\" is not an addon name, so nothing it declares "
                                           "could be keyed").arg(dep.id);
        return false;
    }

    dropDependency(dep.id);
    m_deps.append(dep);

    const QString scripts = scriptsDirFor(dep.scriptRoot);
    if (scripts.isEmpty()) {
        rebuildLookups();
        if (error)
            *error = dep.scriptRoot.isEmpty()
                         ? QStringLiteral("%1 has no folder set, so only what the project already "
                                          "knows about it is available").arg(dep.id)
                         : QStringLiteral("%1 holds no Scripts folder, so only what the project "
                                          "already knows about %2 is available")
                               .arg(QDir::toNativeSeparators(dep.scriptRoot), dep.id);
        return false;
    }
    const QString root = QFileInfo(scripts).absolutePath();

    // Sorted, because the order files arrive in decides which declaration of an
    // overloaded name gets the bare key and which gets the "#2". A key that
    // moved between two runs on the same folder would rebind saved nodes.
    QStringList files;
    QDirIterator it(scripts, {QStringLiteral("*.c")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) files.append(it.next());
    files.sort();

    // The dependency's scripts are not the user's project, so the user's mod
    // prefix and folders must not steer how they are read.
    const Project scratch;
    QHash<QString, int> seenOverloads;

    for (const QString &path : files) {
        const ImportResult res = importEnforceFile(path, cat, builtins, scratch);
        if (!res.ok) {
            note(QStringLiteral("%1: %2").arg(relativeTo(root, path), res.error));
            continue;
        }
        for (const ImportedScript &script : res.scripts) {
            if (!isIdentifier(script.className)) {
                note(QStringLiteral("%1: \"%2\" is not a class name")
                         .arg(relativeTo(root, path), script.className));
                continue;
            }
            ModClass cls;
            cls.name = script.className;
            // The importer leaves a modded class with no base, because
            // `modded class X` reopens X rather than extending anything new.
            // Leaving it empty is what stops the ancestor walk here and hands
            // the name to Catalog::ancestors, where the vanilla X really is.
            cls.base = script.baseClass;
            cls.modded = script.modded;
            cls.depId = dep.id;
            cls.file = relativeTo(root, path);
            cls.module = script.graph.module;
            cls.valid = true;
            // A class declared in more than one file (a modded class usually
            // is) keeps one entry and gains the methods of all of them.
            const auto already = std::find_if(m_classes.cbegin(), m_classes.cend(),
                                              [&cls](const ModClass &c) {
                                                  return c.name == cls.name;
                                              });
            if (already == m_classes.cend()) {
                m_classes.append(cls);
            } else if (already->depId != cls.depId) {
                note(QStringLiteral("%1: %2 is already declared by %3, so this one is listed "
                                    "under that entry")
                         .arg(cls.file, cls.name, already->depId));
            }

            for (const GraphFunction &fn : script.graph.functions) {
                // A private method cannot be called from anywhere the palette
                // could place a node, so it is not one.
                if (fn.isPrivate) continue;
                if (!isCallableName(fn.name)) {
                    note(QStringLiteral("%1: %2 declares \"%3\", which is not a callable name")
                             .arg(cls.file, cls.name, fn.name));
                    continue;
                }
                ModMethod m;
                m.depId = dep.id;
                m.owner = cls.name;
                m.name = fn.name;
                m.ret = fn.returns.isEmpty() ? QStringLiteral("void") : fn.returns;
                m.file = cls.file;
                for (const GraphParam &p : fn.params) m.params.append(paramFromGraph(p));
                if (fn.isStatic) m.flags |= flag::Static;
                if (fn.isOverride) m.flags |= flag::Override;
                if (fn.isProtected) m.flags |= flag::Protected;
                if (fn.name == cls.name) m.flags |= flag::Ctor;

                const QString bucket = cls.name + QLatin1Char('.') + fn.name;
                m.overload = seenOverloads.value(bucket, 0);
                seenOverloads.insert(bucket, m.overload + 1);
                m.key = methodKey(dep.id, cls.name, fn.name, m.overload);
                m.valid = true;
                m_methods.append(m);
            }
        }
    }

    m_indexed.insert(dep.id);
    rebuildLookups();
    return true;
}
