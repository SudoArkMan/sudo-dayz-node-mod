#include "recentprojects.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

namespace {

// Bumped when the shape below changes in a way an older file cannot be read
// into. A file from a newer build is left alone rather than rewritten, so
// running an older build for one session does not cost the list.
constexpr int kStoreVersion = 1;

QString cleanAbsolute(const QString &path)
{
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString isoOrEmpty(const QDateTime &when)
{
    return when.isValid() ? when.toUTC().toString(Qt::ISODate) : QString();
}

QDateTime fromIso(const QString &text)
{
    if (text.isEmpty()) return {};
    QDateTime when = QDateTime::fromString(text, Qt::ISODate);
    // Written in UTC, so a value that came back without a zone is UTC too.
    // Reading it as local time would move every timestamp by the offset.
    if (when.isValid() && when.timeRepresentation() == QTimeZone::LocalTime)
        when.setTimeZone(QTimeZone::UTC);
    return when.toLocalTime();
}

// The mod a project belongs to, spelled the way the user would say it. The
// prefix is the name that appears in config.cpp and on the PBO, so it wins over
// the folder when the file carries both.
QString modNameFrom(const QJsonObject &root, const QString &projectPath)
{
    const QString prefix = root.value(QLatin1String("modPrefix")).toString().trimmed();
    if (!prefix.isEmpty()) return prefix;
    const QString rel = root.value(QLatin1String("modRoot")).toString().trimmed();
    if (rel.isEmpty()) return {};
    const QDir base(QFileInfo(projectPath).absolutePath());
    return QDir(QDir::cleanPath(base.absoluteFilePath(rel))).dirName();
}

} // namespace

QString recentKey(const QString &path)
{
    if (path.isEmpty()) return {};
    QString resolved = QFileInfo(path).canonicalFilePath();
    // A file that is not there has no canonical path, and a missing entry still
    // has to compare equal to itself so the user can remove it.
    if (resolved.isEmpty()) resolved = cleanAbsolute(path);
#ifdef Q_OS_WIN
    return resolved.toLower();
#else
    return resolved;
#endif
}

bool readProjectSummary(const QString &path, RecentProject &out, QString *error)
{
    const QFileInfo info(path);
    out.path = cleanAbsolute(path);
    out.missing = !info.isFile();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1")
                                .arg(QDir::toNativeSeparators(out.path));
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("%1 is not a project file: %2")
                         .arg(info.fileName(),
                              parse.error == QJsonParseError::NoError
                                  ? QStringLiteral("the top level is not a JSON object")
                                  : parse.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    out.name = root.value(QLatin1String("name")).toString().trimmed();
    if (out.name.isEmpty()) out.name = info.completeBaseName();
    out.modName = modNameFrom(root, path);

    const QJsonArray scripts = root.value(QLatin1String("scripts")).toArray();
    out.scriptCount = scripts.size();
    out.nodeCount = 0;
    for (const QJsonValue &v : scripts) {
        out.nodeCount += v.toObject()
                             .value(QLatin1String("graph")).toObject()
                             .value(QLatin1String("nodes")).toArray()
                             .size();
    }
    // Stamped after the read rather than before it: a file rewritten while this
    // was parsing would otherwise be cached under the timestamp of a revision
    // these numbers do not describe.
    out.countsAt = QFileInfo(path).lastModified();
    return true;
}

QString relativeTime(const QDateTime &when, const QDateTime &now)
{
    if (!when.isValid()) return QStringLiteral("never");

    const qint64 seconds = when.secsTo(now);
    // A clock that went backwards, or a file touched by another machine. The
    // distance means nothing, so it is not reported as one.
    if (seconds < -60) return when.toString(QStringLiteral("d MMM yyyy"));
    if (seconds < 60) return QStringLiteral("just now");

    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return QStringLiteral("%1 minute%2 ago").arg(minutes)
            .arg(minutes == 1 ? QString() : QStringLiteral("s"));

    const qint64 hours = minutes / 60;
    if (hours < 24)
        return QStringLiteral("%1 hour%2 ago").arg(hours)
            .arg(hours == 1 ? QString() : QStringLiteral("s"));

    // Counted in calendar days, not in 24 hour blocks: something opened last
    // night at eleven is "yesterday" at nine this morning, and calling it
    // "10 hours ago" is the answer to a question nobody asked.
    const qint64 days = when.date().daysTo(now.date());
    if (days <= 1) return QStringLiteral("yesterday");
    if (days < 7) return QStringLiteral("%1 days ago").arg(days);
    if (days < 30) {
        const qint64 weeks = days / 7;
        return QStringLiteral("%1 week%2 ago").arg(weeks)
            .arg(weeks == 1 ? QString() : QStringLiteral("s"));
    }
    return when.toString(QStringLiteral("d MMM yyyy"));
}

RecentProjects::RecentProjects(const QString &storePath)
    : m_storePath(storePath.isEmpty() ? defaultStorePath() : storePath)
{
}

QString RecentProjects::defaultStorePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // Only when the platform has no answer at all. Writing the list beside the
    // executable would put it in Program Files on an installed build.
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.sudo-dayz-node-mod");
    return QDir::cleanPath(dir + QStringLiteral("/recent-projects.json"));
}

int RecentProjects::indexOf(const QString &path) const
{
    const QString key = recentKey(path);
    if (key.isEmpty()) return -1;
    for (int i = 0; i < m_entries.size(); ++i)
        if (recentKey(m_entries.at(i).path) == key) return i;
    return -1;
}

bool RecentProjects::contains(const QString &path) const
{
    return indexOf(path) >= 0;
}

RecentProject RecentProjects::entry(const QString &path) const
{
    const int at = indexOf(path);
    return at < 0 ? RecentProject{} : m_entries.at(at);
}

void RecentProjects::applyCapacity()
{
    while (m_entries.size() > m_capacity) m_entries.removeLast();
}

void RecentProjects::setCapacity(int entries)
{
    m_capacity = qMax(1, entries);
    applyCapacity();
    save();
}

bool RecentProjects::record(const QString &path)
{
    const QString clean = cleanAbsolute(path);
    if (clean.isEmpty()) return false;

    const int at = indexOf(clean);
    RecentProject entry = at < 0 ? RecentProject{} : m_entries.at(at);
    entry.path = clean;

    const QFileInfo info(clean);
    const bool readable = info.isFile();
    bool ok = true;
    // The counts are only re-read when this revision of the file is not the one
    // behind them. Recording the same project twice in a session costs a stat.
    if (readable && (!entry.countsKnown() || entry.countsAt != info.lastModified()))
        ok = readProjectSummary(clean, entry);
    entry.missing = !readable;
    if (entry.name.isEmpty()) entry.name = info.completeBaseName();

    entry.lastOpened = QDateTime::currentDateTime();
    if (at >= 0) m_entries.removeAt(at);
    m_entries.prepend(entry);
    applyCapacity();
    // Where the Open dialog will start next time. Set here rather than through
    // setLastFolder so the list and the folder reach disk in one write.
    m_lastFolder = info.absolutePath();
    save();
    return readable && ok;
}

bool RecentProjects::remove(const QString &path)
{
    const int at = indexOf(path);
    if (at < 0) return false;
    m_entries.removeAt(at);
    save();
    return true;
}

int RecentProjects::removeMissing()
{
    int gone = 0;
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (!m_entries.at(i).missing) continue;
        m_entries.removeAt(i);
        ++gone;
    }
    if (gone > 0) save();
    return gone;
}

