// Version ordering, the changelog, and every way a check of GitHub can end.
//
// Not one test in this file opens a socket, and that is a property of the
// design rather than a promise made here: the transport is a FetchOutcome value
// handed to evaluateFetch, so offline, timed out, rate limited, a 404, an empty
// list and a reply full of rubbish are all just arguments. The one class that
// does own a socket is only asked what URL it would use.
//
// Version ordering gets the most of this file on purpose. It is the part
// everybody writes with QString::compare and the part where that is wrong: it
// puts 0.10.0 below 0.9.0, and a release tag carries a 'v' and sometimes a
// pre-release suffix on top of that.
//
// The offscreen platform is forced here rather than left to the caller's
// environment, so a plain run needs no display:
//   ./tests/updatetest ../resources [--shots <dir>]
//
// A run asking for pictures is the exception and leaves the platform alone. The
// offscreen plugin carries no font database, so every glyph it draws is an
// empty box: fine for measuring where a panel ends, useless for looking at. The
// page is given WA_DontShowOnScreen either way, so nothing appears in front of
// whoever is at the machine.
#include "recentprojects.h"
#include "theme.h"
#include "update.h"
#include "version.h"
#include "widgets/startpage.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

static int failures = 0;
static QTextStream *out = nullptr;

static void line(const QString &text)
{
    *out << text << Qt::endl;
    out->flush();
}

static void check(bool ok, const QString &what)
{
    line((ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) + what);
    if (!ok) failures++;
}

static void equal(const QString &got, const QString &want, const QString &what)
{
    const bool ok = got == want;
    QString text = (ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) + what;
    if (!ok) text += QStringLiteral(" (wanted [%1], got [%2])").arg(want, got);
    line(text);
    if (!ok) failures++;
}

static void equal(int got, int want, const QString &what)
{
    equal(QString::number(got), QString::number(want), what);
}

// a is strictly older than b, both ways round, and neither is equal to itself
// by accident.
static void ordered(const QString &older, const QString &newer)
{
    const bool forward = compareVersions(older, newer) < 0;
    const bool backward = compareVersions(newer, older) > 0;
    const bool newerWins = isNewerVersion(newer, older) && !isNewerVersion(older, newer);
    check(forward && backward && newerWins,
          QStringLiteral("%1 is older than %2").arg(older, newer));
}

static void same(const QString &a, const QString &b)
{
    check(compareVersions(a, b) == 0,
          QStringLiteral("%1 and %2 are the same version").arg(a, b));
}

