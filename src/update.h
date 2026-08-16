// Which version this is, what changed in it, and whether a newer one has been
// published.
//
// Four separable things live here, and the split is what makes the whole of it
// testable. Ordering versions and reading a changelog are text work that has to
// be right whether or not the machine is online, so they are free functions
// with no transport behind them. The decision the app takes from a reply is a
// third free function, `evaluateFetch`, which turns "here is what came back"
// into "here is what the page should say"; every failure a request can have is
// an argument to it, so no test needs a socket. Only `UpdateCheck` touches the
// network, and it is a thin wrapper that fills in a FetchOutcome and hands it
// to that same function.
//
// Nothing here contacts anything until the user has said it may. The answer
// starts as "not asked" and is remembered, so an install where nobody answers
// never sends a request. This is a tool people install from a mod community and
// a phone home by default would be the wrong first impression.
#pragma once

#include <QDate>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QSettings;

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

// A semantic version, ordered the way semver.org orders them rather than the
// way QString::compare would. String comparison puts 0.10.0 below 0.9.0, which
// is the bug this whole struct exists to stop.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    // The dot separated identifiers after a '-'. Empty for a release, and a
    // release always outranks a pre-release of the same three numbers.
    QStringList pre;
    // What followed a '+'. Kept so a tag can be shown as it was written, and
    // ignored in ordering, which is what semver says to do with it.
    QString build;
    bool valid = false;

    bool isPrerelease() const { return !pre.isEmpty(); }
    // The version, not the tag: a leading 'v' belongs to the tag that carried
    // it and is not part of what is being compared.
    QString toString() const;
};

// Accepts what release tags really carry: a leading 'v' or 'V', one to three
// numeric components, a pre-release suffix and build metadata. A missing minor
// or patch reads as zero, so the tag `v2` is 2.0.0. Anything whose numeric part
// is not numeric comes back invalid rather than as 0.0.0, because a silent zero
// would make every real release look newer than it.
Version parseVersion(const QString &text);

// -1, 0 or 1. An invalid version sorts below every valid one, and two invalid
// ones are equal, so a tag nobody can parse never wins a comparison.
int compareVersions(const Version &a, const Version &b);
int compareVersions(const QString &a, const QString &b);

// True when `candidate` is a version, `current` is a version, and the first is
// the later of the two. False on anything unparseable in either, which is the
// safe direction: no news rather than false news.
bool isNewerVersion(const QString &candidate, const QString &current);

// ---------------------------------------------------------------------------
// The changelog
// ---------------------------------------------------------------------------

// One `## [0.2.0] - 2026-08-16` section of a Keep a Changelog file.
struct ChangelogEntry {
    QString heading;   // the version token as the file spells it, without brackets
    Version version;   // parsed; invalid for an Unreleased section
    QDate date;        // invalid when the heading carries no date
    bool unreleased = false;
    bool yanked = false;
    QString body;      // the markdown under the heading, trimmed

    bool isValid() const { return !heading.isEmpty(); }
};

// Every version section, in the order the file lists them. Headings inside a
// fenced code block are text, not headings, and are left alone. A file that
// parses to nothing comes back empty rather than as one entry holding the whole
// file, so a caller can tell "no changelog" from "a changelog with no releases".
QVector<ChangelogEntry> parseChangelog(const QString &text);

// The section for a version, matched on the parsed numbers rather than on the
// spelling, so `0.2.0` finds `[v0.2.0]`. An entry with an empty heading when
// there is none.
ChangelogEntry changelogEntryFor(const QVector<ChangelogEntry> &entries,
                                 const QString &version);

// A section body as plain lines for a panel: `### Added` becomes `Added:`, list
// markers are dropped, links keep their text and lose their target, and inline
// code and emphasis lose their markers. `maxLines` of 0 means all of them.
QStringList changelogLines(const QString &body, int maxLines = 0);

// CHANGELOG.md beside the executable, or a few levels up in a build tree, which
// is the same walk the start page uses for resources. Empty when there is none.
// `startDir` overrides the executable's folder, for a test.
QString findChangelog(const QString &startDir = QString());

// Reads a file as UTF-8. Empty with `error` set when it cannot be read.
QString readChangelog(const QString &path, QString *error = nullptr);

// ---------------------------------------------------------------------------
// Releases
// ---------------------------------------------------------------------------

// One published release, as much of it as the panel and the button need.
struct Release {
    QString tag;
    Version version;
    QString title;
    QString notes;         // the release body, markdown
    QString pageUrl;       // where "open the release page" goes
    QString assetName;     // the download that looks like a Windows install
    QString assetUrl;
    qint64 assetSize = 0;
    bool prerelease = false;
    bool draft = false;
    QDateTime published;

    bool isValid() const { return version.valid; }
};

