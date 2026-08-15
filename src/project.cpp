#include "project.h"

#include <QDebug>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

namespace {

// A .sdzn can arrive hand-edited, half-written, or from an older build. Two
// rules govern what happens then: anything still usable is repaired in place
// and kept, and anything that can never be drawn or generated is refused or
// dropped out loud. Silently loading a project that looks fine but is not is
// the outcome to avoid: the user only finds out when the mod will not build.
//
// Ids must be unique because every lookup in the model (Graph::node,
// Project::script, disconnectEdge, variableForRef) takes the first match: a
// duplicate id makes the second item unreachable and rebinds whatever pointed
// at it. Re-iding the later item keeps its content and leaves every existing
// reference resolving exactly where it already did.
template <typename T>
void repairIds(QVector<T> &items, const QString &prefix, const char *what,
               const QString &where)
{
    QSet<QString> seen;
    for (T &item : items) {
        if (!item.id.isEmpty() && !seen.contains(item.id)) {
            seen.insert(item.id);
            continue;
        }
        const QString was = item.id;
        do {
            item.id = nextId(prefix);
        } while (seen.contains(item.id));
        seen.insert(item.id);
        if (was.isEmpty())
            qWarning("sdzn: %s in %s had no id; it is now \"%s\"", what,
                     qPrintable(where), qPrintable(item.id));
        else
            qWarning("sdzn: %s in %s repeats the id \"%s\"; it is now \"%s\"", what,
                     qPrintable(where), qPrintable(was), qPrintable(item.id));
    }
}

void repairGraph(Graph &g, const QString &where)
{
    repairIds(g.nodes, QStringLiteral("n"), "a node", where);
    repairIds(g.edges, QStringLiteral("e"), "an edge", where);
    repairIds(g.variables, QStringLiteral("v"), "a variable", where);
    repairIds(g.functions, QStringLiteral("f"), "a function", where);

    // An edge that names a node which is not in the graph, or leaves an end
    // blank, connects nothing: the canvas cannot draw it and both the analyser
    // and the generator walk straight past it. Keeping it only risks a later
    // pass treating the empty id as a match.
    const auto endIsReal = [&g](const EdgeEnd &e) {
        return !e.node.isEmpty() && !e.pin.isEmpty() && g.node(e.node) != nullptr;
    };
    const auto endName = [](const EdgeEnd &e) {
        if (e.node.isEmpty()) return QStringLiteral("nothing");
        return e.node + QLatin1Char('.')
               + (e.pin.isEmpty() ? QStringLiteral("<no pin>") : e.pin);
    };
    for (int i = g.edges.size() - 1; i >= 0; --i) {
        const GraphEdge &e = g.edges.at(i);
        if (endIsReal(e.from) && endIsReal(e.to)) continue;
        qWarning("sdzn: dropped edge \"%s\" in %s: %s -> %s does not connect two "
                 "nodes of this graph", qPrintable(e.id), qPrintable(where),
                 qPrintable(endName(e.from)), qPrintable(endName(e.to)));
        g.edges.removeAt(i);
    }
}

// The .sdzn shape of one dependency. Only scriptRoot is rewritten on the way
// through, relative to the project file the way modRoot is, so moving a project
// folder does not strand the link to a mod beside it.
//
// badgeColor is written when something chose one and left out when nothing did.
// A file that carries no colour is not broken: badgeColorFor(id) answers for it
// at draw time, and keeping that call out of here is what stops the .sdzn
// reader from having to link the theme.
QJsonObject dependencyToJson(const ModDependency &dep, const QDir &base)
{
    QJsonObject o;
    o.insert("id", dep.id);
    if (!dep.displayName.isEmpty()) o.insert("displayName", dep.displayName);
    if (!dep.shortName.isEmpty()) o.insert("shortName", dep.shortName);
    if (!dep.scriptRoot.isEmpty())
        o.insert("scriptRoot", base.relativeFilePath(dep.scriptRoot));
    const auto listOf = [](const QStringList &items) {
        QJsonArray a;
        for (const QString &s : items) a.append(s);
        return a;
    };
    if (!dep.addons.isEmpty()) o.insert("addons", listOf(dep.addons));
    if (!dep.requires.isEmpty()) o.insert("requires", listOf(dep.requires));
    if (!dep.loadedDefine.isEmpty()) o.insert("loadedDefine", dep.loadedDefine);
    if (dep.badgeColor.isValid()) o.insert("badgeColor", dep.badgeColor.name());
    if (dep.optional) o.insert("optional", true);
    return o;
}

ModDependency dependencyFromJson(const QJsonObject &o, const QDir &base)
{
    ModDependency dep;
    dep.id = o.value("id").toString().trimmed();
    dep.displayName = o.value("displayName").toString();
    dep.shortName = o.value("shortName").toString();
    const QString rel = o.value("scriptRoot").toString();
    if (!rel.isEmpty()) dep.scriptRoot = QDir::cleanPath(base.absoluteFilePath(rel));
    const auto listFrom = [&o](const char *key) {
        QStringList out;
        for (const QJsonValue &v : o.value(QLatin1String(key)).toArray()) {
            const QString s = v.toString();
            if (!s.isEmpty()) out.append(s);
        }
        return out;
    };
    dep.addons = listFrom("addons");
    dep.requires = listFrom("requires");
    dep.loadedDefine = o.value("loadedDefine").toString();
    const QColor stored(o.value("badgeColor").toString());
    if (stored.isValid()) dep.badgeColor = stored;
    dep.optional = o.value("optional").toBool();

    // A hand-written .sdzn can leave out the parts only a badge needs. Both are
    // derivable from the addon name, and a badge with no letters on it is worse
    // than one carrying the mod's initials.
    if (dep.displayName.isEmpty()) dep.displayName = dep.id;
    if (dep.shortName.isEmpty()) dep.shortName = shortNameFor(dep.displayName);
    return dep;
}

} // namespace