static QString statusName(UpdateOutcome::Status status)
{
    switch (status) {
    case UpdateOutcome::NotChecked: return QStringLiteral("not checked");
    case UpdateOutcome::Checking: return QStringLiteral("checking");
    case UpdateOutcome::UpToDate: return QStringLiteral("up to date");
    case UpdateOutcome::NoReleases: return QStringLiteral("no releases");
    case UpdateOutcome::UpdateAvailable: return QStringLiteral("update available");
    case UpdateOutcome::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

static void statusIs(const UpdateOutcome &outcome, UpdateOutcome::Status want,
                     const QString &what)
{
    equal(statusName(outcome.status), statusName(want), what);
}

// A release as GitHub spells one, with only the fields the app reads.
static QJsonObject release(const QString &tag, bool draft, bool prerelease,
                           const QString &body = QString(),
                           const QStringList &assets = QStringList())
{
    QJsonArray assetArray;
    for (const QString &name : assets) {
        QJsonObject asset;
        asset.insert("name", name);
        asset.insert("browser_download_url",
                     QStringLiteral("https://example.invalid/%1").arg(name));
        asset.insert("size", 44040192);
        assetArray.append(asset);
    }
    QJsonObject object;
    object.insert("tag_name", tag);
    object.insert("name", QStringLiteral("SUDO DayZ Node Mod %1").arg(tag));
    object.insert("body", body);
    object.insert("html_url",
                  QStringLiteral("https://example.invalid/releases/%1").arg(tag));
    object.insert("draft", draft);
    object.insert("prerelease", prerelease);
    object.insert("published_at", QStringLiteral("2026-08-14T09:12:00Z"));
    object.insert("assets", assetArray);
    return object;
}

static QByteArray body(const QVector<QJsonObject> &releases)
{
    QJsonArray array;
    for (const QJsonObject &object : releases) array.append(object);
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

// A reply that arrived, with a status and a payload.
static FetchOutcome answered(int status, const QByteArray &payload)
{
    FetchOutcome fetch;
    fetch.completed = true;
    fetch.httpStatus = status;
    fetch.body = payload;
    fetch.at = QDateTime::currentDateTimeUtc();
    return fetch;
}

// A reply that never arrived.
static FetchOutcome silence(const QString &reason)
{
    FetchOutcome fetch;
    fetch.completed = false;
    fetch.transportError = reason;
    fetch.at = QDateTime::currentDateTimeUtc();
    return fetch;
}

static const char *kChangelog = R"md(# Changelog

All notable changes to this project are documented here.
The format follows Keep a Changelog, and this project uses semantic versioning.

## [Unreleased]

### Added
- Nothing yet.

## [0.2.0] - 2026-08-16

### Added
- A **what is new** panel on the start page, with the notes for this build.
- An update check that asks [GitHub](https://example.invalid) once a day, and
  only once you have said it may.

### Fixed
- Version ordering no longer compares tags as strings, so `0.10.0` is correctly
  newer than `0.9.0`.

## [0.1.0] - 2026-07-02

### Added
- First build: the graph editor, the catalogue and the code view.

An example of a heading that is not one:

```markdown
## [9.9.9] - 2099-01-01
```

## [0.0.9] - 2026-06-30 [YANKED]

### Removed
- Pulled: the generated script did not compile.
)md";

// A .sdzn small enough to write by hand, so the recent list on the page has
// rows with real files behind them.
static bool writeProject(const QString &path, const QString &name, int scripts)
{
    QJsonArray scriptArray;
    for (int s = 0; s < scripts; ++s) {
        QJsonObject graph;
        graph.insert("className", QStringLiteral("%1_%2").arg(name).arg(s + 1));
        graph.insert("module", "4_World");
        graph.insert("nodes", QJsonArray());
        graph.insert("edges", QJsonArray());
        QJsonObject script;
        script.insert("id", QStringLiteral("s%1").arg(s + 1));
        script.insert("name", QStringLiteral("%1_%2").arg(name).arg(s + 1));
        script.insert("folder", "4_World");
        script.insert("graph", graph);
        scriptArray.append(script);
    }
    QJsonObject root;
    root.insert("name", name);
    root.insert("folders", QJsonArray({QStringLiteral("4_World")}));
    root.insert("scripts", scriptArray);
    root.insert("activeId", "s1");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.error() == QFileDevice::NoError;
}

int main(int argc, char *argv[])
{
    // Read off argv rather than off QCoreApplication::arguments, because the
    // decision has to be made before the QApplication that reads them exists.
    bool wantsShots = false;
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--shots") == 0) wantsShots = true;
    if (!wantsShots && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SUDO DayZ Node Mod"));
    QCoreApplication::setOrganizationName(QStringLiteral("SUDO"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QTextStream stream(stdout);
    out = &stream;

    QTemporaryDir temp;
    if (!temp.isValid()) {
        line(QStringLiteral("could not make a temporary folder"));
        return 1;
    }
    const QDir root(temp.path());

    // ---- reading a version -------------------------------------------------
    line(QStringLiteral("parsing a version"));
    {
        const Version plain = parseVersion(QStringLiteral("1.2.3"));
        check(plain.valid, QStringLiteral("a plain version parses"));
        equal(plain.major, 1, QStringLiteral("major"));
        equal(plain.minor, 2, QStringLiteral("minor"));
        equal(plain.patch, 3, QStringLiteral("patch"));
        check(!plain.isPrerelease(), QStringLiteral("and is not a pre-release"));

        const Version tagged = parseVersion(QStringLiteral("v0.10.0"));
        check(tagged.valid, QStringLiteral("a v prefix is a tag's, not the version's"));
        equal(tagged.toString(), QStringLiteral("0.10.0"),
              QStringLiteral("and does not survive into the version"));
        check(parseVersion(QStringLiteral("V1.0.0")).valid,
              QStringLiteral("an upper case V too"));

        const Version pre = parseVersion(QStringLiteral("v1.0.0-rc.2"));
        check(pre.valid && pre.isPrerelease(),
              QStringLiteral("a pre-release suffix parses"));
        equal(pre.pre.join(QStringLiteral(".")), QStringLiteral("rc.2"),
              QStringLiteral("with its identifiers kept apart"));
        equal(pre.toString(), QStringLiteral("1.0.0-rc.2"),
              QStringLiteral("and comes back the way it went in"));

        const Version built = parseVersion(QStringLiteral("1.0.0-beta+exp.sha.5114f85"));
        check(built.valid, QStringLiteral("build metadata parses"));
        equal(built.build, QStringLiteral("exp.sha.5114f85"),
              QStringLiteral("and is kept"));

        // Tags in the wild are not always three numbers.
        const Version two = parseVersion(QStringLiteral("v2.1"));
        check(two.valid, QStringLiteral("a two part tag parses"));
        equal(two.toString(), QStringLiteral("2.1.0"),
              QStringLiteral("with the missing part reading as zero"));
        equal(parseVersion(QStringLiteral("v3")).toString(), QStringLiteral("3.0.0"),
              QStringLiteral("and so does a one part tag"));

        check(parseVersion(QStringLiteral("  v1.2.3  ")).valid,
              QStringLiteral("surrounding space is trimmed"));

        // What must not parse. A silent 0.0.0 here would make every release on
        // earth look newer than the running build.
        const QStringList refused = {
            QString(),
            QStringLiteral("v"),
            QStringLiteral("latest"),
            QStringLiteral("1.2.x"),
            QStringLiteral("1..2"),
            QStringLiteral("1.2.3.4"),
            QStringLiteral("nightly-2026-08-16"),
            QStringLiteral("1.2.3-"),
            QStringLiteral("1.2.3-rc..1"),
            QStringLiteral("1.2.3+"),
            QStringLiteral("99999999999.0.0"),
        };
        bool allRefused = true;
        for (const QString &text : refused)
            allRefused = allRefused && !parseVersion(text).valid;
        check(allRefused, QStringLiteral("nothing unparseable comes back as 0.0.0"));
        check(!isNewerVersion(QStringLiteral("latest"), QStringLiteral("0.1.0")),
              QStringLiteral("and an unreadable tag never wins a comparison"));
        check(!isNewerVersion(QStringLiteral("9.9.9"), QStringLiteral("latest")),
              QStringLiteral("nor does anything against an unreadable current"));
    }

    // ---- ordering ----------------------------------------------------------
    line(QStringLiteral("\nordering versions"));
    {
        // The one string comparison gets wrong, and the reason this exists.
        ordered(QStringLiteral("0.9.0"), QStringLiteral("0.10.0"));
        ordered(QStringLiteral("0.9.9"), QStringLiteral("0.10.0"));
        ordered(QStringLiteral("1.9.0"), QStringLiteral("1.10.0"));
        ordered(QStringLiteral("1.0.9"), QStringLiteral("1.0.10"));
        ordered(QStringLiteral("0.99.99"), QStringLiteral("1.0.0"));
        ordered(QStringLiteral("1.2.3"), QStringLiteral("2.0.0"));
        ordered(QStringLiteral("v0.9.0"), QStringLiteral("v0.10.0"));

        // A release beats its own pre-releases, which is the half of the rule
        // that reads backwards if you sort the strings.
        ordered(QStringLiteral("1.0.0-rc.1"), QStringLiteral("1.0.0"));
        ordered(QStringLiteral("1.0.0-alpha"), QStringLiteral("1.0.0-alpha.1"));
        ordered(QStringLiteral("1.0.0-alpha.1"), QStringLiteral("1.0.0-alpha.beta"));
        ordered(QStringLiteral("1.0.0-alpha.beta"), QStringLiteral("1.0.0-beta"));
        ordered(QStringLiteral("1.0.0-beta"), QStringLiteral("1.0.0-beta.2"));
        // The numeric one: eleven is after two, however they sort as text.
        ordered(QStringLiteral("1.0.0-beta.2"), QStringLiteral("1.0.0-beta.11"));
        ordered(QStringLiteral("1.0.0-beta.11"), QStringLiteral("1.0.0-rc.1"));
        ordered(QStringLiteral("1.0.0-rc.9"), QStringLiteral("1.0.0-rc.10"));
        ordered(QStringLiteral("0.1.0-alpha"), QStringLiteral("0.1.0"));

        same(QStringLiteral("1.0.0"), QStringLiteral("v1.0.0"));
        same(QStringLiteral("1.0.0+build.1"), QStringLiteral("1.0.0+build.2"));
        same(QStringLiteral("1.0.0-rc.1+a"), QStringLiteral("1.0.0-rc.1+b"));
        same(QStringLiteral("v2.1"), QStringLiteral("2.1.0"));
        same(QStringLiteral("1.0.0-rc.007"), QStringLiteral("1.0.0-rc.7"));

        // Two versions nothing can read are equal, and either loses to one that
        // can be read, so a junk tag never sorts above a real release.
        check(compareVersions(QStringLiteral("junk"), QStringLiteral("nonsense")) == 0,
              QStringLiteral("two unreadable versions are equal"));
        check(compareVersions(QStringLiteral("junk"), QStringLiteral("0.0.1")) < 0,
              QStringLiteral("and an unreadable one sorts below a real one"));
    }

    // ---- the changelog -----------------------------------------------------
    line(QStringLiteral("\nreading the changelog"));
    const QVector<ChangelogEntry> entries =
        parseChangelog(QString::fromUtf8(kChangelog));
    {
        // Unreleased, 0.2.0, 0.1.0 and the yanked 0.0.9. The fifth heading in
        // the file sits inside a code fence and is a line of an example.
        equal(entries.size(), 4, QStringLiteral("four sections, and not the one "
                                                "inside the code fence"));
        check(entries.at(0).unreleased,
              QStringLiteral("the first is Unreleased"));
        check(!entries.at(0).version.valid,
              QStringLiteral("which carries no version to compare"));

        const ChangelogEntry second = entries.at(1);
        equal(second.heading, QStringLiteral("0.2.0"),
              QStringLiteral("the bracket is markdown, not part of the version"));
        equal(second.date.toString(Qt::ISODate), QStringLiteral("2026-08-16"),
              QStringLiteral("the date after the dash is read"));
        check(second.body.contains(QStringLiteral("what is new")),
              QStringLiteral("the body is the text under the heading"));
        check(!second.body.contains(QStringLiteral("First build")),
              QStringLiteral("and stops at the next heading"));

        bool ninetyNine = false;
        for (const ChangelogEntry &entry : entries)
            ninetyNine = ninetyNine || entry.heading == QStringLiteral("9.9.9");
        check(!ninetyNine,
              QStringLiteral("a heading inside a fence is a line of an example"));

        const ChangelogEntry yanked = entries.last();
        equal(yanked.heading, QStringLiteral("0.0.9"),
              QStringLiteral("a yanked release still names its version"));
        check(yanked.yanked, QStringLiteral("and is marked as pulled"));
        check(yanked.date.isValid(),
              QStringLiteral("with its date read past the marker"));

        // Matched on the numbers, so the spelling in the file does not have to
        // agree with the spelling in the binary.
        check(changelogEntryFor(entries, QStringLiteral("0.2.0")).isValid(),
              QStringLiteral("the running version finds its section"));
        check(changelogEntryFor(entries, QStringLiteral("v0.2.0")).isValid(),
              QStringLiteral("with or without the tag's v"));
        check(!changelogEntryFor(entries, QStringLiteral("0.3.0")).isValid(),
              QStringLiteral("a version with no section comes back empty"));
        check(!changelogEntryFor(entries, QStringLiteral("nonsense")).isValid(),
              QStringLiteral("and so does a version nothing can read"));

        check(parseChangelog(QString()).isEmpty(),
              QStringLiteral("an empty file parses to nothing"));
        check(parseChangelog(QStringLiteral("# Changelog\n\nNothing here yet.\n"))
                  .isEmpty(),
              QStringLiteral("so does a file with no version sections"));
        check(parseChangelog(QStringLiteral("## Notes\n\nProse.\n")).isEmpty(),
              QStringLiteral("a heading that is not a version is not a release"));
    }

    // ---- the changelog as lines a panel can draw ---------------------------
    line(QStringLiteral("\nthe notes as lines"));
    {
        const ChangelogEntry entry = changelogEntryFor(entries, QStringLiteral("0.2.0"));
        const QStringList lines = changelogLines(entry.body);
        // Two sections and three items. Two of those items are wrapped over two
        // lines in the file, which is what a changelog written by a person looks
        // like and what turns three items into five if nothing folds them.
        equal(lines.size(), 5, QStringLiteral("two sections and three items"));
        equal(lines.first(), QStringLiteral("Added:"),
              QStringLiteral("a section reads as a label"));
        check(lines.at(2).endsWith(QStringLiteral("once you have said it may.")),
              QStringLiteral("an item wrapped in the file is still one item"));
        check(lines.at(4).contains(QStringLiteral("newer than 0.9.0")),
              QStringLiteral("and so is one wrapped under a later section"));

        bool markers = false;
        bool markup = false;
        for (const QString &text : lines) {
            markers = markers || text.startsWith(QStringLiteral("- "))
                      || text.startsWith(QStringLiteral("#"));
            markup = markup || text.contains(QStringLiteral("**"))
                     || text.contains(QLatin1Char('`'))
                     || text.contains(QStringLiteral("]("));
        }
        check(!markers, QStringLiteral("list markers and hashes are gone"));
        check(!markup, QStringLiteral("bold, code fences and link targets too"));

        bool linkText = false;
        bool codeText = false;
        for (const QString &text : lines) {
            linkText = linkText || text.contains(QStringLiteral("GitHub"));
            codeText = codeText || text.contains(QStringLiteral("0.10.0"));
        }
        check(linkText, QStringLiteral("a link keeps the words it was made of"));
        check(codeText, QStringLiteral("and inline code keeps what it wrapped"));

        equal(changelogLines(entry.body, 3).size(), 3,
              QStringLiteral("the cap is a cap"));
        check(changelogLines(entry.body, 3).first() == lines.first(),
              QStringLiteral("and takes them from the top"));
        // Four lines of this body is "Added:", two items and "Fixed:", and a
        // section heading with nothing under it reads as a line lost.
        const QStringList capped = changelogLines(entry.body, 4);
        check(!capped.isEmpty() && !capped.last().endsWith(QLatin1Char(':')),
              QStringLiteral("a cap never lands on a section with nothing in it"));
        check(changelogLines(QString()).isEmpty(),
              QStringLiteral("an empty body draws nothing"));
    }

    // ---- finding and reading the file --------------------------------------
    line(QStringLiteral("\nthe file on disk"));
    {
        const QString nested = root.filePath(QStringLiteral("tree/build/cli"));
        QDir().mkpath(nested);
        const QString path = root.filePath(QStringLiteral("tree/CHANGELOG.md"));
        QFile file(path);
        check(file.open(QIODevice::WriteOnly), QStringLiteral("a changelog is written"));
        file.write(kChangelog);
        file.close();

        equal(QDir::cleanPath(findChangelog(nested)), QDir::cleanPath(path),
              QStringLiteral("it is found from a build folder a few levels down"));
        check(findChangelog(root.filePath(QStringLiteral("tree/build"))) == path
                  || QFileInfo(findChangelog(root.filePath(
                                   QStringLiteral("tree/build")))).absoluteFilePath()
                         == QFileInfo(path).absoluteFilePath(),
              QStringLiteral("and from one level down"));

        QString error;
        check(!readChangelog(path, &error).isEmpty() && error.isEmpty(),
              QStringLiteral("and reads back"));
        check(readChangelog(root.filePath(QStringLiteral("nothing.md")), &error).isEmpty()
                  && !error.isEmpty(),
              QStringLiteral("a file that is not there says so rather than throwing"));
        check(readChangelog(QString(), &error).isEmpty() && !error.isEmpty(),
              QStringLiteral("so does no path at all"));

        const QString bare = root.filePath(QStringLiteral("bare"));
        QDir().mkpath(bare);
        check(findChangelog(bare).isEmpty() || QFileInfo(findChangelog(bare)).isFile(),
              QStringLiteral("a folder with no changelog above it finds none"));

        // The repository's own CHANGELOG.md, not a fixture. A file the parser
        // cannot read is a start page with no notes on it, and the first anyone
        // would know is a release with an empty panel.
        const QString resources =
            argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("resources");
        const QString repo = findChangelog(QFileInfo(resources).absolutePath());
        if (repo.isEmpty()) {
            line(QStringLiteral("  skip there is no CHANGELOG.md in this tree yet"));
        } else {
            const QVector<ChangelogEntry> real =
                parseChangelog(readChangelog(repo));
            check(!real.isEmpty(),
                  QStringLiteral("the repository's own changelog parses"));
            int versions = 0;
            for (const ChangelogEntry &entry : real)
                if (entry.version.valid) ++versions;
            check(versions > 0, QStringLiteral("and carries at least one release"));
            const ChangelogEntry shipped =
                changelogEntryFor(real, QStringLiteral(NODEMOD_VERSION));
            if (shipped.isValid())
                check(!changelogLines(shipped.body, 4).isEmpty(),
                      QStringLiteral("with notes for the version this build is"));
            else
                line(QStringLiteral("  note the changelog has no section for %1 yet")
                         .arg(QStringLiteral(NODEMOD_VERSION)));
        }
    }

    // ---- reading a list of releases ----------------------------------------
    line(QStringLiteral("\nreading releases"));
    {
        QString error;
        const QVector<Release> list = parseReleases(
            body({release(QStringLiteral("v0.3.0"), false, false,
                          QStringLiteral("### Added\n- A thing.\n"),
                          {QStringLiteral("SUDO-DayZ-Node-Mod-0.3.0-setup.exe"),
                           QStringLiteral("SUDO-DayZ-Node-Mod-0.3.0-win64.zip")}),
                  release(QStringLiteral("v0.2.0"), false, false)}),
            &error);
        check(error.isEmpty(), QStringLiteral("a real payload reads clean"));
        equal(list.size(), 2, QStringLiteral("both releases come back"));
        equal(list.first().version.toString(), QStringLiteral("0.3.0"),
              QStringLiteral("with their tags parsed"));
        equal(list.first().assetName,
              QStringLiteral("SUDO-DayZ-Node-Mod-0.3.0-setup.exe"),
              QStringLiteral("the installer is preferred over the archive"));
        check(list.first().assetSize > 0, QStringLiteral("and carries its size"));
        check(!list.first().pageUrl.isEmpty(),
              QStringLiteral("and the page to open"));

        parseReleases(QByteArray("{ \"message\": \"Not Found\" }"), &error);
        check(!error.isEmpty(), QStringLiteral("an object is not a list of releases"));
        parseReleases(QByteArray("<html>rate limited</html>"), &error);
        check(!error.isEmpty(), QStringLiteral("nor is a page of HTML"));
        parseReleases(QByteArray(), &error);
        check(!error.isEmpty(), QStringLiteral("nor is nothing at all"));

        // One entry nobody can read should cost that entry and not the list.
        const QVector<Release> mixed = parseReleases(
            body({release(QStringLiteral("nightly"), false, false),
                  release(QStringLiteral("v0.4.0"), false, false)}),
            &error);
        check(error.isEmpty() && mixed.size() == 1,
              QStringLiteral("a tag nothing can order is skipped, not fatal"));
    }

    // ---- every way the check can end ---------------------------------------
    line(QStringLiteral("\nwhat a reply means"));
    const QString current = QStringLiteral("0.2.0");
    {
        // Nothing came back. Three different causes, one thing to say.
        statusIs(evaluateFetch(silence(QStringLiteral("Host example.invalid not found")),
                               current),
                 UpdateOutcome::Failed, QStringLiteral("offline"));
        statusIs(evaluateFetch(silence(QStringLiteral("Connection timed out")), current),
                 UpdateOutcome::Failed, QStringLiteral("timed out"));
        const UpdateOutcome quiet = evaluateFetch(silence(QString()), current);
        statusIs(quiet, UpdateOutcome::Failed,
                 QStringLiteral("a failure with no words"));
        check(!quiet.detail.isEmpty(),
              QStringLiteral("still has a reason to put in a tooltip"));
        check(!quiet.hasUpdate(),
              QStringLiteral("and offers nothing to download"));

        statusIs(evaluateFetch(answered(404, QByteArray("{\"message\":\"Not Found\"}")),
                               current),
                 UpdateOutcome::Failed,
                 QStringLiteral("a repository that does not exist yet"));
        statusIs(evaluateFetch(answered(403, QByteArray("{\"message\":\"rate limit\"}")),
                               current),
                 UpdateOutcome::Failed, QStringLiteral("rate limited"));
        statusIs(evaluateFetch(answered(429, QByteArray("{}")), current),
                 UpdateOutcome::Failed, QStringLiteral("rate limited the other way"));
        statusIs(evaluateFetch(answered(500, QByteArray("{}")), current),
                 UpdateOutcome::Failed, QStringLiteral("GitHub having a bad day"));
        statusIs(evaluateFetch(answered(0, QByteArray("[]")), current),
                 UpdateOutcome::Failed, QStringLiteral("a reply with no status"));

        statusIs(evaluateFetch(answered(200, QByteArray("[]")), current),
                 UpdateOutcome::NoReleases,
                 QStringLiteral("a repository with nothing published"));
        statusIs(evaluateFetch(answered(200, QByteArray("not json at all")), current),
                 UpdateOutcome::Failed, QStringLiteral("a reply that is not JSON"));
        statusIs(evaluateFetch(answered(200, QByteArray("{\"x\":1}")), current),
                 UpdateOutcome::Failed, QStringLiteral("a reply that is not a list"));
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("nightly"), false, false)})),
                     current),
                 UpdateOutcome::NoReleases,
                 QStringLiteral("a list of tags nothing can order"));

        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.1.0"), false, false),
                                         release(QStringLiteral("v0.2.0"), false, false)})),
                     current),
                 UpdateOutcome::UpToDate,
                 QStringLiteral("nothing published is newer"));
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.9.0"), true, false)})),
                     current),
                 UpdateOutcome::UpToDate, QStringLiteral("a draft is not a release"));
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.3.0"), false, true)})),
                     current),
                 UpdateOutcome::UpToDate,
                 QStringLiteral("a pre-release is not offered by default"));
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.3.0-rc.1"), false, false)})),
                     current),
                 UpdateOutcome::UpToDate,
                 QStringLiteral("nor is one that only says so in its tag"));
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.3.0"), false, true)})),
                     current, true),
                 UpdateOutcome::UpdateAvailable,
                 QStringLiteral("but it is when they have been asked for"));

        const UpdateOutcome found = evaluateFetch(
            answered(200, body({release(QStringLiteral("v0.2.0"), false, false),
                                release(QStringLiteral("v0.10.0"), false, false,
                                        QStringLiteral("### Fixed\n- The thing.\n"),
                                        {QStringLiteral("setup.exe")}),
                                release(QStringLiteral("v0.9.0"), false, false)})),
            current);
        statusIs(found, UpdateOutcome::UpdateAvailable,
                 QStringLiteral("a newer release is found"));
        equal(found.release.version.toString(), QStringLiteral("0.10.0"),
              QStringLiteral("and it is the newest of them, not the first listed"));
        check(found.hasUpdate() && !found.release.assetName.isEmpty(),
              QStringLiteral("with the download named"));

        // Saying no once has to mean no.
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.3.0"), false, false)})),
                     current, false, QStringLiteral("0.3.0")),
                 UpdateOutcome::UpToDate,
                 QStringLiteral("a version already turned down stays down"));
        statusIs(evaluateFetch(
                     answered(200, body({release(QStringLiteral("v0.4.0"), false, false)})),
                     current, false, QStringLiteral("0.3.0")),
                 UpdateOutcome::UpdateAvailable,
                 QStringLiteral("but what came after it is still reported"));
    }

    // ---- consent, which nothing happens without ----------------------------
    line(QStringLiteral("\nconsent"));
    {
        const QString ini = root.filePath(QStringLiteral("settings/updates.ini"));
        QDir().mkpath(QFileInfo(ini).absolutePath());
        UpdatePreferences prefs(ini);
        check(prefs.consent() == UpdateConsent::NotAsked,
              QStringLiteral("a fresh install has not been asked"));
        check(!prefs.dueForCheck(),
              QStringLiteral("and is not due a check, which is the point"));

        prefs.setConsent(UpdateConsent::Declined);
        check(!prefs.dueForCheck(), QStringLiteral("a no stays a no"));

        prefs.setConsent(UpdateConsent::Allowed);
        check(prefs.dueForCheck(), QStringLiteral("a yes is due one straight away"));

        const QDateTime now = QDateTime::currentDateTimeUtc();
        prefs.setLastChecked(now);
        check(!prefs.dueForCheck(now.addSecs(3600)),
              QStringLiteral("and not again an hour later"));
        check(prefs.dueForCheck(now.addSecs(60 * 60 * 25)),
              QStringLiteral("but is a day later"));
        check(prefs.dueForCheck(now.addSecs(-60 * 60 * 5)),
              QStringLiteral("a clock put back leaves it due rather than never"));

        equal(prefs.owner(), QStringLiteral("DillanStep"),
              QStringLiteral("the owner has a default"));
        check(!prefs.repository().isEmpty(),
              QStringLiteral("and so does the repository"));
        prefs.setRepository(QStringLiteral("someone"), QStringLiteral("their-fork"));
        equal(prefs.owner(), QStringLiteral("someone"),
              QStringLiteral("both are configurable, because the repo does not "
                             "exist yet"));

        prefs.setSkippedVersion(QStringLiteral("0.3.0"));
        prefs.setLastSeenVersion(QStringLiteral("0.1.0"));
        prefs.flush();

        UpdatePreferences reread(ini);
        check(reread.consent() == UpdateConsent::Allowed,
              QStringLiteral("the answer survives a restart"));
        equal(reread.skippedVersion(), QStringLiteral("0.3.0"),
              QStringLiteral("so does a skipped version"));
        equal(reread.lastSeenVersion(), QStringLiteral("0.1.0"),
              QStringLiteral("and the version last shown"));
        equal(reread.repository(), QStringLiteral("their-fork"),
              QStringLiteral("and the repository it was pointed at"));
    }

    // ---- the request, without making one -----------------------------------
    line(QStringLiteral("\nthe request that is not sent"));
    {
        UpdateCheck check1;
        check(!check1.running(), QStringLiteral("nothing is in flight on construction"));
        const QString url = check1.endpoint().toString();
        check(url.startsWith(QStringLiteral("https://")),
              QStringLiteral("the endpoint is over TLS"));
        check(url.contains(QStringLiteral("api.github.com")),
              QStringLiteral("and points at GitHub's API"));
        check(url.contains(QStringLiteral("/DillanStep/")),
              QStringLiteral("with the default owner in it"));
        // Encoded, not interpolated. A settings file is a text file somebody can
        // edit, and a repository name carrying a slash or a question mark would
        // otherwise be aiming the request somewhere else. toEncoded rather than
        // toString, because toString prints a URL for reading and puts the
        // space back.
        check1.setRepository(QStringLiteral("someone"), QStringLiteral("their fork"));
        check(check1.endpoint().toEncoded().contains(QByteArray("their%20fork")),
              QStringLiteral("a repository name with a space is encoded, not injected"));
        check1.setRepository(QStringLiteral("someone"),
                             QStringLiteral("a/../../other?x=1"));
        const QByteArray aimed = check1.endpoint().toEncoded();
        check(aimed.startsWith(QByteArray("https://api.github.com/repos/someone/"))
                  && !aimed.contains(QByteArray("/../")),
              QStringLiteral("and a name full of path and query cannot repoint it"));
        check1.cancel();
        check(!check1.running(),
              QStringLiteral("cancelling a check that never started is harmless"));
    }

    // ---- the page ----------------------------------------------------------
    line(QStringLiteral("\nthe page"));
    const QStringList args = QCoreApplication::arguments();
    const auto argAfter = [&args](const QString &flag) {
        const int at = args.indexOf(flag);
        return at > 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
    };
    {
        theme::apply(app);

        const QString store = root.filePath(QStringLiteral("page/recent.json"));
        const QDateTime now = QDateTime::currentDateTime();
        const QStringList names = {QStringLiteral("SUDO_Link"),
                                   QStringLiteral("Trader Overhaul"),
                                   QStringLiteral("Night Ops"),
                                   QStringLiteral("Old Cache Test")};
        QJsonArray projects;
        for (int i = 0; i < names.size(); ++i) {
            const QString path =
                root.filePath(QStringLiteral("page/%1/%1.sdzn").arg(names.at(i)));
            writeProject(path, names.at(i), i + 1);
            QJsonObject entry;
            entry.insert("path", QDir::cleanPath(path));
            entry.insert("name", names.at(i));
            entry.insert("scripts", i + 1);
            entry.insert("nodes", (i + 1) * 14);
            entry.insert("lastOpened",
                         now.addSecs(-600 * (i + 1)).toUTC().toString(Qt::ISODate));
            projects.append(entry);
        }
        QJsonObject storeRoot;
        storeRoot.insert("version", 1);
        storeRoot.insert("projects", projects);
        QDir().mkpath(QFileInfo(store).absolutePath());
        QFile storeFile(store);
        check(storeFile.open(QIODevice::WriteOnly),
              QStringLiteral("the page's store is written"));
        storeFile.write(QJsonDocument(storeRoot).toJson(QJsonDocument::Indented));
        storeFile.close();

        RecentProjects recent(store);
        check(recent.load(), QStringLiteral("and loads"));

        const QString ini = root.filePath(QStringLiteral("page/updates.ini"));
        QDir().mkpath(QFileInfo(ini).absolutePath());
        {
            UpdatePreferences seed(ini);
            seed.setConsent(UpdateConsent::Allowed);
            seed.setLastSeenVersion(QStringLiteral("0.1.0"));
        }

        auto *page = new StartPage(&recent, nullptr, ini);
        // Laid out and painted, never mapped. A shot run uses the real platform
        // so the glyphs are the ones the app draws, and this is what keeps a
        // window off the screen of whoever is working at the machine.
        page->setAttribute(Qt::WA_DontShowOnScreen, true);
        // Belt and braces: consent is granted above so the panel can be drawn in
        // the state a real user sees, and this is what stops the page acting on
        // it. No run of this test can open a socket.
        page->setAutomaticUpdateCheck(false);
        page->setChangelogPath(root.filePath(QStringLiteral("tree/CHANGELOG.md")));
        check(!page->changelogPath().isEmpty() && page->whatsNew() != nullptr,
              QStringLiteral("the page found the changelog and built the panel"));

        const auto layOut = [&page](int width, int height) {
            page->resize(width, height);
            page->show();
            QApplication::processEvents();
            QApplication::processEvents();
        };

        // ---- no newer release ---------------------------------------------
        UpdateOutcome none =
            evaluateFetch(answered(200, body({release(QStringLiteral("v0.2.0"),
                                                      false, false)})),
                          QStringLiteral("0.2.0"));
        none.checkedAt = QDateTime::currentDateTimeUtc().addSecs(-720);
        page->applyUpdateOutcome(none);
        layOut(1600, 950);

        auto *list = page->findChild<QListWidget *>();
        check(list != nullptr && list->isVisible(),
              QStringLiteral("the recent list is on the page"));
        // Every row drawn, not three of four behind a scroll bar. A scroll area
        // asks for 256 by 192 whatever is in it, which is three rows of this
        // list to the pixel, so the fourth entry is the one that used to go
        // missing in a panel that had room for it.
        check(list->count() == 4
                  && list->visualItemRect(list->item(3)).bottom() <= list->height(),
              QStringLiteral("with every row inside it rather than under a scroll bar"));
        const auto fitsIn = [&page](const QWidget *widget) {
            if (!widget) return false;
            const QPoint bottom =
                widget->mapTo(page, QPoint(0, widget->height()));
            return bottom.y() <= page->height() && bottom.y() > 0;
        };
        check(fitsIn(list),
              QStringLiteral("and ends inside the page at 1600 by 950"));
        check(fitsIn(page->whatsNew()),
              QStringLiteral("with the what is new panel under it, also inside"));

        // The panel sits under the list, not beside it and not over it. Both
        // assertions above say it is inside the page; this one says the order
        // on the page is the order the page was designed in.
        check(page->whatsNew()->mapTo(page, QPoint(0, 0)).y()
                  >= list->mapTo(page, QPoint(0, list->height())).y(),
              QStringLiteral("and starts below the list rather than over it"));

        const QString shots = argAfter(QStringLiteral("--shots"));
        const auto grab = [&page, &shots](const QString &name) {
            if (shots.isEmpty()) return;
            const QString path = QDir(shots).absoluteFilePath(name);
            line((page->grab().save(path) ? QStringLiteral("wrote ")
                                          : QStringLiteral("could not write "))
                 + path);
        };
        grab(QStringLiteral("start-nothing-new-1600x950.png"));

        layOut(1280, 800);
        check(fitsIn(list),
              QStringLiteral("the recent list still ends inside the page at 1280 "
                             "by 800"));
        check(fitsIn(page->whatsNew()),
              QStringLiteral("and so does the panel"));
        grab(QStringLiteral("start-nothing-new-1280x800.png"));

        // ---- a newer release ------------------------------------------------
        UpdateOutcome newer = evaluateFetch(
            answered(200,
                     body({release(QStringLiteral("v0.3.0"), false, false,
                                   QStringLiteral(
                                       "### Added\n"
                                       "- Mod browser opens a `.pbo` straight from "
                                       "disk.\n"
                                       "- Config editor writes `config.cpp` back "
                                       "byte for byte.\n"
                                       "### Fixed\n"
                                       "- Comments no longer disappear on import.\n"),
                                   {QStringLiteral(
                                       "SUDO-DayZ-Node-Mod-0.3.0-setup.exe")})})),
            QStringLiteral("0.2.0"));
        newer.checkedAt = QDateTime::currentDateTimeUtc().addSecs(-120);
        statusIs(newer, UpdateOutcome::UpdateAvailable,
                 QStringLiteral("the fixture is an update"));
        page->applyUpdateOutcome(newer);

        layOut(1600, 950);
        check(fitsIn(list),
              QStringLiteral("with an update to announce the list still fits at "
                             "1600 by 950"));
        check(fitsIn(page->whatsNew()), QStringLiteral("and the panel with it"));
        grab(QStringLiteral("start-update-1600x950.png"));

        layOut(1280, 800);
        check(fitsIn(list),
              QStringLiteral("and at 1280 by 800, which is the tight one"));
        check(fitsIn(page->whatsNew()), QStringLiteral("and the panel with it"));
        grab(QStringLiteral("start-update-1280x800.png"));

        // The panel is bounded whatever it is handed. A release with pages of
        // notes must not be the thing that decides the page's height.
        const int measured = page->whatsNew()->height();
        UpdateOutcome wordy = newer;
        QString essay;
        for (int i = 0; i < 60; ++i)
            essay += QStringLiteral("- Line %1 of a release nobody will read to "
                                    "the end of.\n").arg(i);
        wordy.release.notes = essay;
        page->applyUpdateOutcome(wordy);
        QApplication::processEvents();
        equal(page->whatsNew()->height(), measured,
              QStringLiteral("sixty lines of notes leave the panel the same height"));
        check(fitsIn(list),
              QStringLiteral("so the recent list has not moved"));

        page->applyUpdateOutcome(newer);
        QApplication::processEvents();
        delete page;

        // ---- the state every new install opens in ---------------------------
        //
        // Consent has never been given, so nothing has been asked of GitHub and
        // the panel is the question rather than an answer. A page of its own
        // because the answer is read once, when the page is built.
        {
            const QString fresh = root.filePath(QStringLiteral("first/updates.ini"));
            QDir().mkpath(QFileInfo(fresh).absolutePath());
            auto *first = new StartPage(&recent, nullptr, fresh);
            first->setAttribute(Qt::WA_DontShowOnScreen, true);
            first->setAutomaticUpdateCheck(false);
            first->setChangelogPath(root.filePath(QStringLiteral("tree/CHANGELOG.md")));
            first->resize(1280, 800);
            first->show();
            QApplication::processEvents();
            QApplication::processEvents();

            UpdatePreferences asked(fresh);
            check(asked.consent() == UpdateConsent::NotAsked,
                  QStringLiteral("building the page asks nobody anything"));
            check(!asked.lastChecked().isValid(),
                  QStringLiteral("and records no check, because none was made"));

            auto *firstList = first->findChild<QListWidget *>();
            const QWidget *panel = first->whatsNew();
            check(firstList
                      && firstList->mapTo(first, QPoint(0, firstList->height())).y()
                             <= first->height(),
                  QStringLiteral("the list fits with the question showing"));
            check(panel->mapTo(first, QPoint(0, panel->height())).y()
                      <= first->height(),
                  QStringLiteral("and so does the question"));

            if (!shots.isEmpty()) {
                const QString path = QDir(shots).absoluteFilePath(
                    QStringLiteral("start-first-run-1280x800.png"));
                line((first->grab().save(path) ? QStringLiteral("wrote ")
                                               : QStringLiteral("could not write "))
                     + path);
            }
            delete first;
        }
    }

    line(failures == 0 ? QStringLiteral("\nall good")
                       : QStringLiteral("\n%1 failed").arg(failures));
    return failures == 0 ? 0 : 1;
}