void RecentProjects::clear()
{
    m_entries.clear();
    save();
}

int RecentProjects::refresh()
{
    int missing = 0;
    bool changed = false;
    for (RecentProject &entry : m_entries) {
        const QFileInfo info(entry.path);
        const bool wasMissing = entry.missing;
        entry.missing = !info.isFile();
        if (entry.missing) {
            ++missing;
            changed = changed || !wasMissing;
            continue;
        }
        // Edited outside the app, or by an export that rewrote it. One file is
        // parsed here, not the whole list.
        if (!entry.countsKnown() || entry.countsAt != info.lastModified()) {
            RecentProject fresh = entry;
            if (readProjectSummary(entry.path, fresh)) {
                fresh.lastOpened = entry.lastOpened;
                entry = fresh;
                changed = true;
            }
        }
        changed = changed || wasMissing;
    }
    if (changed) save();
    return missing;
}

void RecentProjects::setLastFolder(const QString &folder)
{
    const QString clean = folder.isEmpty() ? QString() : QDir::cleanPath(folder);
    m_lastFolder = clean;
    save();
}

bool RecentProjects::load(QString *error)
{
    m_entries.clear();

    QFile file(m_storePath);
    if (!file.exists()) {
        // No list yet is the normal first run, not a failure.
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1")
                                .arg(QDir::toNativeSeparators(m_storePath));
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("%1 is damaged: %2")
                         .arg(QFileInfo(m_storePath).fileName(), parse.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    m_lastFolder = root.value(QLatin1String("lastFolder")).toString();
    const int capacity = root.value(QLatin1String("capacity")).toInt(m_capacity);
    if (capacity > 0) m_capacity = capacity;

    for (const QJsonValue &v : root.value(QLatin1String("projects")).toArray()) {
        const QJsonObject o = v.toObject();
        RecentProject entry;
        entry.path = cleanAbsolute(o.value(QLatin1String("path")).toString());
        if (entry.path.isEmpty()) continue;
        // A store written by hand, or one that survived a crash mid-write, can
        // repeat a path. The second copy would shadow the first in every lookup.
        if (contains(entry.path)) continue;
        entry.name = o.value(QLatin1String("name")).toString();
        entry.modName = o.value(QLatin1String("modName")).toString();
        entry.scriptCount = o.value(QLatin1String("scripts")).toInt();
        entry.nodeCount = o.value(QLatin1String("nodes")).toInt();
        entry.lastOpened = fromIso(o.value(QLatin1String("lastOpened")).toString());
        entry.countsAt = fromIso(o.value(QLatin1String("countsAt")).toString());
        if (entry.name.isEmpty())
            entry.name = QFileInfo(entry.path).completeBaseName();
        m_entries.append(entry);
    }
    applyCapacity();
    return true;
}

bool RecentProjects::save(QString *error) const
{
    QJsonArray projects;
    for (const RecentProject &entry : m_entries) {
        QJsonObject o;
        o.insert(QLatin1String("path"), entry.path);
        o.insert(QLatin1String("name"), entry.name);
        if (!entry.modName.isEmpty()) o.insert(QLatin1String("modName"), entry.modName);
        o.insert(QLatin1String("scripts"), entry.scriptCount);
        o.insert(QLatin1String("nodes"), entry.nodeCount);
        o.insert(QLatin1String("lastOpened"), isoOrEmpty(entry.lastOpened));
        // The stamp is what makes the counts trustworthy, so an entry without
        // one is written without its numbers rather than with numbers that
        // nothing can date.
        if (entry.countsKnown())
            o.insert(QLatin1String("countsAt"), isoOrEmpty(entry.countsAt));
        projects.append(o);
    }

    QJsonObject root;
    root.insert(QLatin1String("version"), kStoreVersion);
    root.insert(QLatin1String("capacity"), m_capacity);
    if (!m_lastFolder.isEmpty()) root.insert(QLatin1String("lastFolder"), m_lastFolder);
    root.insert(QLatin1String("projects"), projects);

    QDir().mkpath(QFileInfo(m_storePath).absolutePath());
    QSaveFile out(m_storePath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1")
                                .arg(QDir::toNativeSeparators(m_storePath));
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (error) *error = QStringLiteral("failed to save %1")
                                .arg(QDir::toNativeSeparators(m_storePath));
        return false;
    }
    return true;
}