ScriptEntry *Project::script(const QString &id)
{
    for (ScriptEntry &s : scripts)
        if (s.id == id) return &s;
    return nullptr;
}

const ScriptEntry *Project::script(const QString &id) const
{
    for (const ScriptEntry &s : scripts)
        if (s.id == id) return &s;
    return nullptr;
}

ScriptEntry *Project::active()
{
    if (ScriptEntry *s = script(activeId)) return s;
    return scripts.isEmpty() ? nullptr : &scripts.first();
}

const ModDependency *Project::dependency(const QString &id) const
{
    if (id.isEmpty()) return nullptr;
    for (const ModDependency &d : dependencies)
        if (d.id == id) return &d;
    return nullptr;
}

Project newProject()
{
    Project p;
    p.name = QStringLiteral("Untitled");
    p.folders = {QStringLiteral("3_Game"), QStringLiteral("4_World"),
                 QStringLiteral("5_Mission")};
    ScriptEntry s;
    s.id = nextId(QStringLiteral("s"));
    s.name = QStringLiteral("MyItem");
    s.folder = QStringLiteral("4_World");
    s.graph = Graph{};
    s.graph.className = s.name;
    p.scripts.append(s);
    p.activeId = s.id;
    return p;
}

bool loadProject(const QString &path, Project &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(),
                                                         err.errorString());
        return false;
    }
    const QString file = QFileInfo(path).fileName();
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("%1 is not a project file: the top level "
                                           "is not a JSON object").arg(file);
        return false;
    }
    const QJsonObject root = doc.object();

    const QJsonValue scriptsValue = root.value("scripts");
    if (!scriptsValue.isUndefined() && !scriptsValue.isNull() && !scriptsValue.isArray()) {
        if (error) *error = QStringLiteral("%1 has a \"scripts\" entry that is not a "
                                           "list of scripts").arg(file);
        return false;
    }

    Project p;
    p.path = path;
    p.name = root.value("name").toString(QFileInfo(path).completeBaseName());
    for (const QJsonValue &v : root.value("folders").toArray())
        p.folders << v.toString();
    const QJsonArray scriptArray = scriptsValue.toArray();
    for (int i = 0; i < scriptArray.size(); ++i) {
        const QJsonValue v = scriptArray.at(i);
        // Refuse rather than substitute a default. A script that decodes to an
        // empty shell looks like an empty script in the editor, and saving over
        // it is what finally destroys the content that was there.
        if (!v.isObject()) {
            if (error) *error = QStringLiteral("%1 is damaged: script %2 is not an "
                                               "object").arg(file).arg(i + 1);
            return false;
        }
        const QJsonObject o = v.toObject();
        const QJsonValue graphValue = o.value("graph");
        if (!graphValue.isUndefined() && !graphValue.isNull() && !graphValue.isObject()) {
            if (error) *error = QStringLiteral("%1 is damaged: script %2 has a graph "
                                               "that is not an object")
                                    .arg(file, o.value("name").toString(
                                                   QString::number(i + 1)));
            return false;
        }
        ScriptEntry s;
        s.id = o.value("id").toString();
        s.name = o.value("name").toString();
        s.folder = o.value("folder").toString();
        s.graph = graphFromJson(graphValue.toObject());
        s.preamble = o.value("preamble").toString();
        const QString source = o.value("sourcePath").toString();
        if (!source.isEmpty()) {
            const QDir base(QFileInfo(path).absolutePath());
            s.sourcePath = QDir::cleanPath(base.absoluteFilePath(source));
        }
        for (auto it = o.begin(); it != o.end(); ++it) {
            static const QStringList known = {"id",         "name",     "folder",
                                              "graph",      "preamble", "sourcePath"};
            if (!known.contains(it.key())) s.extra.insert(it.key(), it.value());
        }
        p.scripts.append(s);
    }
    p.activeId = root.value("activeId").toString();
    p.modPrefix = root.value("modPrefix").toString();
    const QDir base(QFileInfo(path).absolutePath());
    const QString rel = root.value("modRoot").toString();
    if (!rel.isEmpty()) p.modRoot = QDir::cleanPath(base.absoluteFilePath(rel));

    // A dependency is not content: it is a pointer at another mod, and every
    // lookup against it (Project::dependency, ModIndex::dependencyOf) takes the
    // first match on the addon name. One with no name can never be found again
    // and one that repeats a name hides the entry in front of it, so neither is
    // kept quietly.
    QSet<QString> seenAddons;
    for (const QJsonValue &v : root.value("dependencies").toArray()) {
        if (!v.isObject()) {
            qWarning("sdzn: a dependency in %s is not an object; it is dropped",
                     qPrintable(file));
            continue;
        }
        const ModDependency dep = dependencyFromJson(v.toObject(), base);
        if (!dep.isValid()) {
            qWarning("sdzn: a dependency in %s names no addon; it is dropped",
                     qPrintable(file));
            continue;
        }
        if (seenAddons.contains(dep.id)) {
            qWarning("sdzn: %s lists the addon \"%s\" twice; the second one is dropped",
                     qPrintable(file), qPrintable(dep.id));
            continue;
        }
        seenAddons.insert(dep.id);
        p.dependencies.append(dep);
    }

    for (auto it = root.begin(); it != root.end(); ++it) {
        static const QStringList known = {"name", "folders", "scripts", "activeId",
                                          "modRoot", "modPrefix", "dependencies"};
        if (!known.contains(it.key())) p.extra.insert(it.key(), it.value());
    }
    if (p.scripts.isEmpty()) {
        if (error) *error = QStringLiteral("%1 contains no scripts").arg(file);
        return false;
    }

    repairIds(p.scripts, QStringLiteral("s"), "a script", file);
    for (ScriptEntry &s : p.scripts)
        repairGraph(s.graph, s.name.isEmpty() ? s.id : s.name);
    // activeId is a pointer into the script list, not content: one that names
    // nothing leaves the editor showing the first script while every id-keyed
    // operation on it (undo snapshots above all) looks for a script that is
    // not there.
    if (!p.script(p.activeId)) p.activeId = p.scripts.first().id;

    out = p;
    return true;
}

