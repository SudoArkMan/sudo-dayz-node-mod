// The projects opened before this one, and enough about each to describe it
// without opening it.
//
// The start page lists these, so the list has to answer "what is this project"
// off a cache rather than by loading twenty .sdzn files at launch. Each entry
// carries the counts it had at a file timestamp; a file whose timestamp still
// matches keeps its numbers, and only one that has actually changed is read
// again.
//
// A project whose file is gone stays in the list, marked. It has nearly always
// been moved rather than deleted, and a row that quietly disappears leaves the
// user with nothing to search for.
//
// Every mutator writes the store before it returns. The list is a few hundred
// bytes and the alternative is a caller that forgets, which costs the user the
// session's history to save a file write nobody would notice.
#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

struct RecentProject {
    QString path;      // absolute and cleaned, as it should be shown
    QString name;      // the project's own name out of the .sdzn
    QString modName;   // mod prefix, or the mod folder's name; empty when neither
    int scriptCount = 0;
    int nodeCount = 0;
    QDateTime lastOpened;
    // File timestamp the counts were read at. They describe that revision of the
    // file and no other, which is what keeps a stale number off the start page.
    QDateTime countsAt;
    // Set by RecentProjects::refresh: the file is not where it was.
    bool missing = false;

    bool isValid() const { return !path.isEmpty(); }
    bool countsKnown() const { return countsAt.isValid(); }
};

// Name, mod name and counts straight out of a .sdzn, without building a Project.
// The full loader repairs ids, resolves relative paths and rebuilds every graph,
// and none of that is needed to draw a row. `out.lastOpened` is left alone.
bool readProjectSummary(const QString &path, RecentProject &out,
                        QString *error = nullptr);

// One key per file on disk, for deciding whether two paths name the same
// project. Canonical when the file is there, cleaned when it is not, and case
// folded on Windows because the file system is.
QString recentKey(const QString &path);

// "just now", "12 minutes ago", "yesterday", then a date once the distance stops
// meaning anything. `now` is a parameter so the test does not have to wait.
QString relativeTime(const QDateTime &when,
                     const QDateTime &now = QDateTime::currentDateTime());

class RecentProjects {
public:
    // Twenty is past the point where anyone scrolls the list rather than using
    // Open, and small enough that a refresh stats it in one go.
    static constexpr int kDefaultCapacity = 20;

    // Defaults to defaultStorePath(). The test passes its own file so a run
    // never touches the user's real list.
    explicit RecentProjects(const QString &storePath = QString());

    static QString defaultStorePath();
    QString storePath() const { return m_storePath; }

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;

    const QVector<RecentProject> &entries() const { return m_entries; }
    int size() const { return m_entries.size(); }
    bool contains(const QString &path) const;
    // The entry for a path, or one with an empty path when there is none.
    RecentProject entry(const QString &path) const;

    // Records an open or a save: the project moves to the front, its counts are
    // read when the cache has nothing for this revision of the file, and the
    // list is capped. False when the file could not be read as a project, in
    // which case the entry is still recorded with whatever was already known
    // about it: an unreadable file is exactly the one the user wants to find
    // again.
    bool record(const QString &path);

    bool remove(const QString &path);
    // Drops every entry whose file has gone. Returns how many went, so the
    // caller can say so rather than leaving the list to change under the user.
    int removeMissing();
    void clear();

    // Stats every entry, marking the missing ones, and re-reads the counts of
    // any file that has changed since they were taken. Returns how many entries
    // are now missing.
    int refresh();

    int capacity() const { return m_capacity; }
    void setCapacity(int entries);

    // Where the Open dialog should start. The last folder a project was opened
    // from or saved to, which is not always the folder of the newest entry.
    QString lastFolder() const { return m_lastFolder; }
    void setLastFolder(const QString &folder);

private:
    int indexOf(const QString &path) const;
    void applyCapacity();

    QString m_storePath;
    QVector<RecentProject> m_entries;
    QString m_lastFolder;
    int m_capacity = kDefaultCapacity;
};