// The array GitHub answers `/repos/<owner>/<repo>/releases` with. Refuses
// anything that is not an array of objects rather than returning half a list,
// and skips a single entry it cannot read rather than failing the whole reply.
QVector<Release> parseReleases(const QByteArray &json, QString *error = nullptr);

// The newest release that beats `currentVersion`, or an invalid Release when
// there is none. Drafts never count. Pre-releases count only when asked for,
// because somebody running a release build has not opted into release
// candidates by installing the app.
Release newestRelease(const QVector<Release> &releases, const QString &currentVersion,
                      bool includePrereleases = false);

// ---------------------------------------------------------------------------
// The check, with the transport taken out
// ---------------------------------------------------------------------------

// What a request came back with. `completed` is false when nothing came back at
// all, which is the offline case and the timeout case.
struct FetchOutcome {
    bool completed = false;
    int httpStatus = 0;
    QByteArray body;
    QString transportError;   // Qt's own words, when there are any
    QDateTime at;
};

struct UpdateOutcome {
    enum Status {
        NotChecked,       // nobody has asked yet, or consent has not been given
        Checking,
        UpToDate,
        NoReleases,       // the repository is there and has published nothing
        UpdateAvailable,
        Failed,
    };

    Status status = NotChecked;
    Release release;      // only meaningful for UpdateAvailable
    // Why, in the app's own words. Kept for a tooltip and a log line, and never
    // put in front of somebody who did not ask for a check.
    QString detail;
    QDateTime checkedAt;

    bool hasUpdate() const { return status == UpdateAvailable && release.isValid(); }
};

// The whole of the decision. Every way the request can go wrong is a value of
// `fetch`, so the failure paths are driven in a test with no network at all.
// `skipVersion` is a release the user has already said they do not want; it is
// reported as up to date rather than as an update.
UpdateOutcome evaluateFetch(const FetchOutcome &fetch, const QString &currentVersion,
                            bool includePrereleases = false,
                            const QString &skipVersion = QString());

// ---------------------------------------------------------------------------
// What the user has agreed to
// ---------------------------------------------------------------------------

// Not asked is a real state and the one every install starts in. It is not the
// same as declined: declined means leave it alone, not asked means ask.
enum class UpdateConsent { NotAsked, Allowed, Declined };

// Where the check looks when nothing has been configured. The repository does
// not exist yet, which is exactly why these are settings and not constants in
// the middle of a URL.
QString defaultUpdateOwner();
QString defaultUpdateRepository();

// The remembered answer, and the rest of what the check needs between runs.
class UpdatePreferences {
public:
    // An empty path uses the application's own settings, which is what the app
    // does. A test passes a file of its own so a run never touches them.
    explicit UpdatePreferences(const QString &iniPath = QString());
    ~UpdatePreferences();

    UpdatePreferences(const UpdatePreferences &) = delete;
    UpdatePreferences &operator=(const UpdatePreferences &) = delete;

    UpdateConsent consent() const;
    void setConsent(UpdateConsent consent);

    QString owner() const;
    QString repository() const;
    void setRepository(const QString &owner, const QString &repository);

    QDateTime lastChecked() const;
    void setLastChecked(const QDateTime &when);

    // A version the user has said they do not want to hear about again.
    QString skippedVersion() const;
    void setSkippedVersion(const QString &version);

    // The version the page last showed. Different from the running one means
    // this is the first run after an update, which is when the notes matter.
    QString lastSeenVersion() const;
    void setLastSeenVersion(const QString &version);

    bool includePrereleases() const;
    void setIncludePrereleases(bool on);

    // Consent given and a day since the last one. Once a day is a courtesy;
    // a tool that asks sixty times an hour is a tool that gets rate limited.
    bool dueForCheck(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;

    void flush();

private:
    QSettings *m_settings = nullptr;
};

// ---------------------------------------------------------------------------
// The request itself
// ---------------------------------------------------------------------------

// The only part that opens a socket. A started check reports through
// `finished` exactly once however it ends, and it is safe to delete while one
// is in flight.
class UpdateCheck : public QObject {
    Q_OBJECT
public:
    explicit UpdateCheck(QObject *parent = nullptr);
    ~UpdateCheck() override;

    void setRepository(const QString &owner, const QString &repository);
    void setCurrentVersion(const QString &version);
    void setIncludePrereleases(bool on);
    void setSkippedVersion(const QString &version);
    // Ten seconds by default. A check nobody asked to watch has no business
    // holding a socket open longer than that.
    void setTimeout(int milliseconds);

    QUrl endpoint() const;
    bool running() const;

public slots:
    // Does nothing when a check is already in flight, so a double click on the
    // button is one request.
    void start();
    // Drops the request without emitting anything. The caller asked for it to
    // stop, and a result nobody is waiting for is what this feature exists to
    // avoid producing.
    void cancel();

signals:
    void finished(const UpdateOutcome &outcome);

private:
    void report(const FetchOutcome &fetch);

    struct Private;
    Private *d;
};