bool saveProject(const Project &project, const QString &path, QString *error)
{
    QJsonObject root;
    root.insert("name", project.name);
    QJsonArray folders;
    for (const QString &f : project.folders) folders.append(f);
    root.insert("folders", folders);

    // Every path in a .sdzn is relative to the file itself, so a project folder
    // can be moved or shared without breaking.
    const QDir base(QFileInfo(path).absolutePath());

    QJsonArray scripts;
    for (const ScriptEntry &s : project.scripts) {
        QJsonObject o;
        o.insert("id", s.id);
        o.insert("name", s.name);
        o.insert("folder", s.folder);
        o.insert("graph", graphToJson(s.graph));
        if (!s.preamble.isEmpty()) o.insert("preamble", s.preamble);
        if (!s.sourcePath.isEmpty())
            o.insert("sourcePath", base.relativeFilePath(s.sourcePath));
        for (auto it = s.extra.begin(); it != s.extra.end(); ++it)
            o.insert(it.key(), it.value());
        scripts.append(o);
    }
    root.insert("scripts", scripts);
    root.insert("activeId", project.activeId);
    if (!project.modPrefix.isEmpty()) root.insert("modPrefix", project.modPrefix);
    if (!project.modRoot.isEmpty())
        root.insert("modRoot", base.relativeFilePath(project.modRoot));
    if (!project.dependencies.isEmpty()) {
        QJsonArray deps;
        for (const ModDependency &d : project.dependencies) {
            if (!d.isValid()) continue;
            deps.append(dependencyToJson(d, base));
        }
        if (!deps.isEmpty()) root.insert("dependencies", deps);
    }
    for (auto it = project.extra.begin(); it != project.extra.end(); ++it)
        root.insert(it.key(), it.value());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("failed to save %1").arg(path);
        return false;
    }
    return true;
}
